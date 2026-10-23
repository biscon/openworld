#include "terrain_cache.hpp"
#include "terrain_detail_policy.hpp"
#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
static thread_local bool countAllocations = false;
static thread_local size_t allocations = 0;
void *operator new(std::size_t size) {
    if (countAllocations)
        ++allocations;
    if (auto p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
namespace {
struct Temp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ow-stream-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
void await(ow::TerrainCache &c, int lease) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!c.complete(lease) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(c.complete(lease), "worker completion deadline");
}
void detailPolicy() {
    ow::TerrainDetailPolicy p;
    ow::Camera c;
    c.chunk = {-1000000, 1000000};
    c.local = {64, 800, 64};
    p.update(c, 128, .01);
    auto here = p.rank({c.chunk.x, c.chunk.z, 0}, 128, false, 1000);
    require(here.protectedNear && here.current == 0, "bounds contain camera regardless of altitude");
    auto behind = p.rank({c.chunk.x - 1, c.chunk.z, 0}, 128, false, 1000);
    auto ahead = p.rank({c.chunk.x + 1, c.chunk.z, 0}, 128, false, 1000);
    near(behind.current, ahead.current, 0, "bounds ranking has no tile-origin bias");
    auto retained = p.rank({c.chunk.x + 3, c.chunk.z, 0}, 128, true, 1000);
    auto fresh = p.rank({c.chunk.x + 3, c.chunk.z, 0}, 128, false, 1000);
    near(fresh.priority - retained.priority, 1, 0, "one-chunk residency hysteresis");
    require(ow::TerrainDetailPolicy::before(here, fresh), "current protection precedes prediction");
    for (int i = 0; i < 100; ++i) {
        c.local.x += 40;
        p.update(c, 128, .1);
    }
    require(p.prediction().x > 190 && p.prediction().x <= 256, "400 m/s look-ahead is bounded");
    c.update({}, 0, {256, .5f});
    p.update(c, 128, .001); // Rebasing must not look like a teleport.
    const auto upcoming = p.rank({c.chunk.x + 2, c.chunk.z, 0}, 128, false, 128);
    const auto receding = p.rank({c.chunk.x - 2, c.chunk.z, 0}, 128, false, 128);
    require(upcoming.eligible && !upcoming.protectedNear && upcoming.distance < upcoming.current,
            "prediction may refine already-resident upcoming tiles");
    require(ow::TerrainDetailPolicy::before(upcoming, receding), "upcoming terrain precedes equally distant receding terrain");
    const auto fine = p.rank({c.chunk.x + 3, c.chunk.z, 0}, 128, false, 10000);
    const auto coarse = p.rank({ow::terrainFloorDivide(c.chunk.x + 3, 2), ow::terrainFloorDivide(c.chunk.z, 2), 1}, 128, false, 10000);
    require(coarse.priority >= coarse.distance + 1 && fine.priority == fine.distance,
            "coarser data levels retain distance penalty");
    c.local.x += 500;
    p.update(c, 128, .01);
    near(glm::length(p.prediction()), 256, 1e-8, "fast motion caps at two chunks");
    for (int i = 0; i < 60; ++i) p.update(c, 128, .05);
    require(glm::length(p.prediction()) < .001, "stopping settles prediction");
    for (int i = 0; i < 12; ++i) { c.local.x -= 10; p.update(c, 128, .05); }
    require(p.prediction().x < -90, "reversal predicts opposite direction");
    c.chunk.x += 5;
    p.update(c, 128, .1);
    require(glm::length(p.prediction()) == 0, "teleport resets prediction");
    c.local.x += 50;
    p.update(c, 128, .6);
    require(glm::length(p.prediction()) == 0, "long update gap resets prediction");
    const auto before = p.rank({c.chunk.x, c.chunk.z, 0}, 128, false, 1000);
    c.yaw += 3;
    c.pitch -= 1;
    p.update(c, 128, .1);
    const auto after = p.rank({c.chunk.x, c.chunk.z, 0}, 128, false, 1000);
    near(before.priority, after.priority, 0, "turning does not change detail demand");
    // At 128 m chunks, this tile is 320 m away: outside the old 256 m
    // detail band but inside the expanded 512 m geometry band.
    ow::Camera rangeCamera;
    rangeCamera.local={64,800,64};
    ow::TerrainDetailPolicy rangePolicy;
    rangePolicy.update(rangeCamera,128,.01);
    const auto rangeRank = [&](float scale, bool quality, bool frozen, float dt) {
        return rangePolicy.rank({3,0,0},128,false,
            ow::terrainHierarchyRange(ow::terrainDetailRangeScale(scale,quality,frozen,dt))*4);
    };
    require(!rangeRank(1,true,true,.1).eligible,"baseline excludes distant detail");
    require(rangeRank(2,true,true,.1).eligible,"expanded geometry requests distant detail");
    require(!rangeRank(2,false,false,.1).eligible,"disabled quality restores baseline demand");
    require(rangeRank(1,true,false,.1).eligible,"one-frame expansion requests upcoming detail");
    near(ow::terrainDetailRangeScale(1.8f,true,false,1),2,0,"demand lookahead bounded at maximum scale");
    near(ow::terrainDetailRangeScale(1.2f,true,true,.1f),1.2,1e-6,"freeze suppresses lookahead");
    ow::TerrainDetailRank active{true, false, 4, 4, 4}, candidate{true, false, 2, 2, 2};
    require(!ow::TerrainDetailPolicy::preempt(active, candidate, .099, .5), "minimum work age");
    require(ow::TerrainDetailPolicy::preempt(active, candidate, .1, .749), "twice priority permits preemption");
    require(!ow::TerrainDetailPolicy::preempt(active, candidate, .1, .75), "late work finishes");
    candidate.priority = 2.01;
    require(!ow::TerrainDetailPolicy::preempt(active, candidate, .2, .5), "priority threshold");
    candidate.priority = 2;
    candidate.distance = 3.51;
    require(!ow::TerrainDetailPolicy::preempt(active, candidate, .2, .5), "minimum distance improvement");
    candidate.protectedNear = true;
    require(ow::TerrainDetailPolicy::preempt(active, candidate, .2, .5), "protected request interrupts distant work");
    require(ow::TerrainDetailPolicy::cooling(1, 1.25, true) &&
            !ow::TerrainDetailPolicy::cooling(1.25, 1.25, true) &&
            !ow::TerrainDetailPolicy::cooling(1, 1.25, false), "cooldown expires or yields to idle recovery");
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 10000; ++i) {
        c.local.x += .1;
        p.update(c, 128, .01);
        p.rank({-1000000, 1000000, 0}, 128, false, 1000);
    }
    countAllocations = false;
    require(allocations == 0, "detail policy updates allocate nothing");
}
void cancelledReads(bool queued) {
    Temp temp;
    ow::TerrainPayloadLayout layout({16, .5f}, 2, 8);
    ow::TerrainStreamingConfig config;
    config.cacheDirectory = temp.path;
    struct Gate { std::atomic<int> entered{0}; std::atomic<bool> open{false}; } gate;
    const auto hook = [](void *p) {
        auto &g = *static_cast<Gate *>(p);
        ++g.entered;
        while (!g.open.load()) std::this_thread::yield();
    };
    ow::TerrainCacheHooks hooks;
    hooks.context = &gate;
    if (queued) hooks.beforeClaim = hook; else hooks.beforeIO = hook;
    ow::TerrainCache cache(layout, config, hooks);
    struct Release { Gate &g; ~Release() { g.open = true; } } release{gate};
    ow::TerrainCacheKey key;
    key.product = ow::TerrainProduct::Detail;
    const ow::TerrainHandle h{3, 7};
    const int old = cache.read(key, h, 41);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gate.entered.load() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    require(gate.entered.load() > 0, "worker reached cancellation gate");
    cache.cancelRead(old);
    require(cache.complete(old) == queued, "queued cancellation completes without stealing running mailbox");
    const int newer = cache.read(key, h, 43);
    require(newer >= 0 && newer != old, "fresh revision uses separate mailbox");
    require(cache.acquire() < 0, "cancelled worker-owned lease is not recycled");
    require(cache.handle(old) == h && cache.revision(old) == 41 && cache.revision(newer) == 43,
            "same tile retains distinct request identities");
    gate.open = true;
    await(cache, old);
    await(cache, newer);
    require(cache.result(old) == ow::TerrainCache::Result::Cancelled, "cancelled completion is discarded");
    require(cache.result(newer) == ow::TerrainCache::Result::Miss, "new revision completes normally");
    cache.release(old);
    cache.release(newer);
    const int again = cache.read(key, h, 44);
    await(cache, again);
    require(cache.result(again) == ow::TerrainCache::Result::Miss && cache.revision(again) == 44,
            "cancelled read cannot suppress later request");
    cache.release(again);
}
void poolSizing() {
    constexpr uint64_t mib = ow::terrainMiB, budget = 448 * mib;
    // Owned device-cost fixture: padded layers plus a whole-array allocation overhead.
    const auto coverage = [](uint32_t slots) { return uint64_t(slots) * 576 * 1024; };
    const auto detail = [](uint32_t slots) { return uint64_t(slots) * 8 * mib + mib; };
    const auto pools = ow::terrainPoolCapacity(budget, 1024, true, coverage, detail);
    require(pools.maximumRadius == 8 && pools.coverage == 365, "padding preserves affordable coverage");
    require(coverage(pools.coverage) + detail(pools.detail) <= budget,
            "actual padded allocations fit budget");
    require(coverage(pools.coverage) + detail(pools.detail + 1) > budget,
            "detail consumes remaining affordable capacity");
    const uint32_t rawTexelSlots = uint32_t((budget - coverage(pools.coverage)) / (6 * mib));
    require(coverage(pools.coverage) + detail(rawTexelSlots) > budget,
            "fixture reproduces raw-texel sizing overflow");
    const auto reduced = ow::terrainPoolCapacity(budget, 1024, true,
                                                 [&](uint32_t n) { return coverage(n) * 2; }, detail);
    require(reduced.maximumRadius < pools.maximumRadius && reduced.detail > 0,
            "coverage padding reduces radius while retaining refinement");
    require(coverage(reduced.coverage) * 2 + detail(reduced.detail) <= budget, "reduced pools fit");
    const auto unitCoverage = [](uint32_t n) { return uint64_t(n); };
    const auto unitDetail = [](uint32_t n) { return uint64_t(n) * 7; };
    const auto exact = ow::terrainPoolCapacity(99, 30, true, unitCoverage, unitDetail);
    require(exact.maximumRadius == 1 && exact.coverage == 29 && exact.detail == 10,
            "device layer limit and exact budget boundary");
    require(ow::terrainPoolCapacity(98, 30, true, unitCoverage, unitDetail).detail == 9,
            "one byte below boundary reduces detail capacity");
    require(ow::terrainPoolCapacity(99, 30, false, unitCoverage, {}).detail == 0,
            "root-only representation never queries a detail allocation");
    bool rejected = false;
    try {
        ow::terrainPoolCapacity(25, 30, true, unitCoverage, unitDetail);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "insufficient coverage budget rejected without unsigned underflow");
    rejected = false;
    try {
        ow::terrainPoolCapacity(99, 30, true, unitCoverage, [](uint32_t) { return uint64_t(100); });
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "minimum detail allocation must fit");
}
void scheduler() {
    using S = ow::TerrainStreamer::State;
    ow::TerrainStreamer s(29, 1);
    ow::Camera camera;
    camera.chunk = {-3, 1000000};
    s.demand(camera, 8, 0, 0, 1);
    require(s.radius() == 1, "radius constrained to capacity");
    const auto first = s.next(S::Requested);
    require(first.index != ow::invalidTerrainSlot, "centre requested");
    require(s.slot(first.index).chunk.x == camera.chunk.x && s.slot(first.index).chunk.z == camera.chunk.z,
            "nearest-first demand");
    size_t desired = 0, visible = 0;
    for (const auto &v : s.slots()) {
        desired += v.wanted;
        visible += v.visible;
    }
    require(desired == 25 && visible == 9, "coverage ring distinct from rendered radius");
    for (uint32_t i = 0; i < s.capacity(); ++i)
        if (s.slot(i).wanted) {
            s.slot(i).state = S::Resident;
            s.slot(i).fade = float(i % 7) / 6;
            s.slot(i).lastUse = 7;
        }
    for (uint32_t i = 0; i < s.capacity(); ++i)
        if (s.slot(i).visible) {
            const auto c = s.slot(i).chunk;
            const int right = s.find({c.x + 1, c.z}), bottom = s.find({c.x, c.z + 1});
            float a[16], b[16];
            s.weights(i, a);
            if (right >= 0) {
                s.weights(uint32_t(right), b);
                for (int z = 0; z < 4; ++z)
                    near(a[z * 4 + 3], b[z * 4], 0, "shared horizontal normal weights");
            }
            if (bottom >= 0) {
                s.weights(uint32_t(bottom), b);
                for (int x = 0; x < 4; ++x)
                    near(a[12 + x], b[x], 0, "shared vertical normal weights");
            }
        }
    allocations = 0;
    countAllocations = true;
    for (int i = 0; i < 1000; ++i)
        s.demand(camera, 1, 0, 7, 1);
    countAllocations = false;
    require(allocations == 0, "steady demand allocation-free");
    camera.chunk.x += 100;
    s.demand(camera, 1, 0, 7, 1);
    require(!s.valid(first), "teleport invalidates old handle immediately");
    size_t retiring = 0;
    for (const auto &v : s.slots())
        retiring += v.state == S::Retiring;
    require(retiring == 25, "in-flight slots retained until fence");
    s.demand(camera, 1, 7, 8, 1);
    require(s.next(S::Requested).index != ow::invalidTerrainSlot, "retired capacity recycled");
    require(!s.valid(first), "recycled generation rejects stale completion");
    auto current = s.next(S::Requested);
    s.demand(camera, 1, 8, 8, 2);
    require(!s.valid(current), "world epoch invalidation");
}
void delayedWorkers() {
    Temp temp;
    ow::TerrainPayloadLayout layout({16, .5f}, 2, 8);
    ow::TerrainStreamingConfig config;
    config.cacheDirectory = temp.path;
    struct Gate {
        std::atomic<int> entered{0};
        std::atomic<bool> open{false};
    } gate;
    const auto hook = [](void *p) {
        auto &g = *static_cast<Gate *>(p);
        ++g.entered;
        while (!g.open.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    ow::TerrainCache cache(layout, config, {hook, &gate});
    struct Release {
        Gate &g;
        ~Release() { g.open = true; }
    } release{gate};
    require(cache.read({}, {1, 1}) >= 0 && cache.read({}, {2, 1}) >= 0, "delayed reads queued");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (gate.entered.load() != 2 && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    require(gate.entered.load() == 2, "I/O runs on both workers");
    ow::TerrainStreamer streaming(365, 8);
    ow::Camera camera;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 1000; ++frame) {
        streaming.demand(camera, 8, frame, frame, 1);
        require(cache.acquire() < 0 && !cache.complete(0) && !cache.complete(1),
                "blocked I/O cannot block frame coordination");
    }
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "coordination continues during stalled disk I/O");
    gate.open = true;
    await(cache, 0);
    await(cache, 1);
    cache.release(0);
    cache.release(1);
}
void pruning() {
    Temp temp;
    ow::TerrainPayloadLayout layout({8, .5f}, 2, 8);
    ow::TerrainStreamingConfig config;
    config.cacheDirectory = temp.path;
    config.diskBytes = layout.coverageBytes + 144;
    ow::TerrainCache cache(layout, config);
    std::vector<uint8_t> bytes(size_t(layout.coverageBytes), 0);
    for (int n = 0; n < 3; ++n) {
        ow::TerrainCacheKey key;
        key.request.chunk.x = n;
        const int i = cache.acquire();
        require(i >= 0, "write lease available");
        std::memcpy(cache.data(i), bytes.data(), bytes.size());
        cache.write(i, key);
        await(cache, i);
        cache.release(i);
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    uint64_t size = UINT64_MAX;
    while (size > config.diskBytes && std::chrono::steady_clock::now() < until) {
        size = 0;
        std::error_code ec;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(temp.path))
            if (entry.path().extension() == ".owc") {
                const auto bytes = entry.file_size(ec);
                if (!ec)
                    size += bytes;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(size <= config.diskBytes, "worker maintenance enforces disk budget without further writes");
}
void cache() {
    Temp temp;
    ow::TerrainPayloadLayout layout({16, .5f}, 2, 8);
    ow::TerrainStreamingConfig config;
    config.cacheDirectory = temp.path;
    config.hostBytes = 64 * ow::terrainMiB;
    ow::TerrainCache cache(layout, config);
    ow::TerrainCacheKey key{{{-8, 123456}, {42, 1024, 400, .22f, .3f}}, 123, 456, ow::TerrainProduct::Coverage};
    std::vector<uint8_t> bytes(size_t(layout.coverageBytes), 0), out(bytes.size());
    // Nontrivial owned flat fixture, with conservative bounds and unit +Y normals.
    for (uint64_t i = 0; i < layout.heightsBytes; i += 4) {
        const float value = 7;
        std::memcpy(bytes.data() + i, &value, 4);
    }
    for (uint64_t i = layout.boundsOffset; i < layout.coarseOffset; i += 8) {
        float values[2] = {6, 8};
        std::memcpy(bytes.data() + i, values, 8);
    }
    for (uint64_t i = layout.coarseOffset; i < layout.coverageBytes; i += 8) {
        const uint16_t up = 0x3c00;
        std::memcpy(bytes.data() + i + 2, &up, 2);
    }
    require(layout.valid(key.product, bytes.data(), bytes.size()), "owned coverage fixture valid");
    require(cache.load(key, out.data()) == ow::TerrainCache::Result::Miss, "missing cache regenerates");
    require(cache.save(key, bytes.data()) == ow::TerrainCache::Result::Written, "cache write");
    require(cache.load(key, out.data()) == ow::TerrainCache::Result::Hit && out == bytes, "round-trip exact product");
    for (auto field : {&ow::TerrainSettings::ridgeStrength, &ow::TerrainSettings::baseDetail}) {
        auto changed = key;
        changed.request.settings.*field = .25f;
        require(cache.load(changed, out.data()) == ow::TerrainCache::Result::Miss,
                "Each smoothness setting invalidates cached terrain");
        require(cache.save(changed, bytes.data()) == ow::TerrainCache::Result::Written &&
                    cache.load(changed, out.data()) == ow::TerrainCache::Result::Hit,
                "Nondefault smoothness cache round trip");
    }
    for (auto field : {&ow::TerrainSettings::regionWavelength, &ow::TerrainSettings::erosionWavelength}) {
        auto changed = key;
        changed.request.settings.*field *= 2;
        require(cache.load(changed, out.data()) == ow::TerrainCache::Result::Miss,
                "Each independent wavelength invalidates cached geography");
    }
    {
        std::fstream f(cache.path(key), std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(8);
        const char oldVersion = 2;
        f.write(&oldVersion, 1);
    }
    require(cache.load(key, out.data()) == ow::TerrainCache::Result::Miss, "Old generator format rejected");
    cache.save(key, bytes.data());
    auto different = key;
    ++different.tileLevel;
    require(cache.load(different, out.data()) == ow::TerrainCache::Result::Miss, "tile level identity");
    different = key; ++different.representation;
    require(cache.load(different, out.data()) == ow::TerrainCache::Result::Miss, "representation identity");
    different = key;
    ++different.request.settings.seed;
    require(cache.load(different, out.data()) == ow::TerrainCache::Result::Miss, "settings identity");
    different = key;
    ++different.deviceFingerprint;
    require(cache.load(different, out.data()) == ow::TerrainCache::Result::Miss, "device identity");
    different = key;
    ++different.shaderFingerprint;
    require(cache.load(different, out.data()) == ow::TerrainCache::Result::Miss, "shader identity");
    {
        auto detailKey=key; detailKey.product=ow::TerrainProduct::Detail;
        std::vector<uint8_t> detail(layout.detailBytes,0), loaded(detail.size());
        for(size_t i=0;i<detail.size();i+=8) { const uint16_t up=0x3c00; std::memcpy(detail.data()+i+2,&up,2); }
        require(cache.save(detailKey,detail.data())==ow::TerrainCache::Result::Written,"owned detail cache fixture");
        require(cache.load(detailKey,loaded.data())==ow::TerrainCache::Result::Hit,"detail cache warm hit");
        ++detailKey.shaderFingerprint;
        require(cache.load(detailKey,loaded.data())==ow::TerrainCache::Result::Miss,"normal shader change invalidates detail too");
    }
    {
        std::fstream f(cache.path(key), std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(150);
        char value = 123;
        f.write(&value, 1);
    }
    require(cache.load(key, out.data()) == ow::TerrainCache::Result::Miss, "corrupt payload rejected");
    cache.save(key, bytes.data());
    std::filesystem::resize_file(cache.path(key), 145);
    require(cache.load(key, out.data()) == ow::TerrainCache::Result::Miss, "truncated payload rejected");
    cache.save(key, bytes.data());
    {
        std::fstream f(cache.path(key), std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(8);
        char value = 99;
        f.write(&value, 1);
    }
    require(cache.load(key, out.data()) == ow::TerrainCache::Result::Miss, "unknown format rejected");
    cache.save(key, bytes.data());
    const int a = cache.read(key, {2, 9}), b = cache.read(key, {3, 7});
    require(a >= 0 && b >= 0 && a != b, "two independent worker mailboxes");
    const auto start = std::chrono::steady_clock::now();
    countAllocations = true;
    allocations = 0;
    for (int i = 0; i < 10000; ++i)
        require(cache.acquire() < 0, "exhaustion never steals a lease");
    countAllocations = false;
    require(!allocations, "polling/exhaustion allocation-free");
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "exhausted mailboxes never wait for I/O");
    await(cache, a);
    await(cache, b);
    require(cache.result(a) == ow::TerrainCache::Result::Hit && cache.handle(a) == ow::TerrainHandle{2, 9},
            "completion preserves generation ticket");
    require(std::memcmp(cache.data(a), bytes.data(), bytes.size()) == 0, "worker read payload");
    cache.release(a);
    cache.release(b);
    auto invalid = bytes;
    float nan = INFINITY;
    std::memcpy(invalid.data(), &nan, 4);
    require(!layout.valid(key.product, invalid.data(), invalid.size()), "nonfinite height rejected");
    invalid = bytes;
    float low = 9;
    std::memcpy(invalid.data() + layout.boundsOffset, &low, 4);
    require(!layout.valid(key.product, invalid.data(), invalid.size()), "invalid bounds rejected");
    const auto file = temp.path / "not-a-directory";
    {
        std::ofstream f(file);
        f << "fixture";
    }
    config.cacheDirectory = file;
    ow::TerrainCache broken(layout, config);
    require(broken.save(key, bytes.data()) == ow::TerrainCache::Result::Error, "write failure is recoverable");
    // Outstanding worker reads are owned by the cache through destruction.
    require(broken.read(key, {1, 1}) >= 0, "shutdown with queued work");
}
} // namespace
void filteredPayload() {
    ow::TerrainPayloadLayout layout({32, .5f}, 2, 8, true);
    std::vector<uint8_t> data(layout.coverageBytes, 0);
    require(layout.valid(ow::TerrainProduct::Coverage, data.data(), data.size()),
            "owned flat filtered payload valid");
    require(layout.heightOffsets[1] == 33 * 33 * 8 && layout.heightOffsets[2] == (33 * 33 + 17 * 17) * 8,
            "height level addressing retains endpoints");
    float negative = -1;
    std::memcpy(data.data() + layout.boundsOffset + layout.errorOffset + 8, &negative, 4);
    require(!layout.valid(ow::TerrainProduct::Coverage, data.data(), data.size()),
            "negative approximation error rejected");
    negative = 0;
    std::memcpy(data.data() + layout.boundsOffset + layout.errorOffset + 8, &negative, 4);
    require(!layout.valid(ow::TerrainProduct::Coverage, data.data(), data.size() - 8),
            "truncated filtered payload rejected");
}
int main() {
    try {
        filteredPayload();
        detailPolicy();
        cancelledReads(true);
        cancelledReads(false);
        poolSizing();
        scheduler();
        cache();
        delayedWorkers();
        pruning();
        std::cout << "Terrain streaming CPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

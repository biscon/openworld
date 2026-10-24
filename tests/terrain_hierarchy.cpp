#include "terrain_stream.hpp"
#include "test_support.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
using namespace ow;
static std::atomic<uint64_t> allocations{0};
void *operator new(std::size_t n) {
    ++allocations;
    if (void *p = std::malloc(n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
namespace {
void publish(TerrainStreamer &s) {
    for (uint32_t i = 0; i < s.capacity(); ++i)
        if (s.slot(i).state == TerrainStreamer::State::Requested)
            s.slot(i).state = TerrainStreamer::State::Resident;
}
void horizon() {
    TerrainGrid grid{32, .5f};
    const auto base = terrainHierarchyCapacity(8);
    TerrainStreamer stream(base + 48 + 16, 8, grid, true, 16, 48);
    Camera camera;
    camera.chunk = {-1, 1000000};
    camera.local = {8, 30, 8};
    for (int step = 0; step < 30; ++step) {
        stream.demand(camera, 8, step, step, 1);
        publish(stream);
        stream.advance(.25f);
    }
    require(stream.outerRadius() == 16 && stream.radius() == 8,
            "Independent detailed and outer coverage");
    require(stream.topLevel() == terrainHierarchyLevel(8), "Horizon preserves shadow caster level");
    require(stream.pressureEvents() == 0, "Horizon fits reserved storage");
    uint32_t outer = 0;
    for (uint32_t i = 0; i < stream.capacity(); ++i) {
        const auto &slot = stream.slot(i);
        if (!slot.horizonOnly || !slot.wanted)
            continue;
        ++outer;
        require(slot.level == stream.topLevel(), "Horizon requests only coarse tiles");
        require(i >= base && i < base + 48, "Horizon does not displace near or shadow slots");
        require(slot.active && slot.drawMask == 15,
                "Resident horizon provides full coarse coverage");
    }
    require(outer > 0, "Horizon extends terrain");
    const auto before = allocations.load();
    for (int i = 0; i < 100; ++i) {
        stream.demand(camera, 8, 30, 30, 1);
        stream.advance(.016f);
    }
    require(allocations.load() == before, "Horizon demand allocates nothing");
    for (int x : {-17, 0, 15, 32}) {
        camera.chunk.x = x;
        for (int i = 0; i < 40; ++i) {
            stream.demand(camera, 8, 100, 100, 1);
            publish(stream);
            stream.advance(.25f);
        }
        const int centre = stream.find(camera.chunk);
        require(centre >= 0 && stream.slot(centre).active, "Horizon flight retains near detail");
    }
    for (int i = 0; i < 40; ++i) {
        stream.demand(camera, 1, 100, 100, 1);
        publish(stream);
        stream.advance(.25f);
    }
    require(stream.outerRadius() == 2, "Outer range follows detailed radius changes");
}
void distantHierarchy() {
    TerrainGrid grid{256, .5f};
    const auto base = terrainHierarchyCapacity(128), distant = 48u * 6;
    TerrainStreamer stream(base + distant + 16, 128, grid, true, 16, distant, 8192);
    Camera camera;
    require(stream.detailedLevel() == 7 && stream.topLevel() == 13,
            "Independent shadow and distant levels");
    for (int x : {-8193, -1, 0, 127, 8191}) {
        camera.chunk = {x, -x - 1};
        for (int frame = 0; frame < 80; ++frame) {
            stream.demand(camera, 64, 100, 100, 1);
            publish(stream);
            stream.advance(.25f);
        }
        const int centre = stream.find(camera.chunk);
        require(centre >= 0 && stream.slot(centre).active,
                "Near terrain remains active under distant ancestors");
        require(stream.outerRadius() == 8192 && stream.radius() == 64,
                "Distant range independent of detail radius");
        for (uint32_t i = 0; i < stream.capacity(); ++i) {
            const auto &tile = stream.slot(i);
            if (tile.wanted && tile.horizonOnly)
                require(i >= base && i < base + distant && tile.level > 7,
                        "Distant pool cannot consume near or shadow slots");
        }
    }
    TerrainStreamer small(terrainHierarchyCapacity(8) + 48 * 13, 8, {32, .5f}, true, 0, 48 * 13,
                          65536);
    for (int i = 0; i < 40; ++i) {
        small.demand(camera, 8, 100, 100, 1);
        publish(small);
        small.advance(.25f);
    }
    require(small.topLevel() == 16 && small.slot(small.find(camera.chunk)).active,
            "Small chunks support distant data levels beyond the per-tile LOD count");
    const auto before = allocations.load();
    for (int i = 0; i < 30; ++i) {
        stream.demand(camera, 32, 100, 100, 1);
        stream.advance(.016f);
    }
    require(allocations.load() == before, "Extended hierarchy demand has no allocations");
}
void curvature() {
    glm::vec4 mapping{6360000, 2000, 1, 16384};
    near(glm::length(curvePosition(glm::dvec3(0), mapping)), 0, 1e-9, "Camera is curvature origin");
    const auto p = curvePosition({100000, -2000, 0}, mapping);
    near(glm::length(p + glm::dvec3(0, 6362000, 0)), 6360000, 1e-7,
         "Zero-altitude terrain lies on shared sphere");
    for (float x : {-1000000.f, -100000.f, -10.f, 0.f, 20000.f, 500000.f}) {
        glm::vec3 lo{x, -4000, -80000}, hi{x + 32000, 2000, 64000}, a = lo, b = hi;
        curveBounds(a, b, mapping);
        for (int i = 0; i <= 8; ++i)
            for (int j = 0; j <= 8; ++j)
                for (int k = 0; k <= 2; ++k) {
                    const auto q = curvePosition(
                        glm::dvec3(glm::mix(lo, hi, glm::vec3(i / 8.f, k / 2.f, j / 8.f))),
                        mapping);
                    require(glm::all(glm::greaterThanEqual(q, glm::dvec3(a))) &&
                                glm::all(glm::lessThanEqual(q, glm::dvec3(b))),
                            "Curved bounds contain interior and boundary samples");
                }
    }
    const auto distance = horizonCoverage(6360000, -4000, 4000);
    require(distance > 500000 && distance < 1048576,
            "Horizon covers low flight and supported elevation bounds");
    Camera c;
    c.renderFar = 1572864;
    auto q = c.viewProjection(1) * glm::vec4(0, 0, -c.renderFar, 1);
    near(q.z / q.w, 0, 1e-7, "Extended reversed-Z far endpoint");
}
void hierarchy() {
    for (int64_t v = -100; v < 100; ++v) {
        const auto p = terrainParent({v, v, 0});
        require(p.x * 2 <= v && p.x * 2 + 2 > v, "signed parent covers child");
    }
    TerrainGrid grid{256, .5f};
    TerrainStreamer s(terrainHierarchyCapacity(128), 128, grid, true);
    Camera camera;
    camera.chunk = {-1, 1000000};
    camera.local = {64, 200, 64};
    s.demand(camera, 128, 0, 0, 1);
    require(s.demandCount() < s.capacity(), "bounded 16 km hierarchy fits reserved capacity");
    // Ancestors precede refinement even when a fine tile is closer.
    auto next = s.next(TerrainStreamer::State::Requested);
    require(s.slot(next.index).level == s.topLevel(), "ancestor-first coverage");
    publish(s);
    for (int i = 0; i < 12; ++i)
        s.advance(.25f);
    int centre = s.find(camera.chunk);
    require(centre >= 0 && s.slot(centre).active && s.slot(centre).heightFade == 1, "near terrain reaches finest tile");
    for (uint32_t i = 0; i < s.capacity(); ++i) {
        const auto &slot = s.slot(i);
        if (!slot.active || slot.level == s.topLevel())
            continue;
        const int p = s.findTile(terrainParent(slot.tile()));
        require(p >= 0 && s.slot(p).active, "visible child retains parent");
        const uint32_t q = uint32_t(slot.chunk.x - terrainFloorDivide(slot.chunk.x, 2) * 2) +
                           2 * uint32_t(slot.chunk.z - terrainFloorDivide(slot.chunk.z, 2) * 2);
        require(!(s.slot(p).drawMask & (1u << q)), "child replaces exactly its parent quadrant");
        const int neighbour = s.findTile({slot.chunk.x + 1, slot.chunk.z, slot.level});
        if (neighbour >= 0 && s.slot(neighbour).active) {
            float a[16], b[16];
            s.heightWeights(i, a);
            s.heightWeights(neighbour, b);
            for (int z = 0; z < 4; ++z)
                require(a[z * 4 + 3] == b[z * 4], "shared transition edge weights");
        }
    }
    const auto before = allocations.load();
    for (int i = 0; i < 1000; ++i) {
        s.demand(camera, 128, 0, 0, 1);
        s.advance(.016f);
    }
    require(allocations.load() == before, "steady hierarchy demand/transition allocates nothing");
    auto stale = TerrainHandle{uint32_t(centre), s.slot(centre).generation};
    // Walking away fades descendants before retiring their ancestors.
    camera.chunk.x += 20;
    s.demand(camera, 128, 0, 10, 1);
    require(s.valid(stale), "moving demand retains active data during fade");
    for (int i = 0; i < 50; ++i) {
        publish(s);
        s.advance(.1f);
        s.demand(camera, 128, 10, 10, 1);
    }
    require(!s.valid(stale), "obsolete detail eventually retires");
    s.demand(camera, 128, 10, 20, 2);
    for (const auto &slot : s.slots())
        require(!slot.active, "epoch change immediately hides old world");
    publish(s);
    for (int i = 0; i < 12; ++i)
        s.advance(.25f);
    // Every radius/position combination remains bounded, including root
    // boundaries.
    for (int radius : {0, 1, 8, 64, 128})
        for (int x = -2; x <= 2; ++x) {
            camera.chunk = {128 * x - 1, -128 * x};
            for (int i = 0; i < 40; ++i) {
                s.demand(camera, radius, 100, 100, 2);
                publish(s);
                s.advance(.25f);
            }
            require(s.demandCount() <= s.capacity(), "demand never grows past capacity");
            require(s.pressureEvents() == 0, "steady hierarchy fits pool at root crossings");
        }
}
void selection() {
    const TerrainGrid grid{32, .5f};
    TerrainStreamer stream(terrainHierarchyCapacity(8), 8, grid, true);
    TerrainLod lod(grid, 32, {true, 8, 64}, stream.capacity());
    Camera camera;
    camera.chunk = {-2, 1};
    camera.local = {8, 30, 8};
    camera.pitch = -1.5f;
    std::vector<glm::vec2> bounds(16, glm::vec2(0));
    for (uint32_t i = 0; i < stream.capacity(); ++i)
        lod.setBounds(i, bounds.data());
    struct Rect {
        int64_t x, z, endX, endZ;
        uint32_t level;
    };
    std::vector<Rect> rects;
    for (int frame = 0; frame < 32; ++frame) {
        stream.demand(camera, 8, frame, frame, 1);
        // Deliberately delay alternating fine products while ancestors settle.
        for (uint32_t i = 0; i < stream.capacity(); ++i)
            if (stream.slot(i).state == TerrainStreamer::State::Requested &&
                (stream.slot(i).level > 0 || frame > 16 || i % 2 == 0))
                stream.slot(i).state = TerrainStreamer::State::Resident;
        stream.advance(.04f);
        lod.beginHierarchy(camera, camera, camera.viewProjection(1), 8);
        for (uint32_t i = 0; i < stream.capacity(); ++i) {
            auto &s = stream.slot(i);
            if (!s.active)
                continue;
            const int64_t scale = int64_t(1) << s.level;
            lod.selectTile(i, {s.chunk.x * scale, s.chunk.z * scale}, s.level, s.drawMask, true, false);
        }
        rects.clear();
        for (auto patch : lod.draws()) {
            const auto &s = stream.slot(patch.slot);
            const int64_t scale = int64_t(1) << s.level;
            const int64_t size = 8 * patch.step * scale / 2;
            for (uint32_t q = 0; q < 4; ++q)
                if (patch.mask & (1u << q)) {
                    auto x = s.chunk.x * grid.cells * scale + patch.x * scale + (q & 1) * size;
                    auto z = s.chunk.z * grid.cells * scale + patch.z * scale + (q >> 1) * size;
                    rects.push_back({x, z, x + size, z + size, patch.level + s.level});
                }
        }
        for (size_t i = 0; i < rects.size(); ++i)
            for (size_t j = i + 1; j < rects.size(); ++j) {
                const auto &a = rects[i], &b = rects[j];
                bool x = std::max(a.x, b.x) < std::min(a.endX, b.endX);
                bool z = std::max(a.z, b.z) < std::min(a.endZ, b.endZ);
                require(!(x && z), "parent/child selection never overlaps");
                bool adjacent = (x && (a.z == b.endZ || b.z == a.endZ)) || (z && (a.x == b.endX || b.x == a.endX));
                if (adjacent)
                    require(std::abs(int(a.level) - int(b.level)) <= 1,
                            "selection remains 2:1 balanced under delayed data");
            }
    }
}
void refinementFollowsCamera() {
    const TerrainGrid grid{256, .5f};
    TerrainStreamer stream(terrainHierarchyCapacity(128), 128, grid, true);
    TerrainLod lod(grid, grid.cells, {true, 32, 64}, stream.capacity());
    std::vector<glm::vec2> bounds(lod.leafCount(), glm::vec2(0));
    for (uint32_t i = 0; i < stream.capacity(); ++i)
        lod.setBounds(i, bounds.data());
    Camera camera;
    camera.local = {64, 200, 64};
    // Fully publish every request: lack of refinement here cannot be I/O latency.
    for (int radius : {64, 128, 8, 1, 0})
        for (auto position : {ChunkCoord{0, 0}, ChunkCoord{0, 2}, ChunkCoord{2, -6},
                              ChunkCoord{10, 10}, ChunkCoord{-13, 19}, ChunkCoord{127, -128},
                              ChunkCoord{129, 255}, ChunkCoord{1000002, -1000006}}) {
            camera.chunk = position;
            for (int frame = 0; frame < 64; ++frame) {
                stream.demand(camera, radius, 100, 100, 1);
                publish(stream);
                stream.advance(.25f);
            }
            const int centre = stream.find(position);
            require(centre >= 0 && stream.slot(centre).active && stream.slot(centre).heightFade == 1 &&
                        stream.slot(centre).drawMask == 15,
                    "near-camera tile reaches full refinement after movement at every radius");
            require(stream.pressureEvents() == 0, "refinement dependencies fit bounded storage");
            float weights[16];
            stream.heightWeights(uint32_t(centre), weights);
            for (float weight : weights)
                require(weight == 1, "near-camera borders also finish refining");
            lod.beginHierarchy(camera, camera, camera.viewProjection(1), radius);
            for (uint32_t i = 0; i < stream.capacity(); ++i) {
                const auto &tile = stream.slot(i);
                if (!tile.active)
                    continue;
                const auto scale = int64_t(1) << tile.level;
                lod.selectTile(i, {tile.chunk.x * scale, tile.chunk.z * scale}, tile.level, tile.drawMask,
                               true, false);
            }
            bool fineUnderCamera = false;
            for (const auto &patch : lod.draws())
                if (patch.slot == uint32_t(centre) && patch.level == 0) {
                    const double x = patch.x * grid.spacing, z = patch.z * grid.spacing;
                    const double side = lod.patchCells() * grid.spacing;
                    fineUnderCamera |= x <= camera.local.x && camera.local.x < x + side &&
                                       z <= camera.local.z && camera.local.z < z + side;
                }
            require(fineUnderCamera, "LOD selection uses 0.5 m geometry beneath the moved camera");
        }
}
void budgets() {
    auto coverage = [](uint32_t n) { return uint64_t(n) * 600000 + 4096; };
    auto detail = [](uint32_t n) { return uint64_t(n) * 6000000 + 8192; };
    auto p = terrainHierarchyPoolCapacity(440 * terrainMiB, 1024, 128, true, coverage, detail);
    require(p.maximumRadius == 128 && p.detail >= 4, "16 km coverage and near normals fit default budget");
    require(coverage(p.coverage) + detail(p.detail) <= 440 * terrainMiB, "actual padded pool costs respected");
    p = terrainHierarchyPoolCapacity(100 * terrainMiB, 1024, 128, true, coverage, detail);
    require(p.maximumRadius < 128, "insufficient budget reduces effective distance");
}
void stationaryPublication() {
    const TerrainGrid grid{32, .5f};
    TerrainStreamer stream(terrainHierarchyCapacity(8), 8, grid, true);
    Camera camera;
    camera.local = {8, 20, 8};
    for (int frame = 0; frame < 40; ++frame) {
        stream.demand(camera, 8, frame, frame, 1);
        publish(stream);
        stream.advance(.25f);
    }
    const int index = stream.find(camera.chunk);
    require(index >= 0 && stream.slot(index).active, "Publication fixture settles fine ground");
    auto &child = stream.slot(index);
    child.active = child.visible = child.refine = false;
    child.heightFade = .5f;
    stream.advance(.025f);
    require(!child.active, "Cancelling incomplete readiness never publishes a child");
    child.refine = true;
    stream.advance(.05f);
    require(!child.active, "Partial readiness retains parent coverage");
    stream.advance(.1f);
    require(child.active, "Complete readiness publishes the child");
    child.refine = false;
    stream.advance(.05f);
    require(child.active, "Departing child remains visible until retirement is ready");
    child.refine = true;
    stream.advance(.025f);
    require(child.active, "Re-entering demand cannot withdraw an already visible child");
    child.refine = false;
    stream.advance(.25f);
    require(!child.active, "Completed withdrawal restores parent coverage");
    const int parent = stream.findTile(terrainParent(child.tile()));
    const auto quadrant = uint32_t(child.chunk.x - terrainFloorDivide(child.chunk.x, 2) * 2) +
                          2 * uint32_t(child.chunk.z - terrainFloorDivide(child.chunk.z, 2) * 2);
    require(parent >= 0 && (stream.slot(parent).drawMask & (1u << quadrant)),
            "Withdrawn child leaves complete parent coverage");
}
void depth() {
    Camera c;
    c.pitch = c.yaw = 0;
    const auto p = c.viewProjection(1);
    auto near = p * glm::vec4(0, 0, -.5f, 1), far = p * glm::vec4(0, 0, -64000, 1);
    require(std::abs(near.z / near.w - 1) < 1e-5f && std::abs(far.z / far.w) < 1e-5f, "reverse depth endpoints");
}
} // namespace
int main() {
    try {
        horizon();
        distantHierarchy();
        curvature();
        hierarchy();
        selection();
        refinementFollowsCamera();
        stationaryPublication();
        budgets();
        depth();
        std::cout << "Terrain hierarchy CPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

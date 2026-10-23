#include "terrain_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
namespace ow {
uint64_t terrainChecksum(const void *data, size_t bytes, uint64_t seed) {
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i)
        seed = (seed ^ p[i]) * 1099511628211ull;
    return seed;
}
namespace {
void put(uint8_t *p, uint64_t value, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        p[i] = uint8_t(value >> (i * 8));
}
uint64_t get(const uint8_t *p, uint32_t n) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
} // namespace
std::array<uint8_t, 128> TerrainCache::header(const TerrainCacheKey &k) const {
    std::array<uint8_t, 128> out{};
    auto *p = out.data();
    auto u = [&](uint64_t v, uint32_t n = 4) {
        put(p, v, n);
        p += n;
    };
    auto f = [&](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u(bits);
    };
    u(0x4843414354574f31ull, 8);
    u(3);
    u(k.representation); // file format and generator representation versions
    u(uint32_t(k.product));
    u(layout_.grid.cells);
    f(layout_.grid.spacing);
    u(layout_.subdivisions);
    u(layout_.patchCells);
    u(layout_.levels);
    u(uint64_t(k.request.chunk.x), 8);
    u(uint64_t(k.request.chunk.z), 8);
    u(k.request.settings.seed);
    f(k.request.settings.wavelength);
    f(k.request.settings.heightScale);
    f(k.request.settings.erosionStrength);
    f(k.request.settings.creaseRounding);
    f(k.request.settings.regionWavelength);
    f(k.request.settings.erosionWavelength);
    u(k.shaderFingerprint, 8);
    u(k.deviceFingerprint, 8);
    u(layout_.size(k.product), 8);
    u(k.tileLevel);
    f(k.request.settings.ridgeStrength);
    f(k.request.settings.baseDetail);
    return out;
}
std::filesystem::path TerrainCache::path(const TerrainCacheKey &k) const {
    auto h = header(k);
    const auto hash = terrainChecksum(h.data(), h.size());
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << hash;
    const auto name = s.str();
    return config_.cacheDirectory / name.substr(0, 2) / (name + ".owc");
}
TerrainCache::TerrainCache(TerrainPayloadLayout layout, const TerrainStreamingConfig &config, TerrainCacheHooks hooks)
    : layout_(layout), config_(config), hooks_(hooks) {
    if (config_.cacheDirectory.empty())
        config_.cacheDirectory = terrainDefaultCacheDirectory();
    if (config_.cacheDirectory.empty())
        config_.diskCache = false;
    const auto bytes = std::max(layout.coverageBytes, layout.detailBytes);
    if (bytes * 2 + 32 * terrainMiB > config.hostBytes)
        throw std::invalid_argument("Host terrain budget too small for two cache "
                                    "buffers and transfer storage");
    entries_.reserve(1024);
    try {
        for (auto &b : boxes_)
            b.bytes.resize(size_t(bytes));
        for (int i = 0; i < 2; ++i)
            boxes_[i].thread = std::thread([this, i] { worker(i); });
    } catch (...) {
        stopping_ = true;
        for (auto &b : boxes_) {
            b.wake.notify_one();
            if (b.thread.joinable())
                b.thread.join();
        }
        throw;
    }
}
TerrainCache::~TerrainCache() {
    stopping_.store(true);
    for (auto &b : boxes_) {
        std::lock_guard<std::mutex> lock(b.mutex);
        b.wake.notify_one();
    }
    for (auto &b : boxes_)
        if (b.thread.joinable())
            b.thread.join();
}
int TerrainCache::acquire() {
    // Only the render thread can claim Idle; workers only consume Queued boxes.
    // Queued's release store publishes the payload, so leasing needs no release
    // operation.
    for (int i = 0; i < 2; ++i) {
        if (boxes_[i].state.load(std::memory_order_acquire) == State::Idle) {
            boxes_[i].state.store(State::Leased, std::memory_order_relaxed);
            return i;
        }
    }
    return -1;
}
int TerrainCache::read(TerrainCacheKey key, TerrainHandle h, uint64_t revision) {
    int i = acquire();
    if (i < 0)
        return -1;
    auto &b = boxes_[i];
    b.key = key;
    b.handle = h;
    b.revision = revision;
    b.cancelled.store(false, std::memory_order_relaxed);
    b.writing = false;
    b.state.store(State::Queued, std::memory_order_release);
    b.wake.notify_one();
    return i;
}
void TerrainCache::cancelRead(int i) {
    auto &b = boxes_[i];
    b.cancelled.store(true, std::memory_order_relaxed);
    auto state = State::Queued;
    if (b.state.compare_exchange_strong(state, State::Leased, std::memory_order_acq_rel)) {
        b.result = Result::Cancelled;
        b.state.store(State::Complete, std::memory_order_release);
    }
}
bool TerrainCache::complete(int i) const { return boxes_[i].state.load(std::memory_order_acquire) == State::Complete; }
uint32_t TerrainCache::pending() const {
    uint32_t count = 0;
    for (const auto &b : boxes_)
        count += b.state.load(std::memory_order_acquire) != State::Idle;
    return count;
}
void TerrainCache::write(int i, TerrainCacheKey key) {
    auto &b = boxes_[i];
    b.key = key;
    b.handle = {};
    b.writing = true;
    b.cancelled.store(false, std::memory_order_relaxed);
    b.state.store(State::Queued, std::memory_order_release);
    b.wake.notify_one();
}
void TerrainCache::release(int i) { boxes_[i].state.store(State::Idle, std::memory_order_release); }
void TerrainCache::worker(int i) {
    auto &b = boxes_[i];
    while (!stopping_.load()) {
        // Timed wait also closes the notify-before-wait window without a
        // render-thread lock.
        std::unique_lock<std::mutex> lock(b.mutex);
        b.wake.wait_for(lock, std::chrono::milliseconds(5),
                        [&] { return stopping_.load() || b.state.load(std::memory_order_acquire) == State::Queued; });
        lock.unlock();
        if (stopping_.load())
            break;
        if (hooks_.beforeClaim)
            hooks_.beforeClaim(hooks_.context);
        auto state = State::Queued;
        if (!b.state.compare_exchange_strong(state, State::Running, std::memory_order_acq_rel)) {
            try {
                prune();
            } catch (...) {
            }
            continue;
        }
        try {
            if (hooks_.beforeIO)
                hooks_.beforeIO(hooks_.context);
            b.result = !b.writing && b.cancelled.load(std::memory_order_relaxed) ? Result::Cancelled
                       : b.writing ? save(b.key, b.bytes.data()) : load(b.key, b.bytes.data());
            if (!b.writing && b.cancelled.load(std::memory_order_relaxed))
                b.result = Result::Cancelled;
        } catch (...) {
            b.result = Result::Error;
        }
        const bool maintain = b.writing && b.result == Result::Written;
        b.state.store(State::Complete, std::memory_order_release);
        if (maintain) {
            try {
                prune();
            } catch (...) {
            }
        }
    }
}
TerrainCache::Result TerrainCache::load(const TerrainCacheKey &k, uint8_t *out) {
    if (!config_.diskCache)
        return Result::Miss;
    const auto filename = path(k);
    std::error_code ec;
    const auto length = std::filesystem::file_size(filename, ec);
    if (ec)
        return ec == std::errc::no_such_file_or_directory ? Result::Miss : Result::Error;
    const auto bytes = layout_.size(k.product);
    if (length != 144 + bytes)
        return Result::Miss;
    std::ifstream file(filename, std::ios::binary);
    if (!file)
        return Result::Error;
    std::array<uint8_t, 144> h{};
    if (!file.read(reinterpret_cast<char *>(h.data()), h.size()))
        return Result::Miss;
    const auto expected = header(k);
    if (!std::equal(expected.begin(), expected.end(), h.begin()))
        return Result::Miss;
    for (uint64_t offset = 0; offset < bytes;) {
        if (stopping_.load())
            return Result::Miss;
        const auto n = std::min<uint64_t>(terrainMiB, bytes - offset);
        if (!file.read(reinterpret_cast<char *>(out + offset), std::streamsize(n)))
            return Result::Miss;
        offset += n;
    }
    if (get(h.data() + 128, 8) != terrainChecksum(out, size_t(bytes)) ||
        get(h.data() + 136, 8) != terrainChecksum(h.data(), 136) || !layout_.valid(k.product, out, size_t(bytes)))
        return Result::Miss;
    std::filesystem::last_write_time(filename, std::filesystem::file_time_type::clock::now(), ec);
    return Result::Hit;
}
TerrainCache::Result TerrainCache::save(const TerrainCacheKey &k, const uint8_t *data) {
    if (!config_.diskCache)
        return Result::Written;
    const auto filename = path(k);
    const auto bytes = layout_.size(k.product);
    if (bytes + 144 > config_.diskBytes)
        return Result::Written;
    std::error_code ec;
    std::filesystem::create_directories(filename.parent_path(), ec);
    if (ec)
        return Result::Error;
    const auto nonce = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                       uint64_t(reinterpret_cast<uintptr_t>(this));
    auto temp = filename;
    temp += ".tmp-" + std::to_string(nonce) + "-" + std::to_string(serial_.fetch_add(1));
    struct Remove {
        std::filesystem::path p;
        ~Remove() {
            std::error_code e;
            std::filesystem::remove(p, e);
        }
    } cleanup{temp};
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file)
        return Result::Error;
    std::array<uint8_t, 144> h{};
    const auto key = header(k);
    std::copy(key.begin(), key.end(), h.begin());
    put(h.data() + 128, terrainChecksum(data, size_t(bytes)), 8);
    put(h.data() + 136, terrainChecksum(h.data(), 136), 8);
    file.write(reinterpret_cast<const char *>(h.data()), h.size());
    for (uint64_t offset = 0; offset < bytes;) {
        if (stopping_.load())
            return Result::Error;
        const auto n = std::min<uint64_t>(terrainMiB, bytes - offset);
        file.write(reinterpret_cast<const char *>(data + offset), std::streamsize(n));
        offset += n;
    }
    file.flush();
    if (!file)
        return Result::Error;
    file.close();
    if (!file)
        return Result::Error;
    std::filesystem::rename(temp, filename, ec);
    // On platforms that don't replace existing files, a valid existing entry is
    // sufficient.
    if (ec && std::filesystem::exists(filename))
        return Result::Written;
    return ec ? Result::Error : Result::Written;
}
void TerrainCache::prune() {
    if (!config_.diskCache)
        return;
    std::unique_lock<std::mutex> lock(maintenanceMutex_, std::try_to_lock);
    if (!lock)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!scanning_ && now < nextMaintenance_)
        return;
    std::error_code ec;
    if (!scanning_) {
        entries_.clear();
        scannedBytes_ = 0;
        scan_ = std::filesystem::recursive_directory_iterator(config_.cacheDirectory, ec);
        nextMaintenance_ = now + std::chrono::seconds(1);
        if (ec)
            return;
        scanning_ = true;
    }
    const auto older = [](const Candidate &a, const Candidate &b) { return a.time < b.time; };
    unsigned visited = 0;
    while (scan_ != std::filesystem::recursive_directory_iterator{} && visited++ < 128) {
        const auto p = scan_->path();
        if (scan_.depth() >= 1)
            scan_.disable_recursion_pending();
        const auto stem = p.stem().string();
        if (scan_.depth() == 1 && p.extension() == ".owc" && stem.size() == 16 &&
            stem.find_first_not_of("0123456789abcdef") == std::string::npos &&
            p.parent_path().filename() == stem.substr(0, 2) && scan_->is_regular_file(ec)) {
            const auto n = scan_->file_size(ec);
            if (!ec) {
                const auto time = scan_->last_write_time(ec);
                if (!ec) {
                    scannedBytes_ += n;
                    Candidate candidate{};
                    std::copy(stem.begin(), stem.end(), candidate.name.begin());
                    candidate.time = time;
                    candidate.bytes = n;
                    if (entries_.size() < 1024) {
                        entries_.push_back(candidate);
                        std::push_heap(entries_.begin(), entries_.end(), older);
                    } else if (time < entries_.front().time) {
                        std::pop_heap(entries_.begin(), entries_.end(), older);
                        entries_.back() = candidate;
                        std::push_heap(entries_.begin(), entries_.end(), older);
                    }
                }
            }
        }
        scan_.increment(ec);
        if (ec) {
            scanning_ = false;
            return;
        }
    }
    if (scan_ != std::filesystem::recursive_directory_iterator{})
        return;
    scanning_ = false;
    if (scannedBytes_ <= config_.diskBytes)
        return;
    std::sort(entries_.begin(), entries_.end(), older);
    for (const auto &entry : entries_) {
        const std::string name(entry.name.begin(), entry.name.end());
        const auto p = config_.cacheDirectory / name.substr(0, 2) / (name + ".owc");
        // A concurrent read may have refreshed this candidate after the scan.
        if (std::filesystem::last_write_time(p, ec) != entry.time || ec)
            continue;
        if (std::filesystem::remove(p, ec) && !ec)
            scannedBytes_ -= std::min(scannedBytes_, entry.bytes);
        if (scannedBytes_ <= config_.diskBytes)
            break;
    }
}
} // namespace ow

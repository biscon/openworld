#pragma once
#include "terrain_stream.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace ow {
struct TerrainCacheKey {
    TerrainRequest request;
    uint64_t shaderFingerprint{}, deviceFingerprint{};
    TerrainProduct product = TerrainProduct::Coverage;
    uint32_t tileLevel = 0, representation = 3;
};
uint64_t terrainChecksum(const void *, size_t, uint64_t seed = 14695981039346656037ull);
// Two single-owner mailboxes. Polling, acquiring and releasing never lock or
// wait.
struct TerrainCacheHooks {
    // Optional worker-side instrumentation/fault injection. Never invoked by
    // frame processing.
    void (*beforeIO)(void *) = nullptr;
    void *context = nullptr;
    void (*beforeClaim)(void *) = nullptr; // Deterministic queued-cancellation fixtures.
};
class TerrainCache {
  public:
    enum class Result { Hit, Miss, Error, Written, Cancelled };
    TerrainCache(TerrainPayloadLayout, const TerrainStreamingConfig &, TerrainCacheHooks = {});
    ~TerrainCache();
    TerrainCache(const TerrainCache &) = delete;
    TerrainCache &operator=(const TerrainCache &) = delete;
    // Lease/poll/access operations below are render-thread only; workers never
    // acquire idle boxes.
    int read(TerrainCacheKey, TerrainHandle, uint64_t revision = 0);
    void cancelRead(int); // Never releases a running worker's mailbox.
    int acquire(); // Lease an idle buffer for asynchronous GPU readback.
    bool complete(int) const;
    uint32_t pending() const;
    uint8_t *data(int i) { return boxes_[i].bytes.data(); }
    const TerrainCacheKey &key(int i) const { return boxes_[i].key; }
    TerrainHandle handle(int i) const { return boxes_[i].handle; }
    uint64_t revision(int i) const { return boxes_[i].revision; }
    Result result(int i) const { return boxes_[i].result; }
    void write(int, TerrainCacheKey); // Hand a lease to its worker.
    void release(int);
    uint64_t allocatedBytes() const { return boxes_[0].bytes.size() * 2; }
    // Synchronous codec operations: workers and owned test fixtures only.
    Result load(const TerrainCacheKey &, uint8_t *);
    Result save(const TerrainCacheKey &, const uint8_t *);
    std::filesystem::path path(const TerrainCacheKey &) const;

  private:
    enum class State { Idle, Leased, Queued, Running, Complete };
    struct Box {
        std::atomic<State> state{State::Idle};
        TerrainCacheKey key{};
        TerrainHandle handle{};
        uint64_t revision = 0;
        std::atomic<bool> cancelled{false};
        bool writing = false;
        Result result = Result::Miss;
        std::vector<uint8_t> bytes;
        std::mutex mutex;
        std::condition_variable wake;
        std::thread thread;
    };
    TerrainPayloadLayout layout_;
    TerrainStreamingConfig config_;
    TerrainCacheHooks hooks_;
    std::array<Box, 2> boxes_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> serial_{0};
    std::mutex maintenanceMutex_; // Workers only; try-lock prevents read/write
                                  // serialization.
    std::filesystem::recursive_directory_iterator scan_;
    struct Candidate {
        std::array<char, 16> name;
        std::filesystem::file_time_type time;
        uint64_t bytes;
    };
    std::vector<Candidate> entries_;
    std::chrono::steady_clock::time_point nextMaintenance_{};
    uint64_t scannedBytes_ = 0;
    bool scanning_ = false;
    void worker(int);
    void prune();
    std::array<uint8_t, 128> header(const TerrainCacheKey &) const;
};
} // namespace ow

#pragma once
#include "terrain_generation_budget.hpp"
#include "terrain_lod.hpp"
#include <filesystem>
#include <functional>
#include <limits>

namespace ow {
constexpr uint32_t invalidTerrainSlot = UINT32_MAX;
constexpr uint64_t terrainMiB = 1024 * 1024;
struct TerrainStreamingConfig {
    bool enabled = false, diskCache = true;
    bool hierarchical = false;                    // Flat mode is retained for small legacy diagnostics.
    bool unfilteredHeightReference = false;       // Startup-only diagnostic comparison; same payload layout.
    bool vertexDiagnostics = false;               // Owned GPU fixtures only; disabled in the application.
    double diagnosticFrameSeconds = 0;            // Inject elapsed time in deterministic tests/probes only.
    uint64_t shadowCasterBytes = 64 * terrainMiB; // Additional coarse coverage; never displaces camera slots.
    uint64_t horizonBytes =
        0; // Opt-in coarse outer ring; camera/detail and shadow reservations remain protected.
    double horizonMaximumDistance = 1048576; // Startup cap for distant coverage.
    double maximumDistance = 16384; // Startup cap; small owned fixtures use smaller hierarchies.
    uint64_t transferBytesPerFrame =
        8 * terrainMiB; // Smaller limits also exercise sliced transfers in tests.
    uint64_t gpuBytes = 512 * terrainMiB, hostBytes = 256 * terrainMiB, diskBytes = 8192 * terrainMiB;
    std::filesystem::path cacheDirectory;
};
std::filesystem::path terrainDefaultCacheDirectory();
struct TerrainPoolCapacity {
    uint32_t coverage{}, detail{};
    int maximumRadius{};
};
// Startup only. Callbacks return actual allocation bytes for an entire pool of
// N slots. Costs must be nondecreasing; detail(0) is implicit and never
// queried.
TerrainPoolCapacity terrainPoolCapacity(uint64_t availableBytes, uint32_t maximumSlots, bool hasDetail,
                                        const std::function<uint64_t(uint32_t)> &coverageBytes,
                                        const std::function<uint64_t(uint32_t)> &detailBytes);
struct TerrainTileKey {
    int64_t x{}, z{};
    uint32_t level{};
    bool operator==(TerrainTileKey b) const { return x == b.x && z == b.z && level == b.level; }
};
int64_t terrainFloorDivide(int64_t value, int64_t divisor);
TerrainTileKey terrainParent(TerrainTileKey);
uint32_t terrainHierarchyLevel(int radius);
uint32_t terrainHierarchyCapacity(int radius);
int terrainMaximumRadius(TerrainGrid);
TerrainPoolCapacity terrainHierarchyPoolCapacity(uint64_t available, uint32_t maximumSlots, int radius,
                                                 bool hasDetail,
                                                 const std::function<uint64_t(uint32_t)> &coverageBytes,
                                                 const std::function<uint64_t(uint32_t)> &detailBytes);
enum class TerrainProduct : uint32_t { Coverage, Detail };
struct TerrainPayloadLayout {
    TerrainGrid grid;
    uint32_t subdivisions{}, patchCells{}, levels{};
    bool filteredHeights = false;
    uint64_t heightsBytes{}, boundsOffset{}, boundsBytes{}, coarseOffset{}, coverageBytes{}, detailBytes{};
    uint64_t leafBoundsBytes{}, errorOffset{};
    std::array<uint64_t, maxTerrainLodLevels> heightOffsets{}, errorOffsets{};
    std::array<uint32_t, maxTerrainLodLevels> widths{}, halos{};
    std::array<uint64_t, maxTerrainLodLevels> offsets{}, bytes{};
    TerrainPayloadLayout(TerrainGrid, uint32_t subdivisions, uint32_t patchCells, bool filtered = false);
    uint64_t size(TerrainProduct p) const {
        return p == TerrainProduct::Coverage ? coverageBytes : detailBytes;
    }
    bool valid(TerrainProduct, const uint8_t *, size_t) const;
};
struct TerrainHandle {
    uint32_t index = invalidTerrainSlot, generation = 0;
    bool operator==(TerrainHandle b) const { return index == b.index && generation == b.generation; }
};
struct TerrainTileSnapshot {
    TerrainTileKey key;
    TerrainHandle handle;
    bool resident{}, active{};
    uint32_t drawMask{};
    float heightFade{};
    uint64_t detailRevision{};
    bool detailWanted{}, detailResident{};
};
struct TerrainStreamStats {
    int requestedRadius{}, effectiveRadius{}, maximumRadius{};
    uint32_t coverage{}, detail{}, pending{}, detailPending{}, ioJobs{}, desired{}, capacity{},
        detailCapacity{};
    float detailFirstRange{}; // Base-tile normal demand range, before normal-level scaling.
    uint32_t detailEligible{}, detailSelected{}, detailReady{}; // Ready counts selected detail only.
    uint64_t hits{}, misses{}, errors{}, generations{}, gpuBytes{}, hostBytes{}, transferBytes{};
    uint32_t dispatches{}, generationGroups{}, generationGroupBudget{};
    uint32_t generationSegments{}, completedGenerationGroups{}, completedGenerationSegments{};
    double generationEstimatedMilliseconds{};
    TerrainGenerationStop generationStop = TerrainGenerationStop::Idle;
    double generationMilliseconds{}, terrainMilliseconds{};
    std::array<uint32_t, 32> residentLevels{}; // Data levels are independent of per-tile mesh LODs.
    int outerRequestedRadius{}, outerEffectiveRadius{};
    uint32_t horizonSlots{}, horizonRequested{}, horizonReady{};
    uint64_t horizonBytes{};
    uint32_t horizonLevels{};
    bool horizonLimited = false;
    double horizonRequiredDistance{}, horizonEffectiveDistance{};
    uint32_t shadowSlots{}, shadowRequested{}, shadowMissing{};
    uint64_t shadowBytes{};
    uint64_t pressureEvents{}, uploadBytes{}, cacheReadbackBytes{}, directBytes{};
    uint64_t detailCancellations{}, detailPreemptions{}, discardedReads{}, skippedExports{},
        discardedGroups{}, discardedPublications{}, obsoleteDetailGroups{};
    TerrainTileKey activeDetailTile;
    float activeDetailProgress{};
    bool activeDetail{}, detailRetiring{};
};
enum class TerrainDetailPhase { Idle, Generation, DirectCopy, Export, Upload };
struct TerrainDetailJobSnapshot {
    TerrainHandle handle;
    uint64_t revision{}, retireAfter{}, groups{}, completed{}, submitted{};
    TerrainDetailPhase phase = TerrainDetailPhase::Idle;
    bool cancelled{}, publicationPending{};
    double progress{};
};
// CPU demand/lifetime policy, independent of Vulkan and filesystem scheduling.
class TerrainStreamer {
  public:
    enum class State { Empty, Requested, Reading, Generate, Working, Resident, Retiring };
    struct Slot {
        ChunkCoord chunk{};
        uint32_t generation = 1;
        State state = State::Empty;
        uint64_t epoch{}, lastUse{}, retireAfter{};
        bool wanted = false, visible = false, detailWanted = false;
        int detail = -1;
        float fade = 0;
        uint32_t level = 0, drawMask = 15;
        float heightFade = 0;
        bool refine = false, active = false, support = false, shadowOnly = false,
             horizonOnly = false;
        TerrainTileKey tile() const { return {chunk.x, chunk.z, level}; }
    };
    TerrainStreamer(uint32_t capacity, int maximumRadius, TerrainGrid grid = {},
                    bool hierarchical = false, uint32_t shadowSlots = 0, uint32_t horizonSlots = 0,
                    int horizonRadius = 0);
    void shadowDemand(const TerrainTileKey *keys, uint32_t count);
    void demand(const Camera &, int radius, uint64_t completed, uint64_t submitted, uint64_t epoch);
    TerrainHandle next(State) const;
    bool valid(TerrainHandle h) const;
    Slot &slot(uint32_t i) { return slots_[i]; }
    const Slot &slot(uint32_t i) const { return slots_[i]; }
    const std::vector<Slot> &slots() const { return slots_; }
    uint32_t capacity() const { return uint32_t(slots_.size()); }
    int radius() const { return radius_; }
    int outerRadius() const { return outerRadius_; }
    int maximumRadius() const { return maximumRadius_; }
    int find(ChunkCoord) const;
    int findTile(TerrainTileKey) const;
    bool hierarchical() const { return hierarchical_; }
    void advance(float dt);
    void heightWeights(uint32_t slot, float out[16]) const;
    glm::vec3 offset(uint32_t slot, const Camera &) const;
    float scale(uint32_t slot) const { return float(uint32_t(1) << slots_[slot].level); }
    uint32_t topLevel() const { return topLevel_; }
    uint32_t detailedLevel() const { return detailedLevel_; }
    bool extendedHorizon() const { return horizonRadius_ > 0; }
    uint32_t demandCount() const { return uint32_t(wanted_.size()); }
    uint64_t pressureEvents() const { return pressureEvents_; }
    void weights(uint32_t slot, float out[16]) const;

  private:
    std::vector<Slot> slots_;
    uint32_t cameraCapacity_ = 0, baseCapacity_ = 0, horizonSlots_ = 0, shadowCount_ = 0;
    std::array<TerrainTileKey, 64> shadowWanted_{};
    std::array<int, 19 * 19> spatial_{};
    ChunkCoord centre_{};
    int radius_{}, outerRadius_{}, maximumRadius_{};
    TerrainGrid grid_;
    bool hierarchical_ = false;
    uint32_t topLevel_ = 0, detailedLevel_ = 0;
    int horizonRadius_ = 0;
    uint64_t pressureEvents_ = 0;
    struct Wanted {
        TerrainTileKey key;
        bool refine;
    };
    std::vector<Wanted> wanted_;
    std::vector<int> table_;
    void rebuildTable();
    void insert(uint32_t);
    void requestTree(TerrainTileKey);
    void demandHierarchy(const Camera &, int, uint64_t, uint64_t, uint64_t);
    double tileDistance(TerrainTileKey) const;
    bool parentReady(TerrainTileKey) const;
};
} // namespace ow

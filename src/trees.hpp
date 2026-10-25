#pragma once
#include "tree_library.hpp"
#include "vegetation_stream.hpp"
#include <memory>
namespace ow {
struct TreeSettings {
    bool enabled = true, occlusion = true;
    float distance = 4000, coverage = .60f, density = 1, wind = 1, time = 0, treeline = 850;
    glm::vec2 windDirection{1, .35f};
    int32_t forceLod = -1;
    bool diagnostic = false;
};
struct TreeStreamConfig {
    uint32_t cellCapacity = 32768, updatesPerFrame = 32;
    float maximumDistance = 4000;
    uint64_t budgetBytes = 768ull << 20;
};
struct TreeInstance {
    glm::vec4 rootScale{};
    float yaw = 0, variation = 0;
    uint32_t identity = 0, assetCell = 0;
};
static_assert(sizeof(TreeInstance) == 32);
struct TreeCellKey {
    int64_t x = 0, z = 0;
    bool operator==(TreeCellKey b) const { return x == b.x && z == b.z; }
};
struct TreeCellHandle {
    uint32_t index = ~0u, generation = 0;
};
struct TreeFixtureCell {
    TreeCellKey key;
    std::array<TreeInstance, 128> instances{};
};
struct TreeOptions {
    bool forceRgba = false;
    std::filesystem::path directory;
    TreeStreamConfig streaming;
    std::shared_ptr<const TreeLibrary> library; // Owned fixture/library override.
    std::vector<TreeFixtureCell> fixture;
};
struct TreeStats {
    bool enabled = false, completed = false, timingsAvailable = false;
    uint32_t resident = 0, pending = 0, deferred = 0, updates = 0, visible = 0, casters = 0, overflow = 0,
             surfaceMisses = 0;
    std::array<uint32_t, 4> lodCounts{};
    uint64_t bytes = 0, triangles = 0;
    double cpuMilliseconds = 0, selectionMilliseconds = 0, updateMilliseconds = 0, depthMilliseconds = 0,
           colourMilliseconds = 0, shadowMilliseconds = 0;
};
uint32_t treeHash(uint32_t);
uint32_t treeCandidateHash(TreeCellKey, uint32_t seed, uint32_t candidate);
glm::vec2 treeCandidatePosition(TreeCellKey, uint32_t seed, uint32_t candidate);
TreeCellKey treeCellAt(WorldPosition);
WorldPosition treeCellOrigin(TreeCellKey);
void validate(const TreeSettings &);
void validate(const TreeStreamConfig &);
class TreeResidency {
  public:
    enum class State { Empty, Active, Retiring };
    struct Slot {
        TreeCellKey key;
        uint32_t generation = 1;
        State state = State::Empty;
        uint64_t revision = 0, lastUse = 0;
        bool wanted = false, generated = false;
        float distanceSquared = 0;
    };
    explicit TreeResidency(TreeStreamConfig);
    void demand(WorldPosition, uint64_t revision, uint64_t completed, bool enabled, float radius);
    bool valid(TreeCellHandle) const;
    void used(uint32_t, uint64_t);
    Slot &slot(uint32_t i) { return slots_[i]; }
    const auto &slots() const { return slots_; }
    const auto &ordered() const { return ordered_; }
    uint32_t deferred() const { return deferred_; }
    uint32_t retiring() const { return retiring_; }

  private:
    struct Wanted {
        TreeCellKey key;
        float distance;
    };
    TreeStreamConfig config_;
    std::vector<Slot> slots_;
    std::vector<int32_t> table_;
    std::vector<Wanted> wanted_;
    std::vector<uint32_t> ordered_;
    uint32_t deferred_ = 0, retiring_ = 0;
    WorldPosition previousCamera_;
    float previousRadius_ = 0;
    uint64_t previousRevision_ = 0;
    bool previousEnabled_ = false, initialized_ = false;
    int find(TreeCellKey) const;
    void insert(uint32_t);
};
struct TreeTerrainCell {
    static constexpr uint32_t maximumPatches = 256;
    std::array<VegetationTerrainPatch, maximumPatches> patches{};
    uint32_t count = 0;
    uint64_t signature = 0;
    glm::vec2 bounds{};
    TerrainParameters geography{};
};
// Explicit blocking diagnostics; bounded to 64 cells and never used by frame rendering.
struct TreeCellSnapshot {
    TreeCellKey key;
    std::array<TreeInstance, 128> population;
    std::array<glm::vec4, 128> attachments;
    TreeTerrainCell surface;
};
struct TreeSnapshot {
    std::vector<TreeCellSnapshot> cells;
};
} // namespace ow

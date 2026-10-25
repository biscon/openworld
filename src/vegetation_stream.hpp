#pragma once
#include "vegetation_library.hpp"
#include <array>
namespace ow {
// Shared soft GPU budget: preserve required repairs and allow one expensive cell
// when time remains, rather than starving forever after a slow timing sample.
uint32_t populationUpdateAllowance(uint32_t pending, uint32_t required, double remaining, double cellCost);
constexpr uint32_t vegetationCandidatesPerCell = 128;
struct VegetationCellKey {
    int64_t x = 0, z = 0;
    bool operator==(VegetationCellKey b) const { return x == b.x && z == b.z; }
};
struct VegetationCellHandle {
    uint32_t index = ~0u, generation = 0;
};
enum class VegetationHabitatMode : uint32_t { Uniform, Blended };
enum class VegetationHabitat : uint32_t { Meadow, Moor, Understory };
struct VegetationHabitatInput {
    uint32_t revision = 1;
    VegetationHabitatMode mode = VegetationHabitatMode::Uniform;
    glm::vec3 weights{1, 0, 0};
    float minimumElevation = -10000, maximumElevation = 10000;
    float minimumUp = .70710678f;
};
struct VegetationExclusionInput {
    uint32_t revision = 1;
    WorldPosition centre;
    float radius = 0; // zero disables the controlled clearing
};
struct VegetationStreamConfig {
    bool enabled = false, diagnostics = false;
    uint32_t cellCapacity = 4096, updatesPerFrame = 32;
    float demandRadius = 96, retentionRadius = 112;
};
VegetationHabitatInput vegetationHabitat(VegetationHabitat);
uint32_t vegetationCandidateHash(VegetationCellKey, uint32_t seed, uint32_t species, uint32_t candidate);
WorldPosition vegetationCellOrigin(VegetationCellKey);
VegetationCellKey vegetationCellAt(WorldPosition);
class VegetationResidency {
  public:
    enum class State { Empty, Active, Retiring };
    struct Slot {
        VegetationCellKey key;
        uint32_t generation = 1;
        State state = State::Empty;
        uint64_t revision = 0, lastUse = 0;
        bool wanted = false, generated = false;
        float distanceSquared = 0;
    };
    explicit VegetationResidency(VegetationStreamConfig);
    void demand(WorldPosition, uint64_t revision, uint64_t completed, bool enabled, float radius = 0);
    bool valid(VegetationCellHandle) const;
    void used(uint32_t slot, uint64_t serial);
    Slot &slot(uint32_t i) { return slots_[i]; }
    const auto &slots() const { return slots_; }
    const auto &ordered() const { return ordered_; }
    uint32_t deferred() const { return deferred_; }
    uint32_t retiring() const { return retiring_; }

  private:
    struct Wanted {
        VegetationCellKey key;
        float distance;
    };
    VegetationStreamConfig config_;
    std::vector<Slot> slots_;
    std::vector<int32_t> table_;
    std::vector<Wanted> wanted_;
    std::vector<uint32_t> ordered_;
    uint32_t deferred_ = 0, retiring_ = 0;
    WorldPosition previousCamera_;
    float previousRadius_ = 0;
    uint64_t previousRevision_ = 0;
    bool previousEnabled_ = false, initialized_ = false;
    int find(VegetationCellKey) const;
    void insert(uint32_t);
};
// Shader ABI: height ranges are float2 element offsets, not byte offsets.
struct VegetationTerrainPatch {
    glm::vec4 rectangle{}; // cell-relative x/z origin, sample spacing, unused
    glm::uvec4 geometry{}; // patch x/z, step in provider samples, patch cells
    glm::uvec4 heights{};  // level offset, parent offset, level width, parent width
    glm::uvec4 topology{}; // edges, corners, geometry level, eroded
};
static_assert(sizeof(VegetationTerrainPatch) == 64);
struct VegetationTerrainCell {
    static constexpr uint32_t maximumPatches = 16;
    std::array<VegetationTerrainPatch, maximumPatches> patches{};
    uint32_t count = 0;
    glm::uvec4 canonical{};    // offset, width, eroded, available
    glm::vec4 canonicalGrid{}; // provider origin relative to cell, spacing, unused
    glm::vec2 bounds{};
    uint64_t signature = 0, populationRevision = 0;
    uint32_t seed = 0;
};
struct VegetationCellSnapshot {
    VegetationCellKey key;
    VegetationCellHandle handle;
    VegetationTerrainCell surface;
    bool historyValid = false;
    std::array<FoliageInstance, 128> population;
    std::array<glm::vec4, 128> attachment;
};
} // namespace ow

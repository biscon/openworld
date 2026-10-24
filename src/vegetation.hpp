#pragma once
#include "scene.hpp"
#include "vegetation_library.hpp"
#include "vegetation_stream.hpp"
#include <array>
#include <filesystem>
#include <memory>
namespace ow {
constexpr uint32_t vegetationBins = 12, vegetationViews = 6, vegetationMaximum = vegetationCellLimit * 128;
struct VegetationInstance {
    glm::vec4 rootScale{0};
    float yaw = 0, variation = 0;
    uint32_t identity = 0, cell = 0;
};
static_assert(sizeof(VegetationInstance) == 32);
struct VegetationAssets {
    std::array<MeshData, vegetationBins> meshes;
    TextureData cards;
};
struct VegetationFixture {
    VegetationAssets assets;
    std::vector<VegetationInstance> instances;
    MeshData ground;
    uint32_t side = 256, seed = 173;
    bool rolling = false;
};
struct VegetationOptions {
    std::shared_ptr<const VegetationLibrary> library;
    std::shared_ptr<const VegetationPopulation> population;
    std::shared_ptr<const VegetationFixture> fixture;
    VegetationStreamConfig streaming;
    uint64_t budgetBytes = 128ull << 20;
    uint32_t listCapacity = 0; // zero = required part entries; smaller values are
                               // overflow diagnostics
};
struct VegetationSettings {
    bool enabled = true, coverage = false;
    VegetationHabitatInput habitat;
    VegetationExclusionInput exclusion;
    uint32_t representation = 0; // blades, cards, hybrid
    float density = 1, cardWidth = 1, cardDensity = 1;
    float time = 0, wind = 1, distance = 70, shadowDistance = 0;
    glm::vec2 windDirection{1, .45f};
    bool transmission = true, depthPrepass = true, occlusion = false;
    int32_t isolateAsset = -1, forceLod = -1; // -2 selects the grass category
    uint32_t inspectIdentity = ~0u;
};
struct VegetationStats {
    uint64_t frame = 0, allocatedBytes = 0, triangles = 0;
    uint32_t visible = 0, casters = 0, overflow = 0, casterCandidates = 0, selectionGroups = 0;
    double preparationMilliseconds = 0, selectionMilliseconds = 0, depthMilliseconds = 0,
           colourMilliseconds = 0, shadowMilliseconds = 0;
    uint32_t residentCells = 0, pendingCells = 0, retiringCells = 0, deferredCells = 0,
             missingTerrainCells = 0, updatedCells = 0, generatedCells = 0;
    double updateMilliseconds = 0;
    bool completed = false, timingsAvailable = false, enabled = false;
};
struct VegetationSnapshot {
    uint32_t binCount = vegetationBins;
    VegetationStats stats;
    std::vector<VegetationCellSnapshot> cells;
    std::array<uint32_t, vegetationBinLimit * vegetationViews> counts{};
    std::array<std::vector<uint32_t>, vegetationViews> identities;
};
VegetationAssets makeVegetationAssets();
void writeVegetationAssets(const std::filesystem::path &, const VegetationAssets &);
VegetationAssets readVegetationAssets(const std::filesystem::path &);
VegetationFixture makeVegetationFixture(VegetationAssets, uint32_t side = 256, bool rolling = false,
                                        uint32_t seed = 173);
float vegetationGroundHeight(float x, float z, bool rolling);
void validate(const VegetationSettings &);
} // namespace ow

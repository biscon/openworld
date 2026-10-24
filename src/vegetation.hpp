#pragma once
#include "scene.hpp"
#include <array>
#include <filesystem>
#include <memory>
namespace ow {
constexpr uint32_t vegetationBins = 12, vegetationViews = 6, vegetationMaximum = 524288;
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
    std::shared_ptr<const VegetationFixture> fixture;
    uint64_t budgetBytes = 128ull << 20;
    uint32_t listCapacity = 0; // zero = population; smaller values are overflow diagnostics
};
struct VegetationSettings {
    bool enabled = true, coverage = false;
    uint32_t representation = 0; // blades, cards, hybrid
    float density = 1, cardWidth = 1, cardDensity = 1;
    float time = 0, wind = 1, distance = 70, shadowDistance = 25;
};
struct VegetationStats {
    uint64_t frame = 0, allocatedBytes = 0, triangles = 0;
    uint32_t visible = 0, casters = 0, overflow = 0;
    double preparationMilliseconds = 0, selectionMilliseconds = 0, depthMilliseconds = 0,
           colourMilliseconds = 0, shadowMilliseconds = 0;
    bool completed = false, timingsAvailable = false, enabled = false;
};
struct VegetationSnapshot {
    VegetationStats stats;
    std::array<uint32_t, vegetationBins * vegetationViews> counts{};
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

#pragma once
#include "scene.hpp"
#include <array>
#include <filesystem>
namespace ow {
constexpr uint32_t vegetationAssetLimit = 8, vegetationBinLimit = 48, vegetationCellLimit = 12288;
struct VegetationAssetId {
    uint32_t value = ~0u;
};
struct FoliageVertex {
    glm::vec3 position{}, normal{0, 1, 0};
    glm::vec4 tangent{1, 0, 0, 1};
    glm::vec2 uv{};
    glm::vec4 deformation{}; // normalized height, bend weight, flutter weight, phase
};
static_assert(sizeof(FoliageVertex) == 64);
struct GrassMorph {
    glm::vec4 centreBlade{}, nextCentreWidth{}, nextNormal{}, nextDeformation{};
};
static_assert(sizeof(GrassMorph) == 64);
struct GrassStability {
    glm::uvec4 counts{}; // zero disables the profile; xyz are nested blade counts
    glm::vec4 bands{18, 28, 38, 55};
    glm::vec4 shading{15, 40, .4f, .95f}; // start/end, macro-normal blend, roughness floor
};
struct FoliagePart {
    std::vector<FoliageVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t material = 0;
    std::vector<GrassMorph> grassMorph;
};
struct FoliageMaterial {
    glm::vec4 colour{1};
    float roughness = .8f, cutoff = .5f, transmission = 0;
    int32_t colourTexture = -1, normalTexture = -1;
    bool masked = false;
};
enum class FoliageCategory : uint32_t { Grass, Plant, Shrub, Clutter };
enum class FoliageSpecies : uint32_t { Grass, Fern, Shrub, Daisy, Heather, Clutter };
struct FoliageAsset {
    FoliageCategory category = FoliageCategory::Plant;
    std::array<std::vector<FoliagePart>, 3> lods;
    glm::vec3 minimum{0}, maximum{0};
    glm::vec4 ranges{20, 45, 70, 0}; // LOD1, LOD2, camera fade end, shadow fade end
    float stiffness = 1, displacement = .5f;
    GrassStability grass;
    FoliageSpecies species = FoliageSpecies::Fern;
    glm::vec4 habitat{1, .65f, .35f, 0};        // meadow, moor, understory, cluster amount
    glm::vec4 placement{.25f, .02f, .8f, 1.2f}; // footprint, conformity tolerance, scale range
    glm::vec4 interaction{0};                   // horizontal displacement, flattening, reserved
};
struct VegetationLibrary {
    uint32_t revision = 3;
    std::vector<FoliageAsset> assets;
    std::vector<FoliageMaterial> materials;
    std::vector<TextureData> textures;
};
struct FoliageInstance {
    glm::vec4 rootScale{0, 0, 0, 1};
    float yaw = 0, variation = 0;
    uint32_t identity = 0, assetCell = 0; // asset low 3 bits, cell remaining bits
};
static_assert(sizeof(FoliageInstance) == 32);
uint32_t packVegetationAsset(VegetationAssetId, uint32_t cell);
struct FoliageCell {
    WorldPosition origin;
    uint32_t first = 0, count = 0;
};
struct VegetationPopulation {
    std::vector<FoliageCell> cells;
    std::vector<FoliageInstance> instances;
};
struct VegetationRecipe {
    uint32_t blades = 24;
    float width = .010f, spread = .23f, height = .65f;
};
VegetationLibrary buildVegetationLibrary(const VegetationRecipe & = {});
void validate(const VegetationLibrary &);
void validate(const VegetationPopulation &, const VegetationLibrary &);
void writeVegetationLibrary(const std::filesystem::path &, const VegetationLibrary &,
                            const std::string &suffix = "");
VegetationLibrary readVegetationLibrary(const std::filesystem::path &, bool rgba = false);
VegetationPopulation makeVegetationPopulation(uint32_t side, bool mixed, bool rolling = false,
                                              uint32_t seed = 173);
} // namespace ow

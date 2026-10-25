#pragma once
#include "vegetation_library.hpp"
namespace ow {
constexpr uint32_t treeAssetLimit = 24, treeLodCount = 4;
enum class TreeSpecies : uint32_t { Beech, Oak, Birch, Pine };
struct TreeAssetId {
    uint32_t value = ~0u;
};
using TreeVertex = FoliageVertex;
struct TreePart {
    std::vector<TreeVertex> vertices;
    std::vector<uint32_t> indices;
};
struct TreeAsset {
    TreeSpecies species = TreeSpecies::Beech;
    uint32_t variant = 0, seed = 1;
    std::array<std::array<TreePart, 2>, 3> lods;
    glm::vec3 minimum{}, maximum{};
    float displacement = 1.2f;
    uint32_t frames = 12, tileSize = 128;
    glm::vec4 capture{}; // centre.xyz, orthographic half extent (all axes)
};
struct TreeLibrary {
    std::vector<TreeAsset> assets;
};
const char *treeSpeciesName(TreeSpecies);
void validate(const TreeLibrary &);
void writeTreeLibrary(const std::filesystem::path &, const TreeLibrary &);
TreeLibrary readTreeLibrary(const std::filesystem::path &);
glm::vec3 treeOctDecode(glm::vec2);
glm::vec2 treeOctEncode(glm::vec3);
struct TreeViewBlend {
    std::array<uint32_t, 3> frames;
    glm::vec3 weights;
};
TreeViewBlend treeViewBlend(glm::vec3 direction, uint32_t frames);
} // namespace ow

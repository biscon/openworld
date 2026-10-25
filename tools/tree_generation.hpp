#pragma once
#include "tree_library.hpp"
namespace ow {
struct TreeRecipe {
    TreeSpecies species = TreeSpecies::Beech;
    uint32_t variant = 0, seed = 1, attractors = 800;
    float height = 20, crownRadius = 5;
};
TreeAsset generateTree(const TreeRecipe &);
} // namespace ow

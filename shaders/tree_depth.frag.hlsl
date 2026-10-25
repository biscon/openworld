#define OW_TREE_DEPTH
#include "tree_surface_render.hlsli"
float main(TreeFragment i, bool front : SV_IsFrontFace) : SV_Depth {
    float depth, thin;
    treeShade(i, front, depth, thin);
    return depth;
}

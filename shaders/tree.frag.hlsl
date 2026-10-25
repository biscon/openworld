#include "tree_surface_render.hlsli"
struct Out {
    float4 colour : SV_Target0;
    float depth : SV_Depth;
};
Out main(TreeFragment i, bool front : SV_IsFrontFace) {
    Out o;
    float thin;
    o.colour = treeShade(i, front, o.depth, thin);
    return o;
}

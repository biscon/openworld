#include "tree_surface_render.hlsli"
struct Out {
    float4 colour : SV_Target0;
    float4 motion : SV_Target1;
    float reactive : SV_Target2;
    float depth : SV_Depth;
};
Out main(TreeFragment i, bool front : SV_IsFrontFace) {
    Out o;
    float thin;
    float4 c = treeShade(i, front, o.depth, thin);
    TemporalOutput t = temporalOutput(c, i.rendered, i.previous, i.previous.w > 0, 0, thin > 0);
    o.colour = t.colour;
    o.motion = t.motion;
    o.reactive = t.reactive;
    return o;
}

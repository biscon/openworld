#include "lighting.hlsli"
#include "tree_types.hlsli"
[[vk::binding(4, 0)]] StructuredBuffer<uint> treeList;
[[vk::binding(5, 0)]] StructuredBuffer<uint> treeWork;
struct In {
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float3 n : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] float4 deformation : TEXCOORD1;
    [[vk::location(4)]] float4 tangent : TEXCOORD2;
};
struct Out {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 local : TEXCOORD0;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD1;
    [[vk::location(3)]] float3 relative : TEXCOORD2;
    [[vk::location(4)]] float4 previous : TEXCOORD3;
    [[vk::location(5)]] nointerpolation uint index : TEXCOORD4;
    [[vk::location(6)]] nointerpolation float pixels : TEXCOORD5;
    [[vk::location(7)]] float3 rendered : TEXCOORD6;
    [[vk::location(8)]] float4 tangent : TEXCOORD7;
};
Out main(In i, uint id : SV_InstanceID) {
    Out o;
    uint view = treePush.view, bin = treePush.bin;
    uint index = treeList[forest.lists[view].x + treeWork[view * treeWorkWords + bin * 8 + 7] + id];
    Tree t = trees[index];
    TreeCell cell = treeCells[t.assetCell >> 5];
    uint asset = t.assetCell & 31, lod = bin % 4;
    t.rootScale.y = treeHeight(index, false);
    float3 p = i.p;
    float4 weights = i.deformation;
    if (lod == 3) {
        float4 capture = forest.capture[asset];
        float3 centre = treePosition(t, cell, capture.xyz, float4(0, .5, 0, 0), index, false);
        float3 dir = treeRotate(view == 0 ? normalize(-centre) : forest.sun.xyz, -t.yaw), r, u;
        treeBasis(dir, r, u);
        p = capture.xyz + (r * i.p.x + u * i.p.y) * capture.w;
        weights = float4(0, .5, 0, 0);
    }
    o.local = p;
    o.relative = treePosition(t, cell, p, weights, index, false);
    o.rendered = curvePosition(o.relative, forest.curvature);
    o.position = mul(forest.views[view], float4(o.rendered, 1));
    o.normal = treeRotate(i.n, t.yaw);
    o.tangent = float4(treeRotate(i.tangent.xyz, t.yaw), i.tangent.w);
    o.uv = i.uv;
    o.index = index;
    o.pixels = treePixels(t, cell, view, index);
    bool valid = forest.mode.y != 0 && cell.previous.w != 0 &&
                 !(asuint(treeAttachments[index].z) == forest.mode.w && treeAttachments[index].w == 2);
    float3 previous = treePosition(t, cell, p, weights, index, true);
    o.previous =
        valid ? mul(temporal.previous, float4(curvePosition(previous, temporal.previousCurvature), 1)) : 0;
    return o;
}

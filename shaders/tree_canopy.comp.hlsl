#include "tree_types.hlsli"
[[vk::binding(6, 0)]] StructuredBuffer<uint> treeGroups;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> canopy;
[numthreads(128, 1, 1)] void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint cell = treeGroups[group.x];
    TreeCell c = treeCells[cell];
    if (c.meta.x == 0 || any(abs(c.origin.xz + 32) > 320))
        return;
    uint index = cell * 128 + lane;
    Tree t = trees[index];
    if (t.rootScale.w <= 0 || treeAttachments[index].w <= 0)
        return;
    uint asset = t.assetCell & 31;
    float radius = max(.5, forest.bounds[asset].x * t.rootScale.w * .68);
    float2 centre = t.rootScale.xz + c.origin.xz;
    int2 lo = clamp(int2(floor((centre - radius + 256) * .25)), 0, 127),
         hi = clamp(int2(ceil((centre + radius + 256) * .25)), 0, 127);
    for (int y = lo.y; y <= hi.y; ++y)
        for (int x = lo.x; x <= hi.x; ++x) {
            float2 p = float2(x, y) * 4 - 254;
            float cover = (1 - smoothstep(radius * .3, radius, length(p - centre))) * .85;
            InterlockedMax(canopy[y * 128 + x], uint(saturate(cover) * 65535));
        }
}

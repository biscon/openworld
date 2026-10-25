#define OW_TREE_SELECTION
#include "tree_types.hlsli"
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> treeList;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> treeWork;
[[vk::binding(6, 0)]] StructuredBuffer<uint> treeGroups;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> treePrefixes;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> treeChoices;
[[vk::binding(9, 0)]] Texture2D<float> treePyramid;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint> treeIndirect;
groupshared uint counts[96];
groupshared uint accepted;
bool occluded(float3 lo, float3 hi) {
    if (forest.pyramid.w == 0 || treePush.view != 0)
        return false;
    float2 a = 1e30, b = -1e30;
    float nearest = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        float3 p = float3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        float4 h = mul(forest.raster, float4(curvePosition(p, forest.curvature), 1));
        if (h.w <= 1 || h.z >= h.w)
            return false;
        float3 q = h.xyz / h.w;
        a = min(a, q.xy);
        b = max(b, q.xy);
        nearest = max(nearest, q.z);
    }
    uint2 extent = forest.pyramid.xy;
    uint2 ia = uint2(clamp(floor((clamp(a, -1, 1) * .5 + .5) * extent) - 1, 0, float2(extent - 1))),
          ib = uint2(clamp(ceil((clamp(b, -1, 1) * .5 + .5) * extent) + 1, 0, float2(extent - 1)));
    uint level = 0;
    uint2 ma = ia, mb = ib;
    while (any(mb - ma > 1) && level + 1 < forest.pyramid.z) {
        ++level;
        uint2 size = max(uint2(1, 1), extent >> level);
        ma = min(ia >> level, size - 1);
        mb = min(ib >> level, size - 1);
    }
    float depth = 1;
    for (uint y = ma.y; y <= mb.y; ++y)
        for (uint x = ma.x; x <= mb.x; ++x)
            depth = min(depth, treePyramid.Load(int3(x, y, level)));
    return nearest < depth - 2e-6;
}
uint choose(uint index, uint cell) {
    TreeCell c = treeCells[cell];
    Tree t = trees[index];
    float4 a = treeAttachments[index];
    if (t.rootScale.w <= 0 || a.w <= 0 || c.meta.x == 0)
        return 0;
    t.rootScale.y = a.x;
    uint asset = t.assetCell & 31;
    if (asset >= forest.limits.z)
        return 0;
    float3 root = t.rootScale.xyz + c.origin.xyz;
    float distance = length(root.xz);
    if (distance > forest.settings.x + (treePush.view != 0 ? 1024 : 0))
        return 0;
    float4 bound = forest.bounds[asset] * t.rootScale.w;
    float3 lo = root + float3(-bound.x, bound.y, -bound.x), hi = root + float3(bound.x, bound.z, bound.x);
    if (!treeVisible(lo, hi, treePush.view) || occluded(lo, hi))
        return 0;
    float pixels = treeProjectedPixels(t, c, treePush.view);
    if (treePush.view == 0 && forest.mode.x >= 4) {
        uint state = treeLodHistory[index];
        float old = (state & 65535) * .25;
        if ((state >> 16) == (c.meta.y & 65535) && old > 0 && abs(pixels - old) < max(.5, old * .015))
            pixels = old;
        uint quantized = clamp(uint(round(pixels * 4)), 1u, 65535u);
        treeLodHistory[index] = ((c.meta.y & 65535) << 16) | quantized;
        pixels = quantized * .25;
    }
    uint lod = forest.mode.x < 4 ? forest.mode.x : treeLod(pixels), other = ~0u;
    if (forest.mode.x >= 4) {
        for (uint k = 0; k < 3; ++k) {
            float boundary = k == 0 ? 320 : k == 1 ? 120 : 64;
            if (pixels > boundary * .85 && pixels < boundary * 1.15) {
                lod = k;
                other = k + 1;
            }
        }
    }
    return (asset * 4 + lod + 1) | ((other < 4 ? asset * 4 + other + 1 : 0) << 16);
}
[numthreads(128, 1, 1)] void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint view = treePush.view, base = view * treeWorkWords, n = forest.limits.x;
    if (treePush.phase == 1) {
        if (lane >= 96)
            return;
        uint total = 0;
        for (uint g = 0; g < n; ++g) {
            uint at = g * 96 + lane, v = treePrefixes[at];
            treePrefixes[at] = total;
            total += v;
        }
        treeWork[base + lane * 8 + 5] = total;
        return;
    }
    if (treePush.phase == 2) {
        if (lane != 0)
            return;
        uint offset = 0;
        for (uint b = 0; b < 96; ++b) {
            uint j = base + b * 8, wanted = treeWork[j + 5],
                 count = min(wanted, forest.lists[view].y - min(offset, forest.lists[view].y));
            treeWork[j] = 0;
            treeWork[j + 1] = count;
            treeWork[j + 2] = 0;
            treeWork[j + 3] = 0;
            treeWork[j + 4] = 0;
            treeWork[j + 6] = 0;
            treeWork[j + 7] = offset;
            offset += count;
            treeWork[base + 768] += wanted - count;
            for (uint part = 0; part < 2; ++part) {
                uint command = ((view * 96 + b) * 2 + part) * 5;
                uint4 geo = forest.geometry[b * 2 + part];
                treeIndirect[command] = geo.x;
                treeIndirect[command + 1] = count;
                treeIndirect[command + 2] = geo.y;
                treeIndirect[command + 3] = geo.z;
                treeIndirect[command + 4] = 0;
            }
        }
        return;
    }
    uint cell = treeGroups[group.x], index = cell * 128 + lane;
    if (lane < 96)
        counts[lane] = 0;
    if (lane == 0) {
        TreeCell c = treeCells[cell];
        accepted = c.meta.x != 0 && treeVisible(c.lo.xyz + c.origin.xyz, c.hi.xyz + c.origin.xyz, view);
    }
    GroupMemoryBarrierWithGroupSync();
    if (treePush.phase == 0) {
        uint v = accepted != 0 ? choose(index, cell) : 0;
        treeChoices[index] = v;
        uint a = v & 65535, b = v >> 16;
        if (a != 0)
            InterlockedAdd(counts[a - 1], 1);
        if (b != 0)
            InterlockedAdd(counts[b - 1], 1);
        GroupMemoryBarrierWithGroupSync();
        if (lane < 96)
            treePrefixes[group.x * 96 + lane] = counts[lane];
        return;
    }
    uint choice = treeChoices[index];
    uint ids[2] = {choice & 65535, choice >> 16};
    for (uint k = 0; k < 2; ++k)
        if (ids[k] != 0) {
            uint b = ids[k] - 1, local;
            InterlockedAdd(counts[b], 1, local);
            uint at = treePrefixes[group.x * 96 + b] + local;
            if (at < treeWork[base + b * 8 + 1])
                treeList[forest.lists[view].x + treeWork[base + b * 8 + 7] + at] = index;
        }
}

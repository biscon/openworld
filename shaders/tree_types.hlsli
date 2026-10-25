#ifndef OW_TREE_TYPES
#define OW_TREE_TYPES
#include "curvature.hlsli"
#include "tree_ecology.hlsli"
struct Tree {
    float4 rootScale;
    float yaw, variation;
    uint identity, assetCell;
};
struct TreeCell {
    float4 origin, previous, lo, hi, phase;
    uint4 meta;
};
struct TreeUniform {
    column_major float4x4 views[6];
    float4 settings, previousSettings, wind, previousWind, detail, sun, curvature;
    uint4 limits, mode;
    float4 capture[24], bounds[24], shadowFootprint[6];
    uint4 geometry[192];
    uint4 lists[6];
    column_major float4x4 raster;
    uint4 pyramid;
};
[[vk::binding(0, 0)]] StructuredBuffer<Tree> trees;
[[vk::binding(1, 0)]] StructuredBuffer<float4> treeAttachments;
[[vk::binding(2, 0)]] StructuredBuffer<TreeCell> treeCells;
[[vk::binding(3, 0)]] ConstantBuffer<TreeUniform> forest;
#ifdef OW_TREE_SELECTION
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> treeLodHistory;
#else
[[vk::binding(12, 0)]] StructuredBuffer<uint> treeLodHistory;
#endif

struct TreePush {
    uint view, bin, phase, part;
};
[[vk::push_constant]] ConstantBuffer<TreePush> treePush;
static const uint treeBins = 96, treeWorkWords = 769;
float3 treeRotate(float3 p, float yaw) {
    float s, c;
    sincos(yaw, s, c);
    return float3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}
float3 treeOctDecode(float2 uv) {
    float2 p = uv * 2 - 1;
    float3 n = float3(p.x, 1 - abs(p.x) - abs(p.y), p.y);
    if (n.y < 0)
        n.xz = (1 - abs(p.yx)) * float2(p.x >= 0 ? 1 : -1, p.y >= 0 ? 1 : -1);
    return normalize(n);
}
float2 treeOctEncode(float3 n) {
    n /= dot(abs(n), 1.0.xxx);
    float2 p = n.xz;
    if (n.y < 0)
        p = (1 - abs(p.yx)) * float2(p.x >= 0 ? 1 : -1, p.y >= 0 ? 1 : -1);
    return p * .5 + .5;
}
void treeViews(float3 d, uint n, out uint3 frames, out float3 weights) {
    float2 p = treeOctEncode(d) * (n - 1);
    uint2 c = min(uint2(p), n - 2);
    float2 f = p - c;
    uint a = c.y * n + c.x;
    if (f.x + f.y <= 1) {
        frames = uint3(a, a + 1, a + n);
        weights = float3(1 - f.x - f.y, f.x, f.y);
    } else {
        frames = uint3(a + n + 1, a + n, a + 1);
        weights = float3(f.x + f.y - 1, 1 - f.x, 1 - f.y);
    }
}
void treeBasis(float3 d, out float3 r, out float3 u) {
    r = normalize(cross(abs(d.y) < .99 ? float3(0, 1, 0) : float3(0, 0, 1), d));
    u = cross(d, r);
}
float3 treeWind(Tree t, TreeCell c, float3 p, float4 weights, bool old) {
    float4 w = old ? forest.previousWind : forest.wind;
    float time = old ? forest.previousSettings.w : forest.settings.w;
    float phase = dot(c.phase.xy + t.rootScale.xz, float2(.024, .017)) + t.variation * 6.2831853;
    float gust = sin(time * .8 + phase) * .38 + sin(time * .23 + phase * 1.7) * .14;
    float fine = sin(time * 4.1 + weights.w * 6.28) * .045 * weights.z;
    float fade = 1 - smoothstep(80, 240, length(t.rootScale.xyz + c.origin.xyz));
    p.xz += w.xy * w.z * (gust * saturate(weights.y) + fine * fade) * t.rootScale.w;
    return p;
}
float treeHeight(uint index, bool old) {
    float4 a = treeAttachments[index];
    return old && asuint(a.z) == forest.mode.w ? a.y : a.x;
}
float3 treePosition(Tree t, TreeCell c, float3 vertex, float4 weights, uint index, bool old) {
    float3 p = treeRotate(vertex * t.rootScale.w, t.yaw);
    p = treeWind(t, c, p, weights, old);
    p += float3(t.rootScale.x, treeHeight(index, old), t.rootScale.z) + (old ? c.previous.xyz : c.origin.xyz);
    return p;
}
float treeProjectedPixels(Tree t, TreeCell c, uint view) {
    uint asset = t.assetCell & 31;
    float size = forest.bounds[asset].w * t.rootScale.w;
    if (view != 0)
        return size / max(forest.shadowFootprint[view].x, 1e-4);
    float d = length(float3(t.rootScale.x, t.rootScale.y, t.rootScale.z) + c.origin.xyz +
                     float3(0, forest.capture[asset].y * t.rootScale.w, 0));
    return size * forest.detail.x / max(d, 1);
}
float treePixels(Tree t, TreeCell c, uint view, uint index) {
    float pixels = treeProjectedPixels(t, c, view);
    if (view == 0 && forest.mode.x >= 4) {
        uint state = treeLodHistory[index];
        if ((state >> 16) == (c.meta.y & 65535) && (state & 65535) != 0)
            pixels = (state & 65535) * .25;
    }
    return pixels;
}
uint treeLod(float pixels) { return pixels > 320 ? 0 : pixels > 120 ? 1 : pixels > 64 ? 2 : 3; }
float treeLodWeight(float pixels, uint lod) {
    float a = lod == 0 ? 1
                       : smoothstep((lod == 1   ? 320
                                     : lod == 2 ? 120
                                                : 64) *
                                        1.15,
                                    (lod == 1   ? 320
                                     : lod == 2 ? 120
                                                : 64) *
                                        .85,
                                    pixels);
    float b = lod == 3 ? 1
                       : smoothstep((lod == 0   ? 320
                                     : lod == 1 ? 120
                                                : 64) *
                                        .85,
                                    (lod == 0   ? 320
                                     : lod == 1 ? 120
                                                : 64) *
                                        1.15,
                                    pixels);
    return min(a, b);
}
bool treeVisible(float3 lo, float3 hi, uint view) {
    float3 centre = (lo + hi) * .5, extent = (hi - lo) * .5;
    float extra = forest.curvature.z != 0
                      ? max(length(lo.xz), length(hi.xz)) * length(extent.xz) / max(forest.curvature.x, 1)
                      : 0;
    centre = curvePosition(centre, forest.curvature);
    extent += extra;
    uint outside = 63;
    [unroll] for (uint i = 0; i < 8; ++i) {
        float3 p = centre + extent * float3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1);
        float4 h = mul(forest.views[view], float4(p, 1));
        uint code = (h.x < -h.w ? 1 : 0) | (h.x > h.w ? 2 : 0) | (h.y < -h.w ? 4 : 0) | (h.y > h.w ? 8 : 0) |
                    (h.z < 0 ? 16 : 0) | (h.z > h.w ? 32 : 0);
        outside &= code;
    }
    return outside == 0;
}
#endif

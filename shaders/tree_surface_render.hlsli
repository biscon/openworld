#include "lighting.hlsli"
#include "tree_types.hlsli"
[[vk::binding(0, 2)]] Texture2D<float4> treeBark;
[[vk::binding(1, 2)]] Texture2D<float4> treeLeaf;
[[vk::binding(2, 2)]] Texture2D<float4> impostorColour;
[[vk::binding(3, 2)]] Texture2D<float4> impostorNormal;
[[vk::binding(4, 2)]] Texture2D<float> impostorDepth;
[[vk::binding(5, 2)]] SamplerState treeSampler;
[[vk::binding(6, 2)]] SamplerState impostorSampler;
struct TreeFragment {
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
float2 atlasUv(float3 p, float3 d, float4 capture, uint frame, uint n, float tile) {
    float3 r, u;
    treeBasis(d, r, u);
    float2 uv = float2(dot(p - capture.xyz, r), -dot(p - capture.xyz, u)) / (2 * capture.w) + .5;
    uv = clamp(uv, .5 / tile, 1 - .5 / tile);
    return (float2(frame % n, frame / n) + uv) / n;
}
void reconstruct(TreeFragment i, out float4 colour, out float3 normal, out float ambient, out float leaf,
                 out float3 local) {
    Tree t = trees[i.index];
    TreeCell cell = treeCells[t.assetCell >> 5];
    uint asset = t.assetCell & 31;
    float4 capture = forest.capture[asset];
    uint n = forest.geometry[(asset * 4 + 3) * 2].w;
    uint width, height;
    impostorColour.GetDimensions(width, height);
    float tile = float(width) / n;
    float3 centre = treePosition(t, cell, capture.xyz, float4(0, .5, 0, 0), i.index, false);
    float3 d = treeRotate(treePush.view == 0 ? normalize(-centre) : forest.sun.xyz, -t.yaw);
    float3 ray = treePush.view == 0 ? treeRotate(normalize(i.relative), -t.yaw) : -d;
    uint3 frames;
    float3 weights;
    treeViews(d, n, frames, weights);
    colour = 0;
    normal = 0;
    ambient = 0;
    leaf = 0;
    local = 0;
    float total = 0;
    float mip = clamp(log2(tile / max(i.pixels, 1)), 0, log2(tile) - 1);
    [unroll] for (uint k = 0; k < 3; ++k) {
        uint frame = frames[k];
        float3 dir = treeOctDecode(float2(frame % n, frame / n) / float(n - 1));
        float denom = dot(ray, dir);
        denom = abs(denom) < .05 ? (denom < 0 ? -.05 : .05) : denom;
        float2 uv = atlasUv(i.local, dir, capture, frame, n, tile / exp2(ceil(mip)));
        float depth = impostorDepth.SampleLevel(impostorSampler, uv, mip);
        float delta = ((depth - .5) * 2 * capture.w - dot(i.local - capture.xyz, dir)) / denom;
        float3 p = i.local + ray * delta;
        uv = atlasUv(p, dir, capture, frame, n, tile / exp2(ceil(mip)));
        float4 c = impostorColour.SampleLevel(impostorSampler, uv, mip);
        float4 mat = impostorNormal.SampleLevel(impostorSampler, uv, mip);
        depth = impostorDepth.SampleLevel(impostorSampler, uv, mip);
        delta = ((depth - .5) * 2 * capture.w - dot(i.local - capture.xyz, dir)) / denom;
        p = i.local + ray * delta;
        float w = weights[k] * c.a;
        colour.rgb += c.rgb * w;
        colour.a += c.a * weights[k];
        normal += treeOctDecode(mat.xy) * w;
        ambient += mat.z * w;
        leaf += mat.w * w;
        local += p * w;
        total += w;
    }
    float inv = 1 / max(total, 1e-6);
    colour.rgb *= inv;
    normal = safeNormal(normal);
    ambient *= inv;
    leaf *= inv;
    local *= inv;
}
float4 treeShade(inout TreeFragment i, bool front, out float depth, out float thin) {
    Tree t = trees[i.index];
    TreeCell cell = treeCells[t.assetCell >> 5];
    uint lod = treePush.bin % 4;
    float4 colour;
    float3 normal = i.normal;
    float ao = 1, leaf = treePush.part, localHeight = i.local.y;
    if (lod == 3) {
        float3 local;
        reconstruct(i, colour, normal, ao, leaf, local);
        normal = treeRotate(normal, t.yaw);
        i.relative = treePosition(t, cell, local, float4(0, .5, 0, 0), i.index, false);
        i.rendered = curvePosition(i.relative, forest.curvature);
        if (i.previous.w > 0)
            i.previous =
                mul(temporal.previous,
                    float4(curvePosition(treePosition(t, cell, local, float4(0, .5, 0, 0), i.index, true),
                                         temporal.previousCurvature),
                           1));
    } else {
        float2 dx = ddx(i.uv), dy = ddy(i.uv);
        colour = treePush.part != 0 ? treeLeaf.SampleGrad(treeSampler, i.uv, dx, dy)
                                    : treeBark.SampleGrad(treeSampler, i.uv, dx, dy);
    }
    if (lod == 3 || treePush.part != 0)
        clip(colour.a - .4);
    float d = length(i.relative.xz), fade = 1 - smoothstep(forest.settings.x - 128, forest.settings.x, d);
    if (treePush.view != 0)
        fade = 1 - smoothstep(forest.settings.x + 896, forest.settings.x + 1024, d);
    float weight = forest.mode.x < 4 ? 1 : treeLodWeight(i.pixels, lod);
    uint2 pixel = uint2(i.position.xy);
    float rank = treeRandom(pixel.x * 1973 + pixel.y * 9277 + t.identity);
    if (forest.mode.x >= 4 && lod > 0) {
        float boundary = lod == 1 ? 320 : lod == 2 ? 120 : 64;
        if (i.pixels > boundary * .85 && i.pixels < boundary * 1.15)
            rank = 1 - rank;
    }
    clip(weight - rank);
    clip(fade - treeRandom(pixel.x * 7919 + pixel.y * 631 + t.identity));
    if (weight < .999)
        i.previous = 0;
    float4 clipPosition = mul(forest.views[treePush.view], float4(i.rendered, 1));
    depth = clipPosition.z / clipPosition.w;
    thin = leaf > .1;
#ifdef OW_TREE_DEPTH
    return 0;
#endif
    if (treePush.view != 0)
        return 0;
    normal = safeNormal(normal);
    if (lod < 3 && treePush.part != 0 && dot(normal, -i.relative) < 0)
        normal = -normal;
    if (lod < 2 && treePush.part == 0) {
        float2 delta = float2(1.0 / 1024, 0);
        float h0 = dot(colour.rgb, float3(.2126, .7152, .0722));
        float hu = dot(treeBark.Sample(treeSampler, i.uv + delta).rgb, float3(.2126, .7152, .0722));
        float hv = dot(treeBark.Sample(treeSampler, i.uv + delta.yx).rgb, float3(.2126, .7152, .0722));
        float3 tangent = safeNormal(i.tangent.xyz);
        float3 bitangent = cross(normal, tangent) * i.tangent.w;
        normal = safeNormal(normal - (tangent * (hu - h0) + bitangent * (hv - h0)) * 2);
    }
    SurfaceMaterial m;
    m.baseColour = colour.rgb * lerp(.88, 1.08, t.variation);
    m.normal = normal;
    m.roughness = leaf > .5 ? .82 : .92;
    m.metallic = 0;
    m.ao = ao;
    m.emissive = 0;
    m.opacity = 1;
    if (forest.mode.z != 0) {
        float3 colours[4] = {float3(.9, .2, .1), float3(.2, .8, .15), float3(.1, .4, 1), float3(.8, .2, .9)};
        return float4(colours[lod], 1);
    }
    return float4(
        shadeSurface(m, i.relative, screenVisibility(i.position.xy), i.rendered, leaf * .28, normal), 1);
}

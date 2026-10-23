#include "terrain_common.hlsli"
#include "terrain_evaluate.hlsli"
[[vk::binding(1, 0)]] RWStructuredBuffer<float2> heights;
struct Tile { uint4 unused; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    id.xy += tile.offset.xy;
    uint width = params.dimensions.x*params.dimensions.w+3+2*params.sampling.x;
    if (id.x>=width || id.y>=width) return;
    float2 local = (float2(id.xy)-float(params.sampling.x+1))*(params.shape.x/params.dimensions.w);
    heights[id.y*width+id.x] = evaluateTerrain(local);
}

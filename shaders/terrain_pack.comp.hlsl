#include "terrain_common.hlsli"
[[vk::binding(1,0)]] RWStructuredBuffer<float2> outputHeights;
[[vk::binding(2,0)]] StructuredBuffer<Sample> samples;
struct Tile { uint4 unused; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    id.xy+=tile.offset.xy;
    uint width=params.dimensions.x+1;
    if(any(id.xy>=width)) return;
    uint i=id.y*width+id.x;
    outputHeights[i]=float2(samples[i].base.w,samples[i].eroded.w);
}

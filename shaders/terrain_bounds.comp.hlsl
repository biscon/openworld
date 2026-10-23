#include "terrain_common.hlsli"
[[vk::binding(2,0)]] StructuredBuffer<Sample> samples;
[[vk::binding(11,0)]] RWStructuredBuffer<float2> bounds;
struct Tile { uint4 unused; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
groupshared float2 reduction[64];
[numthreads(64,1,1)]
void main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex) {
    group.xy+=tile.offset.xy;
    uint cells=params.sampling.y, width=params.dimensions.x+1;
    float2 b=float2(3.402823e38,-3.402823e38);
    for(uint i=thread;i<(cells+1)*(cells+1);i+=64) {
        uint2 p=group.xy*cells+uint2(i%(cells+1),i/(cells+1));
        Sample s=samples[p.y*width+p.x];
        b.x=min(b.x,min(s.base.w,s.eroded.w)); b.y=max(b.y,max(s.base.w,s.eroded.w));
    }
    reduction[thread]=b;
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride>0;stride>>=1) {
        if(thread<stride) reduction[thread]=float2(min(reduction[thread].x,reduction[thread+stride].x),
                                                  max(reduction[thread].y,reduction[thread+stride].y));
        GroupMemoryBarrierWithGroupSync();
    }
    if(thread==0) bounds[group.y*(params.dimensions.x/cells)+group.x]=reduction[0]+float2(-0.01,0.01);
}

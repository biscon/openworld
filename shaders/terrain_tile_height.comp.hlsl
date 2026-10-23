#include "terrain_common.hlsli"
#ifdef OW_HEIGHT_FIXTURE
// Owned analytic ridges, independent of content assets and the procedural world.
float2 evaluateTerrain(float2 p) {
    float centre=32+.37*p.y+float(params.dimensions.y)*.17;
    if((params.dimensions.y&8)!=0) centre+=3*sin(p.y*.08);
    float d=(p.x-centre)/1.5;
    return (params.shape.y*exp(-d*d)).xx;
}
#else
#include "terrain_evaluate.hlsli"
#endif
#include "terrain_height_levels.hlsli"
[[vk::binding(1,0)]] RWStructuredBuffer<float2> outputHeights;
struct Tile { uint width,halo,unused,level; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
groupshared float2 stencil[19*19];
[numthreads(8,8,1)]
void main(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID,uint thread:SV_GroupIndex) {
    uint2 origin=group.xy*8+tile.offset.xy;
    float spacing=params.shape.x*float(1u<<tile.level);
    bool filtered=(params.sampling.w&2)==0 && tile.level+(params.sampling.w>>8)!=0;
    if(filtered) {
        for(uint i=thread;i<19*19;i+=64)
            stencil[i]=evaluateTerrain((float2(origin)+(float2(i%19,i/19)-2)*.5)*spacing);
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 id=origin+local.xy;
    if(any(id>=tile.width)) return;
    float2 h=0;
    if(!filtered) h=evaluateTerrain(float2(id)*spacing);
    else {
        const float w[5]={1,4,6,4,1};
        [unroll] for(uint z=0;z<5;++z) [unroll] for(uint x=0;x<5;++x)
            h+=stencil[(local.y*2+z)*19+local.x*2+x]*(w[x]*w[z]);
        h*=1.0/256.0;
    }
    outputHeights[heightLevelOffset(params.dimensions.x,tile.level)+id.y*tile.width+id.x]=h;
}

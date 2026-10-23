#include "terrain_common.hlsli"
#ifdef OW_NORMAL_FIXTURE
// Compile-time diagnostic only: the production normal kernel with owned terrain.
float2 evaluateTerrain(float2 p) {
    float wave=.3*sin(p.x*1.57079632679)+.2*cos(p.y*.78539816339);
    float d=(p.x-8-.37*p.y)/1.5;
    return float2(.1*p.x+wave, .1*p.x+wave+3*exp(-d*d));
}
[[vk::binding(2,0)]] RWStructuredBuffer<Sample> normalSamples;
#else
#include "terrain_evaluate.hlsli"
[[vk::binding(4,0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> normals;
#endif
struct Tile { uint width,halo,unused,level; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
// Share the bounded height stencil across an 8x8 output group. A filtered
// level needs 21x21 heights, rather than 36 terrain evaluations per normal.
groupshared float2 sampledHeights[21*21];

void normalAt(uint2 p,uint width,float delta,out float3 a,out float3 b) {
    uint i=p.y*width+p.x;
    float2 dx=(sampledHeights[i+1]-sampledHeights[i-1])/(2*delta);
    float2 dz=(sampledHeights[i+width]-sampledHeights[i-width])/(2*delta);
    a=normalize(float3(-dx.x,1,-dz.x)); b=normalize(float3(-dx.y,1,-dz.y));
}
[numthreads(8,8,1)]
void main(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID,uint thread:SV_GroupIndex) {
    bool filtered=tile.level+(params.sampling.w>>8)!=0;
    uint width=filtered?21:10, factor=filtered?2:1, halo=filtered?2:1;
    float spacing=params.shape.x/params.dimensions.w*float(1u<<tile.level);
    float delta=spacing/factor;
    uint2 origin=group.xy*8+tile.offset.xy;
    float2 base=(float2(origin)-tile.halo)*spacing;
    // Shading retains sub-mesh relief. Filtering geometry heights here as well
    // would erase it twice; only the normal-resolution filter below is applied.
    for(uint i=thread;i<width*width;i+=64) {
        float2 p=base+(float2(i%width,i/width)-halo)*delta;
        sampledHeights[i]=evaluateTerrain(p);
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 id=origin+local.xy;
    if(any(id>=tile.width)) return;
    uint2 centre=local.xy*factor+halo;
    float3 a=0,b=0;
    if(!filtered) normalAt(centre,width,delta,a,b);
    else {
        [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
            float3 na,nb; normalAt(uint2(int2(centre)+int2(x,y)),width,delta,na,nb);
            float w=(x==0?2:1)*(y==0?2:1); a+=na*w; b+=nb*w;
        }
    }
#ifdef OW_NORMAL_FIXTURE
    Sample result;
    result.base=float4(normalize(a),0); result.eroded=float4(normalize(b),0);
    normalSamples[id.y*tile.width+id.x]=result;
#else
    normals[uint3(id,0)]=float4(normalize(a),0);
    normals[uint3(id,1)]=float4(normalize(b),0);
#endif
}

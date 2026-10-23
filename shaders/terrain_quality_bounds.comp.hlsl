#include "terrain_common.hlsli"
#include "terrain_evaluate.hlsli"
#include "terrain_height_levels.hlsli"
[[vk::binding(1,0)]] StructuredBuffer<float2> heights;
[[vk::binding(11,0)]] RWStructuredBuffer<float2> metadata;
struct Tile { uint width,halo,unused,level; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
groupshared float4 reduction[64];
float2 triangleHeight(float2 p,uint level) {
    uint cells=params.dimensions.x>>level,width=cells+1,offset=heightLevelOffset(params.dimensions.x,level);
    uint2 a=min(uint2(p),cells-1);float2 f=p-a;
    float2 h00=heights[offset+a.y*width+a.x],h10=heights[offset+a.y*width+a.x+1];
    float2 h01=heights[offset+(a.y+1)*width+a.x],h11=heights[offset+(a.y+1)*width+a.x+1];
    // The patch topology uses the (0,0)-(1,1) diagonal.
    return f.x>=f.y ? h00+(h10-h00)*f.x+(h11-h10)*f.y : h00+(h11-h01)*f.x+(h01-h00)*f.y;
}
[numthreads(64,1,1)]
void main(uint3 group:SV_GroupID,uint thread:SV_GroupIndex) {
    group.xy+=tile.offset.xy;
    uint patch=params.sampling.y,n=patch*2+1;
    float4 value=float4(3.402823e38,-3.402823e38,0,0);
    for(uint i=thread;i<n*n;i+=64) {
        float2 p=float2(group.xy*patch)+float2(i%n,i/n)*.5;
        float2 reference=evaluateTerrain(p*(params.shape.x*float(1u<<tile.level)));
        float2 h=triangleHeight(p,tile.level), parent=triangleHeight(p*.5,tile.level+1);
        value.xy=float2(min(value.x,min(min(h.x,h.y),min(parent.x,parent.y))),
                       max(value.y,max(max(h.x,h.y),max(parent.x,parent.y))));
        value.zw=max(value.zw,abs(h-reference));
    }
    reduction[thread]=value;
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride>0;stride>>=1) {
        if(thread<stride) {
            float4 b=reduction[thread+stride],a=reduction[thread];
            reduction[thread]=float4(min(a.x,b.x),max(a.y,b.y),max(a.zw,b.zw));
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(thread==0) {
        uint leaves=params.dimensions.x/patch,offset=leaves*leaves;
        for(uint l=0;l<tile.level;++l){uint w=leaves>>l;offset+=2*w*w;}
        uint idx=group.y*tile.width+group.x;
        metadata[offset+idx*2]=reduction[0].xy+float2(-.01,.01);
        metadata[offset+idx*2+1]=reduction[0].zw;
        if(tile.level==0) metadata[idx]=reduction[0].xy+float2(-.01,.01);
    }
}

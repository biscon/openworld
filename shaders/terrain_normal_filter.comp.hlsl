#include "terrain_common.hlsli"
[[vk::binding(4,0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> outputNormals;
[[vk::binding(8,0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> inputNormals;
struct Filter { uint width; uint halo; uint previousHalo; uint level; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Filter> filter;
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    id.xy += filter.offset.xy;
    if(any(id.xy>=filter.width)) return;
    int2 centre=(int2(id.xy)-int(filter.halo))*2+int(filter.previousHalo);
    [unroll] for(uint layer=0;layer<2;++layer) {
        float3 n=0;
        [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
            n+=inputNormals[int3(centre+int2(x,y),layer)].xyz*float((x==0?2:1)*(y==0?2:1));
        outputNormals[uint3(id.xy,layer)]=float4(normalize(n),0);
    }
}

#include "post_common.hlsli"
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID){
    uint2 p=tid.xy;if(any(p>=settings.dims.xy))return;
    float3 high=source.Load(int3(p,0)).rgb;
    if(settings.pass.x!=0){result[p]=float4(high,1);return;}
    uint width,height;sourceDepth.GetDimensions(width,height);float2 uv=(p+.5)/settings.dims.xy;
    float3 low=0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
        low+=sourceDepth.SampleLevel(linearClamp,uv+float2(x,y)*settings.effect.w/float2(width,height),0).rgb*((x==0?2:1)*(y==0?2:1)/16.);
    result[p]=float4(finiteHdr((high+low)*.5),1);
}

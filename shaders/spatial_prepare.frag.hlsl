#include "spatial_common.hlsli"
#include "tonemap.hlsli"
float4 main(float4 position:SV_Position):SV_Target0 {
    float2 uv=position.xy*settings.metrics.xy;
    float3 c=max(source.Load(int3(uint2(position.xy),0)).rgb,0);
    if(settings.bloomIntensity>0)c+=auxiliary.SampleLevel(linearClamp,uv,0).rgb*settings.bloomIntensity;
    return float4(settings.mapper==0?acesFitted(c):agx(c),1);
}

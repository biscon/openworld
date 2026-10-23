#include "tonemap.hlsli"
[[vk::binding(0,0)]] Texture2D<float4> hdr;
[[vk::binding(1,0)]] Texture2D<float4> gtao;
[[vk::binding(2,0)]] SamplerState linearClamp;
struct Output { uint diagnostic, mapper, aoDiagnostic, outputWidth, outputHeight; };
[[vk::push_constant]] ConstantBuffer<Output> settings;
float4 main(float4 position:SV_Position):SV_Target0 {
    float2 uv=position.xy/float2(settings.outputWidth,settings.outputHeight);
    uint width,height;
    hdr.GetDimensions(width,height);
    uint2 pixel=min(uint2(uv*float2(width,height)),uint2(width-1,height-1));
    if(settings.aoDiagnostic!=0)return gtao.Load(int3(pixel,0));
    float3 c=max(settings.diagnostic!=0?hdr.Load(int3(pixel,0)).rgb:
                 hdr.SampleLevel(linearClamp,uv,0).rgb,0);
    return float4(settings.diagnostic!=0?c:settings.mapper==0?acesFitted(c):agx(c),1);
}

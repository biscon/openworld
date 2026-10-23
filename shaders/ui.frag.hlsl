[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float4> uiTexture;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState uiSampler;
struct Input { [[vk::location(0)]] float4 colour:COLOR0; [[vk::location(1)]] float2 uv:TEXCOORD0; };
float4 main(Input i):SV_Target0 {
    float4 c=i.colour*uiTexture.Sample(uiSampler,i.uv);
    float3 lo=c.rgb/12.92,hi=pow((c.rgb+.055)/1.055,2.4);
    return float4(float3(c.r<=.04045?lo.r:hi.r,c.g<=.04045?lo.g:hi.g,c.b<=.04045?lo.b:hi.b),c.a);
}

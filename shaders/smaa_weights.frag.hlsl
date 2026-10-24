#include "smaa_common.hlsli"
float4 main(float4 position:SV_Position):SV_Target0 {
    float2 uv=position.xy*settings.metrics.xy,pixel;float4 offsets[3];
    SMAABlendingWeightCalculationVS(uv,pixel,offsets);
    return SMAABlendingWeightCalculationPS(uv,pixel,offsets,source,areaTex,searchTex,0);
}

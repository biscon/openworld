#include "smaa_common.hlsli"
float4 main(float4 position:SV_Position):SV_Target0 {
    float2 uv=position.xy*settings.metrics.xy;float4 offset;
    SMAANeighborhoodBlendingVS(uv,offset);
    return SMAANeighborhoodBlendingPS(uv,offset,source,auxiliary);
}

#include "smaa_common.hlsli"
float2 main(float4 position:SV_Position):SV_Target0 {
    float2 uv=position.xy*settings.metrics.xy;float4 offsets[3];
    SMAAEdgeDetectionVS(uv,offsets);
    return SMAALumaEdgeDetectionPS(uv,offsets,source);
}

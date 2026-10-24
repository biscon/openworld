// This software contains source code provided by NVIDIA Corporation.
#include "spatial_common.hlsli"
#define FXAA_PRESET 3
#define FXAA_HLSL_4 1
#include "../third_party/fxaa/FxaaShader.h"
float4 main(float4 position:SV_Position):SV_Target0 {
    FxaaTex tex = {linearClamp,source};
    return float4(FxaaPixelShader(position.xy*settings.metrics.xy,tex,settings.metrics.xy),1);
}

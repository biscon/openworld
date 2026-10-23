// Offline diagnostic output. Separate pipeline keeps probe-only work out of
// production height generation; both paths evaluate the same terrain function.
#include "terrain_common.hlsli"
#include "terrain_evaluate.hlsli"
[[vk::binding(2, 0)]] RWStructuredBuffer<Sample> samples;
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width=params.dimensions.x+1;
    if(any(id.xy>=width)) return;
    float2 local=float2(id.xy)*params.shape.x;
    BaseTerrainSample base=evaluateBaseTerrain(local);
    Sample s;
    s.base=float4(base.gradient,base.mountainWeight,base.height);
    s.eroded=float4(base.peakValley,base.erosionGradient,evaluateTerrain(local,base).y);
    samples[id.y*width+id.x]=s;
}

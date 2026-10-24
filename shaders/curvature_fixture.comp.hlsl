// Owned numerical probe. Deliberately perturb trig accuracy independently of
// the driver's implementation, while exercising the production mapping.
#include "terrain_common.hlsli"
struct Probe { float4 curvature; uint4 options; };
[[vk::push_constant]] ConstantBuffer<Probe> probe;
[[vk::binding(1,0)]] StructuredBuffer<Sample> inputs;
[[vk::binding(2,0)]] RWStructuredBuffer<Sample> outputs;
float fixtureSin(float a) {
    float s=sin(a);
    return probe.options.y!=0?round(s*1e7)*1e-7:s;
}
#define sin fixtureSin
#include "curvature.hlsli"
#undef sin
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=probe.options.x)return;
    Sample s=inputs[id.x];
    Sample result;
    result.base=float4(curvePosition(s.base.xyz,probe.curvature),1);
    result.eroded=float4(curveNormal(s.base.xyz,s.eroded.xyz,probe.curvature),1);
    outputs[id.x]=result;
}

#ifndef OW_TEMPORAL_SURFACE
#define OW_TEMPORAL_SURFACE
#include "temporal_types.hlsli"
#include "temporal_motion.hlsli"
#include "curvature.hlsli"
#include "volumetrics_common.hlsli"
[[vk::binding(14,1)]] ConstantBuffer<TemporalView> temporal;
struct TemporalTerrainRecord {uint4 draw[8];float4 weights[12];float4 observer,camera,valid;};
[[vk::binding(15,1)]] StructuredBuffer<TemporalTerrainRecord> temporalTerrain;
struct TemporalOutput {float4 colour:SV_Target0;float4 motion:SV_Target1;float reactive:SV_Target2;};
TemporalOutput temporalOutput(float4 colour,float3 current,float4 previous,float valid,float reactive,bool thin=false) {
    TemporalOutput o;o.colour=colour;o.motion=0;o.reactive=max(reactive,atmosphere.options.x==2?atmosphere.view.z:0);
    o.reactive=max(o.reactive,fogReactivity(current));
    if(temporal.state.x==0)return o;
    float4 clip=mul(temporal.current,float4(current,1));
    if(valid>0 && previous.w>0 && previous.w<65504000 && clip.w>0 && all(isfinite(previous)))
        o.motion=float4((previous.xy/previous.w-clip.xy/clip.w)*.5,previous.w*(temporal.curvature.z!=0?.001:1),1);
    if(thin)o.motion.w=temporalMotionValid(o.motion.w)?3:-1;
    return o;
}
#endif

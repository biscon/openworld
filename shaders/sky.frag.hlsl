#include "volumetrics_common.hlsli"
struct SkyVertex {float4 position:SV_Position;float3 direction:TEXCOORD0;};
#ifdef OW_SKY_TEMPORAL
struct SkyOutput {float4 colour:SV_Target0;float4 motion:SV_Target1;float reactive:SV_Target2;};
SkyOutput main(SkyVertex i){
    float3 d=normalize(i.direction);float4 now=mul(atmosphere.current,float4(d,0)),old=mul(atmosphere.previous,float4(d,0));
    SkyOutput o;o.colour=float4(mediaSky(d),1);
    o.motion=float4((old.xy/max(old.w,1e-6)-now.xy/now.w)*.5,0,old.w>0?2:0);
    o.reactive=max(atmosphere.view.z,fog.grid.w!=0?fogReactivity(d*(fog.depth.x/max(.001,now.w))):0);return o;
}
#else
float4 main(SkyVertex i):SV_Target0{return float4(mediaSky(normalize(i.direction)),1);}
#endif

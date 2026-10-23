// Camera-centred immutable latitude/longitude dome, generated without vertex storage.
#include "atmosphere_common.hlsli"
struct SkyVertex {float4 position:SV_Position;float3 direction:TEXCOORD0;};
SkyVertex main(uint id:SV_VertexID){
    const uint longitude=64,latitude=32;
    uint cell=id/6,corner=id%6;uint2 offsets[6]={uint2(0,0),uint2(1,0),uint2(1,1),uint2(0,0),uint2(1,1),uint2(0,1)};
    float2 uv=(float2(cell%longitude,cell/longitude)+offsets[corner])/float2(longitude,latitude);
    float a=uv.x*2*atmPi,e=(uv.y-.5)*atmPi;
    float3 d=float3(cos(e)*cos(a),sin(e),cos(e)*sin(a));
    // Project direction only. The inverse of inverseRaster is the jittered camera projection.
    float4 clip=mul(atmosphere.current,float4(d,0));clip.xy+=atmosphere.haze.zw*2*clip.w;
    SkyVertex o;o.position=float4(clip.xy,0,clip.w);o.direction=d;return o;
}

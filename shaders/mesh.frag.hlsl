#include "temporal_surface.hlsli"
#include "mesh_surface.hlsli"
float4 shadePixel(MeshSurface i,bool front:SV_IsFrontFace):SV_Target0 {
    MaterialRecord r=materialRecords[i.material];float2 dx=ddx(i.uv),dy=ddy(i.uv);
    float4 base=meshBase(r,i.uv,dx,dy);SurfaceMaterial m=meshSurface(i,r,base,dx,dy,front);
    if(lighting.output.y!=0 || lighting.output.w!=0)return float4(m.baseColour*(.22+.78*saturate(dot(m.normal,normalize(float3(-.6,.8,.35)))))*m.opacity,m.opacity);
    float3 colour=r.alpha.w!=0?min((m.baseColour+m.emissive)*lighting.output.x,65504):shadeSurface(m,i.relative,r.alpha.x==2?1:screenVisibility(i.position.xy),i.renderedPosition);
    if(r.alpha.x==2)colour=mediaApply(colour,i.renderedPosition);
    return float4(colour*m.opacity,m.opacity);
}

#ifdef OW_TEMPORAL_OUTPUT
TemporalOutput main(MeshSurface i,bool front:SV_IsFrontFace) {
    MaterialRecord r=materialRecords[i.material];
    float4 colour=shadePixel(i,front);
    float reactive=r.alpha.x==2?saturate(colour.a*4):saturate(dot(r.emission.rgb,float3(.2126,.7152,.0722))/10000);
    return temporalOutput(colour,i.renderedPosition,i.previousClip,float(i.previousClip.w>0),reactive);
}

#else
float4 main(MeshSurface i,bool front:SV_IsFrontFace):SV_Target0 { return shadePixel(i,front); }
#endif

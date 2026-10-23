#include "mesh_surface.hlsli"
float4 main(MeshSurface i,bool front:SV_IsFrontFace):SV_Target0 {
    MaterialRecord r=materialRecords[i.material];float2 dx=ddx(i.uv),dy=ddy(i.uv);
    float4 base=meshBase(r,i.uv,dx,dy);SurfaceMaterial m=meshSurface(i,r,base,dx,dy,front);
    if(lighting.output.y!=0 || lighting.output.w!=0)return float4(m.baseColour*(.22+.78*saturate(dot(m.normal,normalize(float3(-.6,.8,.35)))))*m.opacity,m.opacity);
    float3 colour=r.alpha.w!=0?min((m.baseColour+m.emissive)*lighting.output.x,65504):shadeSurface(m,i.relative,r.alpha.x==2?1:screenVisibility(i.position.xy));
    return float4(colour*m.opacity,m.opacity);
}

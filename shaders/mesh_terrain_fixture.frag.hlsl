// Tool-owned terrain sampling workload on ordinary mesh geometry.
#include "mesh_surface.hlsli"
float4 main(MeshSurface i):SV_Target0 {
    float4 weights=materialRecords[i.material].base;
    if(materialInfo.options.x==0)return float4(shadeSurface(terrainMaterial(i.normal),i.relative,screenVisibility(i.position.xy)),1);
    if(materialInfo.options.y!=0)return float4(weightColour(weights),1);
    return float4(displayMappedMaterial(evaluateMappedMaterial(safeNormal(i.normal),i.relative,weights),i.relative,screenVisibility(i.position.xy)),1);
}

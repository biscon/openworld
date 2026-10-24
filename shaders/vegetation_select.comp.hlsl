#include "vegetation_types.hlsli"
[[vk::binding(2,0)]] RWStructuredBuffer<uint> visiblePlants;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> work;
[[vk::binding(5,0)]] StructuredBuffer<uint4> groups;
[[vk::binding(6,0)]] StructuredBuffer<uint> casterIndices;
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint view=plantPush.view,base=view*plantViewWords;
    uint4 list=vegetation.lists[view];
    if(plantPush.phase==1) {
        if(group.x!=0||lane!=0)return;
        uint offset=0;
        for(uint b=0;b<vegetation.limits.z;++b) {
            uint j=base+b*8,count=work[j+5],bounded=min(count,list.y-min(offset,list.y));
            work[j]=vegetation.geometry[b].x;work[j+1]=bounded;work[j+2]=vegetation.geometry[b].y;
            work[j+3]=0;work[j+4]=0;work[j+6]=0;work[j+7]=offset;offset+=bounded;
            work[base+plantViewWords-1]+=count-bounded;
        }
        return;
    }
    uint4 range=groups[list.z+group.x];
    if(lane>=range.y)return;
    uint id=range.w!=0?casterIndices[range.x+lane]:range.x+lane;
    uint cellIndex=vegetation.inspection.y!=0?id/128:range.w!=0?plants[id].assetCell>>3:range.z;
    Cell cell=cells[cellIndex];
    if(vegetation.inspection.y!=0&&cell.origin.w==0)return;
    Plant p=loadPlant(id);
    if(p.rootScale.w<=0)return;
    float3 cellLo=view==0?cell.lo.xyz:cell.casterLo.xyz,cellHi=view==0?cell.hi.xyz:cell.casterHi.xyz;
    float3 relative=p.rootScale.xyz+cell.origin.xyz;float d=length(relative);
    uint asset=plantAsset(p,vegetation.modes.x,d),lod=plantLod(p,d,asset);
    if(!plantBoxVisible(cellLo+cell.origin.xyz,cellHi+cell.origin.xyz,view))return;
    if(plantCoverage(p,d,asset,vegetation.settings,vegetation.cards)<=0)return;
    float shadowEnd=vegetation.profile.x!=0?vegetation.settings.w:vegetation.ranges[asset].w;
    if(view!=0&&(shadowEnd<=0||d>=shadowEnd))return;
    float4 e=vegetation.envelope[asset]*p.rootScale.w;
    float extra=dot(abs(plantSlope),float2(e.x,e.x));
    float3 lo=relative+float3(-e.x,e.y-extra,-e.x),hi=relative+float3(e.x,e.z+extra,e.x);
    if(vegetation.profile.x!=0){lo=relative-float3(.9,.05,.9);hi=relative+float3(.9,.65,.9);}
    if(!plantBoxVisible(lo,hi,view))return;
    uint4 bins=vegetation.lods[asset*3+lod];
    for(uint part=0;part<bins.y;++part) {
        uint j=base+(bins.x+part)*8,slot;
        if(plantPush.phase==0)InterlockedAdd(work[j+5],1,slot);
        else {InterlockedAdd(work[j+6],1,slot);if(slot<work[j+1])visiblePlants[list.x+work[j+7]+slot]=id;}
    }
}

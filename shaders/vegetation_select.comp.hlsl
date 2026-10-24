#include "vegetation_types.hlsli"
[[vk::binding(2,0)]] RWStructuredBuffer<uint> visiblePlants;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> work;
struct Cell {float4 lo;float4 hi;};
[[vk::binding(4,0)]] StructuredBuffer<Cell> cells;
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint view=plantPush.view,base=view*97;
    if(plantPush.phase==1){
        if(group.x!=0||lane!=0)return;
        uint offset=0;
        for(uint b=0;b<12;b++){
            uint j=base+b*8,count=work[j+5];
            uint bounded=min(count,vegetation.limits.x-min(offset,vegetation.limits.x));
            work[j]=vegetation.geometry[b].x;work[j+1]=bounded;
            work[j+2]=vegetation.geometry[b].y;work[j+3]=0;work[j+4]=0;
            work[j+6]=0;work[j+7]=offset;offset+=bounded;
            work[base+96]+=count-bounded;
        }
        return;
    }
    Cell cell=cells[group.x];
    if(!plantBoxVisible(cell.lo.xyz-vegetation.cameraTime.xyz,cell.hi.xyz-vegetation.cameraTime.xyz,view))return;
    uint id=group.x*128+lane;if(id>=vegetation.modes.w)return;
    Plant p=plants[id];float3 relative=p.rootScale.xyz-vegetation.cameraTime.xyz;
    float d=length(relative);uint bin=plantBin(p,d,vegetation.modes.x);
    if(plantCoverage(p,d,bin,vegetation.settings,vegetation.cards)<=0)return;
    if(view!=0 && d>=vegetation.settings.w)return;
    if(!plantBoxVisible(relative-float3(.9,.05,.9),relative+float3(.9,.65,.9),view))return;
    uint j=base+bin*8,slot;
    if(plantPush.phase==0)InterlockedAdd(work[j+5],1,slot);
    else {
        InterlockedAdd(work[j+6],1,slot);
        if(slot<work[j+1])visiblePlants[view*vegetation.limits.x+work[j+7]+slot]=id;
    }
}

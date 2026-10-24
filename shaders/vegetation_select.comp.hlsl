#include "vegetation_types.hlsli"
[[vk::binding(2,0)]] RWStructuredBuffer<uint> visiblePlants;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> work;
[[vk::binding(5,0)]] StructuredBuffer<uint4> groups;
[[vk::binding(6,0)]] StructuredBuffer<uint> casterIndices;
[[vk::binding(8,0)]] RWStructuredBuffer<uint> prefixes;
[[vk::binding(9,0)]] RWStructuredBuffer<uint> choices;
#ifdef OW_VEGETATION_HIZ
[[vk::binding(10,0)]] Texture2D<float> plantPyramid;
bool plantOccluded(float3 lo,float3 hi) {
    plantCurveBounds(lo,hi);
    float2 a=1e30,b=-1e30;float nearest=0;
    [unroll]for(uint i=0;i<8;++i) {
        float3 p=float3(i&1?hi.x:lo.x,i&2?hi.y:lo.y,i&4?hi.z:lo.z);
        float4 h=mul(vegetation.raster,float4(p,1));
        float e=max(1e-5,9.5367431640625e-7*max(max(abs(h.x),abs(h.y)),max(abs(h.z),abs(h.w))));
        if(!all(isfinite(h))||h.w<=e||h.z>=h.w-e)return false;
        float3 ndc=h.xyz/h.w;a=min(a,ndc.xy);b=max(b,ndc.xy);nearest=max(nearest,ndc.z);
    }
    if(any(b < -1)||any(a > 1))return false;
    uint2 extent=vegetation.pyramidSize.xy;
    int2 ia=clamp(int2(floor((clamp(a,-1.0,1.0)*.5+.5)*extent))-1,0,int2(extent)-1);
    int2 ib=clamp(int2(ceil((clamp(b,-1.0,1.0)*.5+.5)*extent))+1,0,int2(extent)-1);
    uint level=0;uint2 ma=ia,mb=ib;
    [loop]while(any(mb-ma>1)&&level+1<vegetation.pyramidSize.z) {
        ++level;uint2 size=max(uint2(1,1),extent>>level);
        ma=min(uint2(ia)>>level,size-1);mb=min(uint2(ib)>>level,size-1);
    }
    float depth=1;
    [loop]for(uint y=ma.y;y<=mb.y;++y)
        [loop]for(uint x=ma.x;x<=mb.x;++x)depth=min(depth,plantPyramid.Load(int3(x,y,level)));
    return nearest < depth-max(1e-6,9.5367431640625e-7*max(abs(nearest),abs(depth)));
}
#endif
groupshared uint binCounts[48];
groupshared uint selected[128];
groupshared uint cellVisible;
uint selection(uint id,uint cellIndex,uint view) {
    Cell cell=cells[cellIndex];
    if(vegetation.inspection.y!=0&&cell.origin.w==0)return 0;
    float3 cellLo=view==0?cell.lo.xyz:cell.casterLo.xyz,cellHi=view==0?cell.hi.xyz:cell.casterHi.xyz;
    if(view!=0&&!plantBoxVisible(cellLo+cell.origin.xyz,cellHi+cell.origin.xyz,view))return 0;
    Plant p=loadPlant(id);
    if(p.rootScale.w<=0)return 0;
    float3 relative=p.rootScale.xyz+cell.origin.xyz;float d=length(relative);
    uint asset=plantAsset(p,vegetation.modes.x,d),lod=plantLod(p,d,asset);
    if(plantCoverage(p,d,asset,vegetation.settings,vegetation.cards,view)<=0)return 0;
    float shadowEnd=vegetation.profile.x!=0?vegetation.settings.w:vegetation.ranges[asset].w;
    if(view!=0&&(shadowEnd<=0||d>=shadowEnd))return 0;
    float4 e=vegetation.envelope[asset]*p.rootScale.w;
    float extra=dot(abs(plantSlope),float2(e.x,e.x));
    float3 lo=relative+float3(-e.x,e.y-extra,-e.x),hi=relative+float3(e.x,e.z+extra,e.x);
    if(vegetation.profile.x!=0){lo=relative-float3(.9,.05,.9);hi=relative+float3(.9,.65,.9);}
    if(!plantBoxVisible(lo,hi,view))return 0;
    #ifdef OW_VEGETATION_HIZ
    if(view==0&&plantOccluded(lo,hi))return 0;
    #endif
    uint4 bins=vegetation.lods[asset*3+lod];
    return bins.x|(bins.y<<8);
}
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint view=plantPush.view,base=view*plantViewWords;
    uint4 list=vegetation.lists[view];
    // One lane per bin scans bounded workgroups. Prefixes are reused between views.
    if(plantPush.phase==1) {
        if(group.x!=0||lane>=vegetation.limits.z)return;
        uint total=0;
        for(uint g=0;g<list.w;++g) {
            uint at=g*48+lane,n=prefixes[at];prefixes[at]=total;total+=n;
        }
        work[base+lane*8+5]=total;return;
    }
    if(plantPush.phase==2) {
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
    if(plantPush.phase==0) {
        if(lane<48)binCounts[lane]=0;
        if(lane==0) {
            cellVisible=1;
            if(view==0) {
                Cell c=cells[range.z];
                cellVisible=(vegetation.inspection.y==0||c.origin.w!=0)&&plantBoxVisible(c.lo.xyz+c.origin.xyz,c.hi.xyz+c.origin.xyz,view);
            }
        }
        GroupMemoryBarrierWithGroupSync();
        uint choice=0;
        if(lane<range.y&&cellVisible!=0) {
            uint id=range.w!=0?casterIndices[range.x+lane]:range.x+lane;
            uint cellIndex=vegetation.inspection.y!=0?id/128:range.w!=0?plants[id].assetCell>>3:range.z;
            choice=selection(id,cellIndex,view);
            for(uint p=0;p<(choice>>8);++p)InterlockedAdd(binCounts[(choice&255)+p],1);
        }
        choices[group.x*128+lane]=choice;
        GroupMemoryBarrierWithGroupSync();
        if(lane<vegetation.limits.z)prefixes[group.x*48+lane]=binCounts[lane];
        return;
    }
    uint choice=choices[group.x*128+lane];selected[lane]=choice;
    GroupMemoryBarrierWithGroupSync();
    for(uint part=0;part<(choice>>8);++part) {
        uint bin=(choice&255)+part,slot=prefixes[group.x*48+bin];
        for(uint prior=0;prior<lane;++prior) {
            uint c=selected[prior],first=c&255;
            slot+=bin>=first&&bin<first+(c>>8)?1:0;
        }
        uint j=base+bin*8;
        if(slot<work[j+1]) {
            uint id=range.w!=0?casterIndices[range.x+lane]:range.x+lane;
            visiblePlants[list.x+work[j+7]+slot]=id;
        }
    }
}

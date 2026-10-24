#define main populateMain
#include "vegetation_populate.comp.hlsl"
#undef main
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    populateMain(group,lane);
    Job job=jobs[group.x];uint index=job.config.x*128+lane;
    float2 local=population[index].rootScale.xz;
    float3 w=vegetationHabitatWeights(job.key,local,job.habitat.xyz,true,job.config.y);
    uint4 adjacent=job.key;adjacent.xy=habitatAdd(adjacent.xy,1);
    float3 other=vegetationHabitatWeights(adjacent,local-float2(4,0),job.habitat.xyz,true,job.config.y);
    float clearing=vegetationClearing(job.key,local,true,job.config.y);
    float otherClearing=vegetationClearing(adjacent,local-float2(4,0),true,job.config.y);
    attachment[index]=float4(w,max(max(abs(w.x-other.x),abs(w.y-other.y)),max(abs(w.z-other.z),abs(clearing-otherClearing))));
    if(lane&1) {
        float ground=vegetationGroundCover(w,clearing,true);
        float across=vegetationGroundCover(other,otherClearing,true);
        attachment[index]=float4(vegetationGroundCover(w,1,true),vegetationGroundCover(w,0,true),
                                abs(ground-across),vegetationGroundCover(float3(0,0,1),1,false));
    }
}

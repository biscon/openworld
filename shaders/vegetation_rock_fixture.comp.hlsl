// Owned integration fixture: execute real population generation, then expose
// classification in the existing diagnostic attachment readback (not a runtime path).
#define main populateMain
#include "vegetation_populate.comp.hlsl"
#undef main
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    populateMain(group,lane);
    Job job=jobs[group.x];uint index=job.config.x*128+lane;
    Plant p=population[index];
    plantGeography=job.geography;
    float rock=terrainRockWeight(length(evaluateBaseTerrain(p.rootScale.xz,true).gradient));
    // Re-express the exact same point from the adjacent 4 m cell, including
    // carry through the signed 64-bit quarter-metre origin.
    uint lo=plantGeography.worldOrigin.x;
    plantGeography.worldOrigin.x+=16;
    plantGeography.worldOrigin.y+=plantGeography.worldOrigin.x<lo?1:0;
    float adjacent=terrainRockWeight(length(evaluateBaseTerrain(p.rootScale.xz-float2(4,0),true).gradient));
    float height;float2 slope=0;bool valid=false;
    if(job.canonical.w!=0)valid=canonicalSurface(job,p.rootScale.xz,height,slope);
    attachment[index]=float4(rock,adjacent,rsqrt(1+dot(slope,slope)),valid?1:0);
}

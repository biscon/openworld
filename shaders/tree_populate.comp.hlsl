#include "terrain_geography.hlsli"
#include "terrain_rock.hlsli"
#include "terrain_stationary.hlsli"
#include "tree_ecology.hlsli"
static MaterialGeography treeGeography;
#define params treeGeography
#define OW_MATERIAL_CONTINUOUS
#include "terrain_evaluate.hlsli"
#undef OW_MATERIAL_CONTINUOUS
#undef params
struct Tree {
    float4 rootScale;
    float yaw, variation;
    uint identity, assetCell;
};
struct Patch {
    float4 rectangle;
    uint4 geometry, heights, topology;
};
struct Job {
    uint4 key, config;
    float4 ecology;
    uint4 publication;
    Patch patches[256];
    MaterialGeography geography;
};
[[vk::binding(0, 0)]] RWStructuredBuffer<Tree> population;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> attachment;
[[vk::binding(2, 0)]] StructuredBuffer<Job> jobs;
[[vk::binding(3, 0)]] StructuredBuffer<float2> heights;
#include "tree_surface.hlsli"
[numthreads(128, 1, 1)] void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    Job job = jobs[group.x];
    uint index = job.config.x * 128 + lane;
    Tree t = population[index];
    if (job.config.z != 0) {
        uint h = treeCandidateHash(job.key, job.config.y, lane);
        float2 p = treeCandidate(job.key, job.config.y, lane);
        treeGeography = job.geography;
        BaseTerrainSample base = evaluateBaseTerrain(p, true);
        float elevation = evaluateTerrain(p, base).y;
        float probability = forestCover(job.key, p, job.config.y, job.ecology.x) * job.ecology.y;
        probability *= 1 - smoothstep(job.ecology.z * .8, job.ecology.z, elevation);
        probability *= 1 - smoothstep(.55, .9, terrainRockWeight(length(base.gradient)));
        uint species = forestSpecies(job.key, p, job.config.y, elevation, job.ecology.z, base.mountainWeight);
        float cover = forestCover(job.key, p, job.config.y, job.ecology.x);
        uint variant = treeRandom(h + 9) < .18 ? uint(treeRandom(h + 10) * 2)
                       : cover > .65           ? 2 + uint(treeRandom(h + 10) * 2)
                                               : 4 + uint(treeRandom(h + 10) * 2);
        uint asset = min(species * 6 + variant, job.publication.y - 1);
        t.rootScale = float4(p.x, 0, p.y, .85 + .30 * treeRandom(h + 3));
        t.yaw = treeRandom(h + 4) * 6.2831853;
        t.variation = treeRandom(h + 5);
        t.identity = h;
        t.assetCell = (job.config.x << 5) | asset;
        if (treeRandom(h + 6) > probability || !treeSpacing(job.key, job.config.y, lane, p))
            t.rootScale.w = 0;
        population[index] = t;
    }
    float height;
    float2 slope;
    bool valid = t.rootScale.w > 0 && treeSurface(job, t.rootScale.xz, height, slope);
    float4 old = attachment[index];
    if (valid)
        attachment[index] = float4(height, job.config.z == 0 && old.w > 0 ? old.x : height,
                                   asfloat(job.publication.x), job.config.z == 0 && old.w > 0 ? 1 : 2);
    else if (job.config.z != 0)
        attachment[index] = 0; // Queued/missing repairs retain their last valid root.
}

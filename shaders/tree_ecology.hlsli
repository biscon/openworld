#ifndef OW_TREE_ECOLOGY
#define OW_TREE_ECOLOGY
#include "vegetation_habitat.hlsli"
uint treeHash(uint x) { return habitatHash(x); }
float treeRandom(uint x) { return (treeHash(x) >> 8) * (1.0 / 16777216.0); }
uint4 treeOffsetKey(uint4 k, int2 offset) {
    return uint4(habitatAdd(k.xy, offset.x), habitatAdd(k.zw, offset.y));
}
uint4 treeFourMetreKey(uint4 k) {
    return uint4(k.x << 4, (k.y << 4) | (k.x >> 28), k.z << 4, (k.w << 4) | (k.z >> 28));
}
uint treeCandidateHash(uint4 k, uint seed, uint lane) {
    uint h = treeHash(seed ^ 0x54524545u);
    h = treeHash(h ^ k.x);
    h = treeHash(h ^ k.y);
    h = treeHash(h ^ k.z);
    h = treeHash(h ^ k.w);
    return treeHash(h ^ lane);
}
float2 treeCandidate(uint4 k, uint seed, uint lane) {
    uint h = treeCandidateHash(k, seed, lane);
    return min((float2(lane % 8, (lane / 8) % 8) + float2(treeRandom(h + 1), treeRandom(h + 2))) * 8,63.999996.xx);
}
bool treeSpacing(uint4 key, uint seed, uint lane, float2 p) {
    uint rank = treeCandidateHash(key, seed, lane);
    int2 base = int2(lane % 8, (lane / 8) % 8);
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x) {
            int2 g = base + int2(x, z), off = int2(g.x < 0    ? -1
                                                   : g.x >= 8 ? 1
                                                              : 0,
                                                   g.y < 0    ? -1
                                                   : g.y >= 8 ? 1
                                                              : 0);
            uint4 k = treeOffsetKey(key, off);
            uint2 local = uint2((g + 8) % 8);
            for (uint n = 0; n < 2; ++n) {
                uint candidate = local.y * 8 + local.x + n * 64;
                uint other = treeCandidateHash(k, seed, candidate);
                float2 q = treeCandidate(k, seed, candidate) + float2(off) * 64;
                if (other < rank && dot(p - q, p - q) < 9)
                    return false;
            }
        }
    return true;
}
float treeNoise(uint4 key, float2 p, uint scale, uint seed) {
    return habitatNoise(treeFourMetreKey(key), p, scale, seed);
}
float forestCover(uint4 key, float2 p, uint seed, float coverage) {
    float n = treeNoise(key, p, 8, seed ^ 8171u);
    float clear = treeNoise(key, p, 4, seed ^ 317u);
    return smoothstep(1 - coverage - .14, 1 - coverage + .14, n) * (1 - smoothstep(.72, .88, clear));
}
uint forestSpecies(uint4 key, float2 p, uint seed, float elevation, float treeline, float mountain) {
    float wet = treeNoise(key, p, 8, seed ^ 1753u), stand = treeNoise(key, p, 6, seed ^ 9511u);
    float high = smoothstep(treeline * .30, treeline * .85, elevation);
    float pine = saturate(high * .75 + mountain * .15 + (1 - wet) * .25);
    float edgeMix = (treeNoise(key, p, 1, seed ^ 6739u) - .5) * .12;
    if (stand + edgeMix < pine)
        return 3;
    float edge = treeNoise(key, p, 5, seed ^ 4327u);
    if (edge + edgeMix > .68)
        return 2;
    return wet + edgeMix > .49 ? 0 : 1;
}
#endif

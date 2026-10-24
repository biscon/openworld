#ifndef OW_VEGETATION_HABITAT
#define OW_VEGETATION_HABITAT
// Stable habitat probabilities, independent of quality, visibility and residency.
float vegetationProbability(float3 weights,uint species) {
    return dot(weights,species==0?float3(1,.65,.35):species==1?float3(.15,.25,1):float3(.15,.75,.4));
}
bool vegetationExcluded(float2 relative,float3 clearing) {
    return clearing.z>0&&distance(relative,clearing.xy)<clearing.z;
}
#endif

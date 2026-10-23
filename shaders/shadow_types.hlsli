#ifndef OW_SHADOW_TYPES
#define OW_SHADOW_TYPES
struct ShadowUniform {
    column_major float4x4 matrices[5];
    float4 ranges[5];
    float4 forward,sun,sampling;
    uint4 options;
};
#endif

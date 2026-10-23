#ifndef OW_TERRAIN_COMMON
#define OW_TERRAIN_COMMON
struct NoiseDomain { int4 lattice; float4 fractionFrequency; };
struct Parameters {
    uint4 dimensions;
    float4 shape;
    NoiseDomain domains[13];
    uint4 sampling;
    uint4 worldOrigin;
    float4 geography; // erosion wavelength, ridge strength, base detail, reserved
};
[[vk::binding(0, 0)]] ConstantBuffer<Parameters> params;
struct Sample { float4 base; float4 eroded; };
#endif

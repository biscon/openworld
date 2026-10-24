#ifndef OW_TERRAIN_GEOGRAPHY
#define OW_TERRAIN_GEOGRAPHY
// Matches the 496-byte TerrainParameters CPU ABI.
struct NoiseDomain {int4 lattice;float4 fractionFrequency;};
struct MaterialGeography {uint4 dimensions;float4 shape;NoiseDomain domains[13];uint4 sampling;uint4 worldOrigin;float4 geography;};
#endif

#ifndef OW_MATERIALS
#define OW_MATERIALS
struct NoiseDomain {int4 lattice;float4 fractionFrequency;};
struct MaterialGeography {uint4 dimensions;float4 shape;NoiseDomain domains[13];uint4 sampling;uint4 worldOrigin;float4 geography;};
struct MaterialCoordinates {float4 microPhase,macroPhase;};
struct MaterialUniform {
    MaterialGeography geography; float4 controls,tint; uint4 options;
    MaterialCoordinates coordinates; float4 blend; uint4 features;
};
struct MaterialRecord {uint4 maps;uint4 macro;float4 base,emission,factors;uint4 meshMaps;float4 alpha;};
[[vk::binding(0,2)]] ConstantBuffer<MaterialUniform> materialInfo;
[[vk::binding(1,2)]] StructuredBuffer<MaterialRecord> materialRecords;
#ifdef OW_BINDLESS
[[vk::binding(2,2)]] Texture2D<float4> materialImages[];
[[vk::binding(3,2)]] Texture2DArray<float4> materialArrays[];
#else
[[vk::binding(2,2)]] Texture2D<float4> materialImages[8];
[[vk::binding(3,2)]] Texture2DArray<float4> materialColour;
[[vk::binding(4,2)]] Texture2DArray<float4> materialNormal;
[[vk::binding(5,2)]] Texture2DArray<float4> materialScalar;
[[vk::binding(7,2)]] Texture2DArray<float4> materialMacro;
#endif
[[vk::binding(6,2)]] SamplerState materialSampler;
[[vk::binding(8,2)]] SamplerState macroSampler;
#define params materialInfo.geography
#define OW_MATERIAL_CONTINUOUS
#include "terrain_evaluate.hlsli"
#undef OW_MATERIAL_CONTINUOUS
float4 terrainWeights(float2 relative){
    if(materialInfo.options.z>0){float4 isolated=0;isolated[materialInfo.options.z-1]=1;return isolated;}
    float2 local=relative+materialInfo.controls.zw;
    // Hard-clamped octave feedback has discontinuous derivatives. Classify
    // the smooth material proxy so smoothstep below receives a continuous slope.
    BaseTerrainSample land=evaluateBaseTerrain(local,true);
    float slope=length(land.gradient);
    float rock=smoothstep(.47,1.0,slope);
    float patch=saturate(.5+gradientNoise(12,local).x);
    float soil=smoothstep(.48,.72,patch)*.65;
    float gravel=smoothstep(.2,.7,slope)*(.2+.4*(1-patch));
    float4 w=float4((1-soil)*(1-gravel),soil*(1-gravel),0,gravel)*(1-rock);w.z=rock;
    return w/max(dot(w,1.0.xxxx),1e-6);
}
#include "material_sampling.hlsli"
float3 weightColour(float4 w) {
    return w.x*float3(.1,.8,.1)+w.y*float3(.6,.25,.05)+w.z*float3(.5,.5,.8)+w.w*float3(.9,.75,.2);
}
float3 displayMappedMaterial(MaterialEvaluation e,float3 relative,float visibility){
    if(materialInfo.features.w==1)return weightColour(e.weights);
    if(materialInfo.features.w==2)return e.projections;
    if(materialInfo.features.w==3)return e.surface.normal*.5+.5;
    if(materialInfo.features.w==4)return float3(e.reads/24.0,1-e.reads/24.0,0);
    return shadeSurface(e.surface,relative,visibility);
}
float3 shadeTerrain(float3 normal,float3 relative,float visibility){
    if(materialInfo.options.x==0)return shadeSurface(terrainMaterial(normal),relative,visibility);
    float4 w=terrainWeights(relative.xz);
    if(materialInfo.options.y!=0)return weightColour(w);
    return displayMappedMaterial(evaluateMappedMaterial(normal,relative,w),relative,visibility);
}
#endif
#undef params

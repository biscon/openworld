#include "temporal_types.hlsli"
[[vk::binding(0,0)]] Texture2D<float4> source;
[[vk::binding(1,0)]] Texture2D<float4> sourceDepth;
[[vk::binding(2,0)]] Texture2D<float4> motion;
[[vk::binding(3,0)]] Texture2D<float4> reactivity;
[[vk::binding(4,0)]] Texture2D<float4> history;
[[vk::binding(5,0)]] Texture2D<float4> historyDepth;
[[vk::image_format("rgba16f")]] [[vk::binding(6,0)]] RWTexture2D<float4> result;
[[vk::binding(7,0)]] RWTexture2D<float> depthResult;
[[vk::binding(8,0)]] SamplerState linearClamp;
[[vk::binding(9,0)]] ConstantBuffer<TemporalView> temporal;
struct Push {uint4 dims,pass;float4 effect,extra;};
[[vk::push_constant]] ConstantBuffer<Push> settings;
float luminance(float3 c){return dot(c,float3(.2126,.7152,.0722));}
float3 finiteHdr(float3 c){return all(isfinite(c))?clamp(c,0,65504):0;}

[[vk::binding(0,0)]] Texture2D<float4> source;
[[vk::binding(1,0)]] Texture2D<float4> auxiliary;
[[vk::binding(2,0)]] Texture2D<float4> areaTex;
[[vk::binding(3,0)]] Texture2D<float4> searchTex;
[[vk::binding(4,0)]] SamplerState linearClamp;
[[vk::binding(5,0)]] SamplerState pointClamp;
struct Spatial { float4 metrics; uint mapper; float bloomIntensity; uint2 unused; };
[[vk::push_constant]] ConstantBuffer<Spatial> settings;

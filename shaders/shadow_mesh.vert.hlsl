#include "shadow_resources.hlsli"
struct InstanceRecord {column_major float4x4 transform;column_major float4x4 normalTransform;uint4 options;column_major float4x4 previousTransform;};
[[vk::binding(0,0)]] StructuredBuffer<InstanceRecord> instances;
[[vk::binding(2,3)]] StructuredBuffer<uint> shadowSources;
struct Input {[[vk::location(0)]]float3 position:POSITION;[[vk::location(1)]]float3 normal:NORMAL;[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD0;};
struct Output {float4 position:SV_Position;[[vk::location(3)]]float2 uv:TEXCOORD1;[[vk::location(4)]]nointerpolation uint material:TEXCOORD2;};
Output main(Input i,uint id:SV_InstanceID) {
    InstanceRecord instance=instances[shadowSources[shadowRecord(id)]];
    Output o;o.position=shadowPosition(mul(instance.transform,float4(i.position,1)).xyz);
    o.uv=i.uv;o.material=instance.options.x;return o;
}

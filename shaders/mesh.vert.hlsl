#include "temporal_surface.hlsli"
#include "lighting.hlsli"
struct InstanceRecord {column_major float4x4 transform;column_major float4x4 normalTransform;uint4 options;column_major float4x4 previousTransform;};
[[vk::binding(0,0)]] StructuredBuffer<InstanceRecord> instances;
struct Draw {uint instance;};
[[vk::push_constant]] ConstantBuffer<Draw> draw;
struct Input {[[vk::location(0)]]float3 position:POSITION;[[vk::location(1)]]float3 normal:NORMAL;[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD0;};
struct Output {[[vk::location(8)]]float4 previousClip:TEXCOORD7;float4 position:SV_Position;[[vk::location(0)]]float3 relative:TEXCOORD0;[[vk::location(1)]]float3 normal:NORMAL;[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD1;[[vk::location(4)]]nointerpolation uint material:TEXCOORD2;};
Output main(Input i,uint id:SV_InstanceID){InstanceRecord instance=instances[(draw.instance&0x80000000u)!=0?id:draw.instance];Output o;
    o.relative=mul(instance.transform,float4(i.position,1)).xyz;o.position=mul(lighting.viewProjection,float4(o.relative,1));
    o.normal=mul(instance.normalTransform,float4(i.normal,0)).xyz;
    o.tangent=float4(mul(instance.transform,float4(i.tangent.xyz,0)).xyz,i.tangent.w*(instance.options.y!=0?-1:1));
    o.previousClip=instance.options.z!=0?mul(temporal.previous,mul(instance.previousTransform,float4(i.position,1))):0;
    o.uv=i.uv;o.material=instance.options.x;return o;
}

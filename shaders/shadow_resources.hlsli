#include "shadow_types.hlsli"
[[vk::binding(1,3)]] ConstantBuffer<ShadowUniform> shadow;
struct ShadowPush {uint cascade;uint record;};
[[vk::push_constant]] ConstantBuffer<ShadowPush> shadowPush;
uint shadowRecord(uint instance) {return shadowPush.record==0xffffffff?instance:shadowPush.record;}
float4 shadowPosition(float3 position) {return mul(shadow.matrices[shadowPush.cascade],float4(position,1));}

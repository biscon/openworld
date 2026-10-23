#ifndef OW_TERRAIN_SURFACE
#define OW_TERRAIN_SURFACE
struct TerrainSurface {
    float4 position : SV_Position;
    [[vk::location(8)]] float4 previousClip : TEXCOORD7;
#ifdef TERRAIN_RECORD_ID
    [[vk::location(7)]] nointerpolation uint recordID : TEXCOORD6;
#endif
    [[vk::location(6)]] float3 relativePosition : TEXCOORD5;
    [[vk::location(0)]] float3 normal : NORMAL;
    [[vk::location(1)]] float2 gridPosition : TEXCOORD0;
    [[vk::location(2)]] nointerpolation uint4 options : TEXCOORD1;
    [[vk::location(3)]] nointerpolation float4 normalInfo : TEXCOORD2;
    [[vk::location(4)]] float morph : TEXCOORD3;
    [[vk::location(5)]] float3 barycentric : TEXCOORD4;
};
#endif

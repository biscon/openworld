struct ShadowTerrainSurface {
    float4 position:SV_Position;
    [[vk::location(0)]] float2 gridPosition:TEXCOORD0;
    [[vk::location(1)]] nointerpolation uint recordID:TEXCOORD1;
};

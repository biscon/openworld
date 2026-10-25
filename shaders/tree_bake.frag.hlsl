struct Push {
    float4 right, up, direction, centre;
    uint part;
};
[[vk::push_constant]] ConstantBuffer<Push> bake;
[[vk::binding(0, 0)]] Texture2D<float4> bark;
[[vk::binding(1, 0)]] Texture2D<float4> leaf;
[[vk::binding(2, 0)]] SamplerState linearSampler;
struct In {
    float4 p : SV_Position;
    [[vk::location(0)]] float3 n : NORMAL;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float depth : TEXCOORD1;
};
struct Out {
    float4 colour : SV_Target0;
    float4 normal : SV_Target1;
    float depth : SV_Target2;
};
float2 oct(float3 n) {
    n /= dot(abs(n), 1.0.xxx);
    float2 p = n.xz;
    if (n.y < 0)
        p = (1 - abs(p.yx)) * float2(p.x >= 0 ? 1 : -1, p.y >= 0 ? 1 : -1);
    return p * .5 + .5;
}
Out main(In i, bool front : SV_IsFrontFace) {
    Out o;
    float4 c = bake.part != 0 ? leaf.Sample(linearSampler, i.uv) : bark.Sample(linearSampler, i.uv);
    if (bake.part != 0)
        clip(c.a - .4);
    float3 n = normalize(i.n);
    if (bake.part != 0 && dot(n, bake.direction.xyz) < 0)
        n = -n;
    o.colour = float4(c.rgb, 1);
    o.normal = float4(oct(n), bake.part != 0 ? .8 : 1, bake.part);
    o.depth = i.depth;
    return o;
}

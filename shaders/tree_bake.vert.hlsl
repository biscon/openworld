struct Push {
    float4 right, up, direction, centre;
    uint part;
};
[[vk::push_constant]] ConstantBuffer<Push> bake;
struct In {
    [[vk::location(0)]] float3 p : POSITION;
    [[vk::location(1)]] float3 n : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct Out {
    float4 p : SV_Position;
    [[vk::location(0)]] float3 n : NORMAL;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float depth : TEXCOORD1;
};
Out main(In i) {
    Out o;
    float3 p = i.p - bake.centre.xyz;
    o.depth = .5 + dot(p, bake.direction.xyz) / (2 * bake.centre.w);
    o.p = float4(dot(p, bake.right.xyz) / bake.centre.w, -dot(p, bake.up.xyz) / bake.centre.w, o.depth, 1);
    o.n = i.n;
    o.uv = i.uv;
    return o;
}

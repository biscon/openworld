#include "terrain_common.hlsli"
[[vk::binding(1, 0)]] StructuredBuffer<float2> heights;
[[vk::binding(2, 0)]] RWStructuredBuffer<Sample> samples;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indices;
[[vk::binding(4, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> normalMap;
struct Tile { uint4 unused; uint4 offset; };
[[vk::push_constant]] ConstantBuffer<Tile> tile;
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    id.xy += tile.offset.xy;
    uint factor = params.dimensions.w;
    uint width = params.dimensions.x*factor+1+2*params.sampling.x, halo = width+2;
    float spacing = params.shape.x/factor;
    if (id.x>=width || id.y>=width) return;
    uint index = (id.y+1)*halo+id.x+1;
    float2 dx = (heights[index+1]-heights[index-1])/(2*spacing);
    float2 dz = (heights[index+halo]-heights[index-halo])/(2*spacing);
    Sample s;
    s.base = float4(normalize(float3(-dx.x,1,-dz.x)), heights[index].x);
    s.eroded = float4(normalize(float3(-dx.y,1,-dz.y)), heights[index].y);
    normalMap[uint3(id.xy, 0)] = float4(s.base.xyz, 0);
    normalMap[uint3(id.xy, 1)] = float4(s.eroded.xyz, 0);
    // Geometry uses a subset of the densely evaluated height field.
    if (any(id.xy < params.sampling.x)) return;
    uint2 local = id.xy-params.sampling.x;
    if (any(local > params.dimensions.x*factor) || any(local % factor != 0)) return;
    uint2 vertex = local/factor;
    uint meshWidth = params.dimensions.x+1;
    samples[vertex.y*meshWidth+vertex.x] = s;
    if (params.dimensions.z != 0 && all(vertex < params.dimensions.x)) {
        uint a = vertex.y*meshWidth+vertex.x;
        uint i = (vertex.y*(meshWidth-1)+vertex.x)*6;
        indices[i] = a;
        indices[i+1] = a+meshWidth;
        indices[i+2] = a+1;
        indices[i+3] = a+1;
        indices[i+4] = a+meshWidth;
        indices[i+5] = a+meshWidth+1;
    }
}

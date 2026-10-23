#include "lighting.hlsli"
#include "materials.hlsli"
#include "terrain_surface.hlsli"
[[vk::binding(5, 0)]] Texture2DArray<float4> normalMap;
[[vk::binding(7, 0)]] Texture2DArray<float4> nextNormals;
[[vk::binding(6, 0)]] SamplerState normalSampler;
float4 main(TerrainSurface input) : SV_Target0 {
    float3 normal = input.normal;
    if (input.options.y != 0) {
        uint width, height, layers;
        normalMap.GetDimensions(width, height, layers);
        float2 grid=clamp(input.gridPosition,0.0,float(input.options.z));
        float2 uv=(grid*input.normalInfo.x+input.normalInfo.y+0.5)/float2(width,height);
        normal=normalMap.SampleLevel(normalSampler,float3(uv,float(input.options.x)),0).xyz;
        if(input.morph>0) {
            nextNormals.GetDimensions(width,height,layers);
            uv=(grid*input.normalInfo.x*0.5+input.normalInfo.z+0.5)/float2(width,height);
            normal=lerp(normal,nextNormals.SampleLevel(normalSampler,float3(uv,float(input.options.x)),0).xyz,input.morph);
        }
    }
    float light = 0.22 + 0.78*max(0.0,dot(normalize(normal),normalize(float3(-0.6,0.8,0.35))));
    float3 colour = (input.options.w & 1) != 0 ? normalize(normal)*0.5+0.5 : float3(0.40,0.46,0.38)*light;
    if ((input.options.w & 8) != 0) {
        const float3 colours[8]={float3(0.2,0.8,0.25),float3(0.2,0.5,0.9),float3(0.95,0.65,0.15),
            float3(0.85,0.2,0.25),float3(0.6,0.25,0.9),float3(0.1,0.8,0.8),float3(0.9,0.4,0.6),float3(0.7,0.7,0.7)};
        colour=colours[uint(input.normalInfo.w)]*light;
    }
    if ((input.options.w & 2) != 0) {
        float3 d=input.barycentric/max(fwidth(input.barycentric),1e-6);
        float edgeDistance=min(d.x,min(d.y,d.z));
        colour=lerp(float3(0.015,0.015,0.015),colour,smoothstep(0.5,1.5,edgeDistance));
    }
    if ((input.options.w & 4) != 0) {
        float2 distance = min(input.gridPosition, float(input.options.z) - input.gridPosition)
                          / max(fwidth(input.gridPosition), 1e-6);
        colour = lerp(float3(0.9,0.35,0.04), colour, smoothstep(0.5,1.5,min(distance.x,distance.y)));
    }
    if(lighting.output.y==0) colour=shadeTerrain(normal,input.relativePosition,screenVisibility(input.position.xy));
    return float4(colour,1);
}

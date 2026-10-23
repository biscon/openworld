#include "temporal_surface.hlsli"
#include "lighting.hlsli"
#include "materials.hlsli"
#include "terrain_stream_common.hlsli"
#include "terrain_surface.hlsli"
#include "terrain_clip.hlsli"
float4 shadePixel(TerrainSurface input) : SV_Target0 {
    loadDraw(input.recordID);
    clipTerrainView(input);
    if(draw.tile.w>=0) return float4(lerp(float3(.05,.6,.1),float3(1,.05,.02),draw.tile.w),1);
    float3 normal = input.options.y != 0 ? sampledNormal(clamp(input.gridPosition,0.0,float(input.options.z)),input.morph) : input.normal;
    float light = 0.22 + 0.78*max(0.0,dot(normalize(normal),normalize(float3(-0.6,0.8,0.35))));
    float3 colour = (input.options.w & 1) != 0 ? normalize(normal)*0.5+0.5 : float3(0.40,0.46,0.38)*light;
    if ((input.options.w & 8) != 0) {
        const float3 colours[8]={float3(0.2,0.8,0.25),float3(0.2,0.5,0.9),float3(0.95,0.65,0.15),
            float3(0.85,0.2,0.25),float3(0.6,0.25,0.9),float3(0.1,0.8,0.8),float3(0.9,0.4,0.6),float3(0.7,0.7,0.7)};
        colour=colours[uint(input.normalInfo.w)%8]*light;
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

#ifdef OW_TEMPORAL_OUTPUT
TemporalOutput main(TerrainSurface input) {
    return temporalOutput(shadePixel(input),input.relativePosition,input.previousClip,float(input.previousClip.w>0),saturate(dot(lighting.emissive.rgb,float3(.2126,.7152,.0722))/10000));
}

#else
float4 main(TerrainSurface input):SV_Target0 { return shadePixel(input); }
#endif

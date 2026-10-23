// Owned numerical fixture: outputs raw material results into the HDR target.
#include "lighting.hlsli"
#include "materials.hlsli"
#include "terrain_surface.hlsli"
float4 main(TerrainSurface input):SV_Target0 {
    if(input.morph<0)return input.morph.xxxx; // Keep the production vertex interface live.
    if(materialInfo.features.w==2) {
        float angle=input.position.x/128*3.14159265;
        float3 n=normalize(float3(sin(angle),cos(angle),.2*sin(angle)));
        return float4(projectionWeights(n),1);
    }
    uint column=uint(input.position.x)/16,row=uint(input.position.y)/16;
    if(row==0) {
        if(column==0)return heightWeights(0,0,1,.25);
        if(column==1)return heightWeights(float4(0,0,1,0),float4(1,1,0,1),1,.25);
        if(column==2)return heightWeights(float4(.5,.5,0,0),float4(0,1,1,1),1,.25);
        if(column==3)return heightWeights(float4(.5,.5,0,0),float4(1,1,0,0),1,.25);
        if(column==4)return heightWeights(float4(.25,.75,0,0),0,0,.25);
        return heightWeights(float4(.25,.25,.25,.25),float4(0,.3,.7,1),1,.01);
    }
    if(row==1) {
        float3 normals[8]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),
                           float3(0,0,1),float3(0,0,-1),normalize(float3(1,1,1)),normalize(float3(-1,1,-1))};
        MaterialEvaluation e=evaluateMappedMaterial(normals[column],input.relativePosition,float4(0,0,1,0));
        return float4(e.surface.normal*.5+.5,1);
    }
    if(row==2) {
        if(column>=6) {
            float3 anchored=float3(input.position.x*.01-8,3-lighting.camera.y,input.position.y*.01-8);
            return float4(evaluateMappedMaterial(float3(0,0,1),anchored,float4(0,0,1,0)).surface.baseColour,1);
        }
        return 0;
    }
    if(row==3) {
        float x=input.position.x/128;
        return heightWeights(float4(1-x,x,0,0),float4(.2,.8,0,0),1,.25);
    }
    float3 n=normalize(float3(.8,.3,-.5));
    MaterialEvaluation e=evaluateMappedMaterial(n,input.relativePosition,.25.xxxx);
    if(row==4)return float4(e.surface.baseColour,e.surface.roughness);
    return float4(e.weights.xyz,e.reads/24);
}

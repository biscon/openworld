#ifndef OW_LIGHTING
#define OW_LIGHTING
#include "atmosphere_common.hlsli"
struct LightingUniform {
    column_major float4x4 viewProjection;
    float4 camera, sunDirection, sunColour, environment;
    float4 baseRoughness, material, emissive, output;
    float4 irradiance[9];
};
[[vk::binding(0,1)]] ConstantBuffer<LightingUniform> lighting;
[[vk::binding(1,1)]] TextureCube<float4> environmentMap;
[[vk::binding(2,1)]] Texture2D<float4> brdfMap;
[[vk::binding(3,1)]] SamplerState lightingSampler;
[[vk::binding(9,1)]] Texture2D<float4> gtaoMap;
float screenVisibility(float2 pixel) { return lighting.material.z>0 ? gtaoMap.Load(int3(uint2(pixel),0)).r : 1; }
struct SurfaceMaterial { float3 baseColour, normal; float roughness, metallic, ao; float3 emissive; float opacity; };
float3 safeNormal(float3 n) { return dot(n,n)>1e-12?normalize(n):float3(0,1,0); }
float3 environmentDirection(float3 d) {
    float c=lighting.environment.z,s=lighting.environment.w;
    return float3(c*d.x-s*d.z,d.y,s*d.x+c*d.z);
}
float3 diffuseIrradiance(float3 n) {
    float b[9]={.2820947918,.4886025119*n.y,.4886025119*n.z,.4886025119*n.x,
        1.0925484306*n.x*n.y,1.0925484306*n.y*n.z,.3153915653*(3*n.z*n.z-1),
        1.0925484306*n.x*n.z,.5462742153*(n.x*n.x-n.y*n.y)};
    float3 result=0;
    [unroll]for(uint i=0;i<9;i++)result+=lighting.irradiance[i].rgb*b[i];
    return max(result,0);
}
#include "clusters.hlsli"
float3 directSurface(SurfaceMaterial m,float3 n,float3 v,float3 l) {
    const float pi=3.14159265359;
    float nv=max(dot(n,v),1e-5),nl=saturate(dot(n,l));
    float r=clamp(m.roughness,.045,1),a=r*r,a2=a*a;
    float3 f0=lerp(.04.xxx,m.baseColour,m.metallic);
    if(nl<=0 || dot(v+l,v+l)<=1e-10)return 0;
    float3 h=normalize(v+l);float nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float denom=nh*nh*(a2-1)+1;
    float d=a2/(pi*denom*denom);
    float vis=.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-7);
    float3 f=f0+(1-f0)*pow(1-vh,5);
    return ((1-f)*(1-m.metallic)*m.baseColour/pi+d*vis*f)*nl;
}
float3 localSurface(SurfaceMaterial m,float3 n,float3 v,float3 position,uint index) {
    LocalLightRecord light=localLights[index];float3 delta=light.positionRange.xyz-position;
    float d2=dot(delta,delta),range2=light.positionRange.w*light.positionRange.w;
    if(d2>=range2)return 0;
    float3 l=safeNormal(delta);float a=saturate(1-(d2/range2)*(d2/range2));
    float cone=1;
    if(light.parameters.z!=0 && d2>1e-12)cone=saturate((dot(-l,light.directionOuter.xyz)-light.directionOuter.w)/(light.parameters.y-light.directionOuter.w));
    float attenuation=a*a*cone*cone/max(d2,light.parameters.x*light.parameters.x);
    return directSurface(m,n,v,l)*light.colourIntensity.rgb*(light.colourIntensity.w*attenuation);
}
uint surfaceCluster(float3 position,out bool valid) {
    float3 view=mul(clusters.view,float4(position,1)).xyz;
    valid=view.z>=clusters.depth.x && view.z<=clusters.depth.y;
    float2 ndc=float2(view.x/clusters.projection.z,-view.y/clusters.projection.w)/max(view.z,1e-6);
    float2 pixel=(ndc*.5+.5)*clusters.projection.xy;
    valid=valid && all(pixel>=0) && all(pixel<clusters.projection.xy);
    uint2 xy=uint2(clamp(floor(pixel/clusters.grid.w),0,float2(clusters.grid.xy-1)));
    return (sliceForDepth(view.z)*clusters.grid.y+xy.y)*clusters.grid.x+xy.x;
}
#include "shadow_sampling.hlsli"
float3 shadeSurface(SurfaceMaterial m,float3 relativePosition,float visibility) {
    const float pi=3.14159265359;
    float3 n=safeNormal(m.normal),v=safeNormal(-relativePosition);
    float nv=max(dot(n,v),1e-5),r=clamp(m.roughness,.045,1);
    float3 f0=lerp(.04.xxx,m.baseColour,m.metallic);
    float shadowVisibility=sunVisibility(relativePosition,n);
    if(sunShadow.sampling.w==1)return shadowVisibility.xxx;
    if(sunShadow.sampling.w==2)return float3(shadowVisibility,1-shadowVisibility,.5);
    if(sunShadow.sampling.w==3)return float3(1-shadowVisibility,shadowVisibility,0);
    float3 colour=directSurface(m,n,v,lighting.sunDirection.xyz)*lighting.sunColour.rgb*atmSurfaceSun(relativePosition)*shadowVisibility;
    if(clusters.options.z!=0 && (clusters.options.x>0 || clusters.options.w!=0)) {
        bool valid;uint id=surfaceCluster(relativePosition,valid);
        uint count=clusters.options.x;
        bool brute=clusters.options.z==1 || !valid;
        if(!brute){count=clusterCounts[id];brute=count>clusters.options.y;}
        if(clusters.options.w!=0 && clusters.options.z==2 && valid) {
            if(clusters.options.w==2)return count>clusters.options.y?float3(1,0,1):0;
            float t=saturate(float(count)/clusters.options.y);return float3(t,t*t,1-t);
        }
        if(brute){[loop]for(uint i=0;i<clusters.options.x;++i)colour+=localSurface(m,n,v,relativePosition,i);}
        else {[loop]for(uint i=0;i<count;++i)colour+=localSurface(m,n,v,relativePosition,clusterIndices[id*clusters.options.y+i]);}
    }
    float2 brdf=brdfMap.SampleLevel(lightingSampler,float2(nv,r),0).rg;
    float3 spec=atmosphere.options.x==2?
        atmosphereEnvironment.SampleLevel(atmosphereSampler,reflect(-v,n),r*6).rgb*atmosphere.illumination.rgb*atmosphere.options.w:
        environmentMap.SampleLevel(lightingSampler,environmentDirection(reflect(-v,n)),r*6).rgb;
    float3 f=f0+(max(1-r,f0)-f0)*pow(1-nv,5);
    colour+=(atmosphere.options.x==2?1:lighting.environment.x)*((1-f)*(1-m.metallic)*m.baseColour/pi*min(m.ao,visibility)*
        (atmosphere.options.x==2?atmDiffuse(n):diffuseIrradiance(environmentDirection(n)))+spec*(f0*brdf.x+brdf.y));
    return min(max((colour+m.emissive)*lighting.output.x,0),65504);
}
SurfaceMaterial terrainMaterial(float3 normal) {
    SurfaceMaterial m;
    m.baseColour=lighting.baseRoughness.rgb;m.roughness=lighting.baseRoughness.a;
    m.normal=normal;m.metallic=lighting.material.x;m.ao=lighting.material.y;
    m.emissive=lighting.emissive.rgb;m.opacity=1;
    return m;
}
#endif

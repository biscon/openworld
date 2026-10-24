// Atmosphere algorithms adapted from Hillaire (EGSR 2020).
// Copyright (c) 2020 Epic Games, Inc. MIT attribution: third_party/sky_atmosphere/LICENSE.
#ifndef OW_ATMOSPHERE
#define OW_ATMOSPHERE
#ifndef ATM_SET
#define ATM_SET 1
#endif
struct AtmosphereUniform {
    column_major float4x4 inverseRaster,current,previous;
    float4 planet,rayleigh,mie,ozone,profile,sun,illumination,view,options,extent,haze;
};
[[vk::binding(16,ATM_SET)]] ConstantBuffer<AtmosphereUniform> atmosphere;
[[vk::binding(17,ATM_SET)]] Texture2D<float4> atmosphereTransmittance;
[[vk::binding(18,ATM_SET)]] Texture2D<float4> atmosphereMultiple;
[[vk::binding(19,ATM_SET)]] Texture2D<float4> atmosphereSky;
[[vk::binding(20,ATM_SET)]] Texture3D<float4> atmosphereAerialT;
[[vk::binding(21,ATM_SET)]] Texture3D<float4> atmosphereAerialS;
#ifdef ATM_COMPUTE
[[vk::binding(22,ATM_SET)]] RWStructuredBuffer<float4> atmosphereIrradiance;
#else
[[vk::binding(22,ATM_SET)]] StructuredBuffer<float4> atmosphereIrradiance;
#endif
[[vk::binding(23,ATM_SET)]] SamplerState atmosphereSampler;
[[vk::binding(24,ATM_SET)]] TextureCube<float4> atmosphereEnvironment;
static const float atmPi=3.14159265359;
// Atmosphere integration uses kilometres. World-space renderer inputs remain metres.
float3 atmOrigin(){return float3(0,atmosphere.planet.x+max(.001,atmosphere.view.x),0);}
float atmHeight(float3 p){return max(0,length(p)-atmosphere.planet.x);}
float atmEnd(float3 p,float3 d,out bool ground) {
    float r=atmosphere.planet.x,rt=r+atmosphere.planet.y,b=dot(p,d);
    // Factored radial terms avoid subtraction of two squared planet radii.
    float h=length(p),top=-b+sqrt(max(0,b*b+(rt-h)*(rt+h)));
    float disc=b*b+(r-h)*(r+h);ground=false;
    if(disc>=0){float near=-b-sqrt(disc);if(near>=0){ground=true;return max(0,min(top,near));}}
    return max(0,top);
}
void atmMedium(float3 p,out float3 extinction,out float3 rayleigh,out float mie) {
    float h=atmHeight(p);rayleigh=atmosphere.rayleigh.rgb*exp(-h/atmosphere.rayleigh.w);
    float md=exp(-h/atmosphere.mie.z);mie=atmosphere.mie.x*md;
    extinction=rayleigh+(atmosphere.mie.x+atmosphere.mie.y)*md+
        atmosphere.ozone.rgb*saturate(1-abs(h-atmosphere.profile.x)/atmosphere.profile.y);
}
float2 atmTransUv(float radius,float mu) {
    float r=atmosphere.planet.x,rt=r+atmosphere.planet.y;
    float H=sqrt((rt-r)*(rt+r)),rho=sqrt(max(0,(radius-r)*(radius+r)));
    float d=max(0,-radius*mu+sqrt(max(0,radius*radius*mu*mu+(rt-radius)*(rt+radius))));
    float2 uv=float2((d-(rt-radius))/max(rho+H-(rt-radius),1e-6),rho/H);
    return (saturate(uv)*float2(255,63)+.5)/float2(256,64);
}
void atmTransRay(float2 uv,out float3 p,out float3 d) {
    float r=atmosphere.planet.x,rt=r+atmosphere.planet.y,H=sqrt((rt-r)*(rt+r));
    float rho=uv.y*H,radius=sqrt(r*r+rho*rho),distance=lerp(rt-radius,rho+H,uv.x);
    float mu=distance>1e-6?clamp(((rt-radius)*(rt+radius)-distance*distance)/(2*radius*distance),-1,1):1;
    p=float3(0,radius,0);d=float3(sqrt(saturate(1-mu*mu)),mu,0);
}
float3 atmSunT(float3 p,float3 sun) {
    float radius=length(p),mu=dot(p,sun)/radius,r=atmosphere.planet.x;
    if(mu<0 && radius*radius*mu*mu+(r-radius)*(r+radius)>0)return 0;
    return atmosphereTransmittance.SampleLevel(atmosphereSampler,atmTransUv(radius,mu),0).rgb;
}
float2 atmMultiUv(float3 p,float3 sun) {
    return (saturate(float2(dot(normalize(p),sun)*.5+.5,atmHeight(p)/atmosphere.planet.y))*31+.5)/32;
}
float atmHorizon(){return -acos(saturate(atmosphere.planet.x/(atmosphere.planet.x+max(.001,atmosphere.view.x))));}
float2 atmSkyUv(float3 d) {
    float angle=asin(clamp(d.y,-1,1)),h=atmHorizon(),v;
    v=angle>=h?.5+.5*sqrt(saturate((angle-h)/(atmPi*.5-h))):.5-.5*sqrt(saturate((h-angle)/(h+atmPi*.5)));
    float2 uv=float2(frac(atan2(d.z,d.x)/(2*atmPi)+1),v);
    // Latitude endpoints are texel-centred; longitude is periodic.
    return float2(uv.x,(uv.y*(atmosphere.extent.z-1)+.5)/atmosphere.extent.z);
}
float3 atmSkyDirection(float2 uv) {
    float v=uv.y*2-1,h=atmHorizon(),e=h+sign(v)*v*v*(v>=0?atmPi*.5-h:h+atmPi*.5);
    float a=uv.x*2*atmPi;return float3(cos(e)*cos(a),sin(e),cos(e)*sin(a));
}
float3 atmSampleSky(float3 d) {
    // Explicit periodic longitude handles the clamp sampler's seam.
    float2 uv=atmSkyUv(d);float x=uv.x*atmosphere.extent.y-.5,ix=floor(x),f=frac(x);
    float w=atmosphere.extent.y;
    float2 a=float2((fmod(ix+w,w)+.5)/w,uv.y),b=float2((fmod(ix+1+w,w)+.5)/w,uv.y);
    return lerp(atmosphereSky.SampleLevel(atmosphereSampler,a,0).rgb,atmosphereSky.SampleLevel(atmosphereSampler,b,0).rgb,f);
}
float3 atmAnalytic(float3 d) {
    float daylight=saturate(atmosphere.sun.y*4+.15);
    return lerp(float3(.015,.018,.02),lerp(float3(.12,.22,.4),float3(.035,.11,.3),sqrt(saturate(d.y))),daylight)*.08;
}
float3 atmSkyRadiance(float3 d) {
    float3 c=atmosphere.options.x==2?atmSampleSky(d):atmAnalytic(d);
    float mu=dot(d,atmosphere.sun.xyz),r=atmosphere.sun.w;
    float disk=smoothstep(cos(r*1.08),cos(r*.92),mu);
    float3 trans=atmosphere.options.x==2?atmSunT(atmOrigin(),d):1;
    if(d.y>sin(atmHorizon()))c+=trans*disk/(2*atmPi*(1-cos(r)));
    return min(max(c*atmosphere.illumination.rgb*atmosphere.illumination.w,0),65504);
}
float3 atmSurfaceSun(float3 relativePosition) {
    if(atmosphere.options.x!=2)return 1;
    float3 p=atmOrigin()+relativePosition*.001;
    if(atmosphere.view.w==0)p=float3(0,atmosphere.planet.x+max(.001,atmosphere.view.x+relativePosition.y*.001),0);
    return atmSunT(p,atmosphere.sun.xyz);
}
float3 atmDiffuse(float3 n) {
    float b[9]={.2820947918,.4886025119*n.y,.4886025119*n.z,.4886025119*n.x,
        1.0925484306*n.x*n.y,1.0925484306*n.y*n.z,.3153915653*(3*n.z*n.z-1),
        1.0925484306*n.x*n.z,.5462742153*(n.x*n.x-n.y*n.y)};
    float3 c=0;[unroll]for(uint i=0;i<9;++i)c+=atmosphereIrradiance[i].rgb*b[i];
    return max(c,0)*atmosphere.illumination.rgb*atmosphere.options.w;
}
float atmSliceDistance(float index) {
    float nearCount=atmosphere.view.w,n=atmosphere.extent.w;
    if(nearCount==0)return pow(index/(n-1),2)*atmosphere.view.y;
    if(index<=nearCount-1)return pow(index/(nearCount-1),2)*64000;
    return 64000*pow(max(1,atmosphere.view.y/64000),(index-nearCount+1)/(n-nearCount));
}
float atmDistanceSlice(float depth) {
    float nearCount=atmosphere.view.w,n=atmosphere.extent.w;
    if(nearCount==0)return sqrt(saturate(depth/atmosphere.view.y))*(n-1);
    if(depth<=64000)return sqrt(saturate(depth/64000))*(nearCount-1);
    return nearCount-1+(n-nearCount)*saturate(log(depth/64000)/max(1e-6,log(atmosphere.view.y/64000)));
}
float3 atmApply(float3 colour,float3 position) {
    if(atmosphere.options.x!=2 || atmosphere.options.y==0)return colour;
    float4 clip=mul(atmosphere.current,float4(position,1));
    float2 uv=clip.xy/clip.w*.5+.5;
    // The volume is generated from jittered rays, so use the matching raster UV.
    uv+=atmosphere.haze.zw;
    float z=atmDistanceSlice(clip.w);
    float n=atmosphere.extent.w;
    float3 coord=float3(uv,(z+.5)/n);
    float3 t=atmosphereAerialT.SampleLevel(atmosphereSampler,coord,0).rgb;
    float3 s=atmosphereAerialS.SampleLevel(atmosphereSampler,coord,0).rgb*atmosphere.illumination.rgb*atmosphere.illumination.w;
    if(atmosphere.options.z==1)return t;
    if(atmosphere.options.z==2)return s;
    return min(max(t*colour+s,0),65504);
}
#endif

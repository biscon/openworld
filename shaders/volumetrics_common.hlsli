#ifndef OW_VOLUMETRICS
#define OW_VOLUMETRICS
#include "atmosphere_common.hlsli"
#include "curvature.hlsli"
struct VolumetricUniform {
    column_major float4x4 inverseViewProjection,current,previous;
    float4 curvature,previousCurvature,originDelta,grid,depth,height,albedo,temporal,counts,background,sun,illumination,jitter;
};
[[vk::binding(25,1)]] ConstantBuffer<VolumetricUniform> fog;
[[vk::binding(26,1)]] Texture3D<float4> fogT;
[[vk::binding(27,1)]] Texture3D<float4> fogS;
[[vk::binding(28,1)]] Texture3D<float4> fogExt;
[[vk::binding(29,1)]] Texture3D<float4> fogSource;
[[vk::binding(30,1)]] Texture2D<float4> fogSky;
[[vk::binding(31,1)]] Texture2D<float4> fogSkyT;
// Share the 16 m distribution offset with volumetricDepthOffset on the CPU.
float fogDepth(float index){return 16*(exp(fog.depth.y*index/fog.grid.z)-1);}
float fogSlice(float distance){return log(1+max(0,distance)/16)/fog.depth.y*fog.grid.z;}
float fogHistoryDepth(float distance){
    // Coefficients live at arithmetic interval midpoints, not log midpoints.
    // Exact self-reprojection prevents repeated filtering towards the next slice.
    float step=fog.depth.y/fog.grid.z;
    float centre=log((1+exp(step))*.5)/step;
    return (fogSlice(distance)-centre+.5)/fog.grid.z;
}
float3 fogUncurve(float3 p,float4 c){
    if(c.z==0)return p;
    float r=length(p.xz),base=c.x+c.y,y=base+p.y,radius=length(float2(r,y));
    float a=atan2(r,y),scale=r>1e-6?c.x*a/r:c.x/radius;
    return float3(p.x*scale,(r*r+p.y*(2*base+p.y))/(radius+base),p.z*scale);
}
float3 fogRay(float2 uv){float4 p=mul(fog.inverseViewProjection,float4(uv*2-1,0,1));return normalize(p.xyz/p.w);}
float fogDirectionDepth(float3 ray){return max(.001,mul(fog.current,float4(ray,0)).w);}
float3 fogIntegral(float3 ext,float distance){
    // Avoid cancellation for thin cells; analytic limit has a second-order series.
    float3 tau=ext*distance;
    return float3(tau.x<.001?distance*(1-tau.x*.5+tau.x*tau.x/6):(1-exp(-tau.x))/ext.x,
                  tau.y<.001?distance*(1-tau.y*.5+tau.y*tau.y/6):(1-exp(-tau.y))/ext.y,
                  tau.z<.001?distance*(1-tau.z*.5+tau.z*tau.z/6):(1-exp(-tau.z))/ext.z);
}
struct FogTransport {float3 t,s;float reactive,coverage;};
FogTransport fogNearRay(uint2 xy,float distance){
    float d=clamp(distance,0,fog.depth.x),z=fogSlice(d);uint cell=min(uint(floor(z)),uint(fog.grid.z)-1);
    float4 t=fogT.Load(int4(xy,cell,0)),s=fogS.Load(int4(xy,cell,0));
    float4 ext=fogExt.Load(int4(xy,cell,0)),source=fogSource.Load(int4(xy,cell,0));
    float dt=max(0,d-fogDepth(cell))/fogDirectionDepth(fogRay((xy+.5)/fog.grid.xy));
    FogTransport o;o.s=s.rgb+t.rgb*source.rgb*fogIntegral(ext.rgb,dt);o.t=t.rgb*exp(-ext.rgb*dt);
    o.reactive=max(t.a,source.a);o.coverage=ext.a;return o;
}
FogTransport fogNear(float2 uv,float depth){
    float2 p=clamp(uv*fog.grid.xy-.5,0,fog.grid.xy-1),f=frac(p);uint2 a=uint2(p),b=min(a+1,uint2(fog.grid.xy)-1);
    FogTransport q[4];q[0]=fogNearRay(a,depth);q[1]=fogNearRay(uint2(b.x,a.y),depth);
    q[2]=fogNearRay(uint2(a.x,b.y),depth);q[3]=fogNearRay(b,depth);
    float4 w=float4((1-f.x)*(1-f.y),f.x*(1-f.y),(1-f.x)*f.y,f.x*f.y);
    FogTransport o;o.t=0;o.s=0;o.reactive=0;o.coverage=0;
    [unroll]for(uint k=0;k<4;++k){o.t+=q[k].t*w[k];o.s+=q[k].s*w[k];o.reactive=max(o.reactive,q[k].reactive);o.coverage+=q[k].coverage*w[k];}
    return o;
}
float2 fogUv(float3 position,out float depth){float4 p=mul(fog.current,float4(position,1));depth=p.w;return p.xy/p.w*.5+.5;}
float fogReactivity(float3 position){
    if(fog.grid.w==0)return 0;
    float d;float2 uv=fogUv(position,d);
    uint z=min(uint(ceil(fogSlice(d))),uint(fog.grid.z));
    float2 p=clamp(uv*fog.grid.xy-.5,0,fog.grid.xy-1);
    uint2 a=uint2(p),b=min(a+1,uint2(fog.grid.xy)-1);
    return max(max(fogT.Load(int4(a,z,0)).a,fogT.Load(int4(b.x,a.y,z,0)).a),
               max(fogT.Load(int4(a.x,b.y,z,0)).a,fogT.Load(int4(b,z,0)).a));
}
float3 mediaApply(float3 colour,float3 position){
    if(fog.grid.w==0)return atmApply(colour,position);
    float d;float2 uv=fogUv(position,d);FogTransport near=fogNear(uv,d);
    float3 tail=colour;
    if(d>fog.depth.x && atmosphere.options.x==2 && atmosphere.options.y!=0){
        float3 at=float3(uv+fog.jitter.xy,(atmDistanceSlice(d)+.5)/atmosphere.extent.w);
        float3 t=atmosphereAerialT.SampleLevel(atmosphereSampler,at,0).rgb;
        float3 s=atmosphereAerialS.SampleLevel(atmosphereSampler,at,0).rgb*atmosphere.illumination.rgb*atmosphere.illumination.w;
        tail=t*colour+s;
    }
    if(fog.depth.w==1)return near.t;if(fog.depth.w==2)return near.s;
    if(fog.depth.w==3)return near.coverage.xxx;if(fog.depth.w==4)return near.reactive.xxx;
    return min(max(near.t*tail+near.s,0),65504);
}
float3 mediaSky(float3 d){
    if(fog.grid.w==0)return atmSkyRadiance(d);
    float4 clip=mul(fog.current,float4(d,0));float2 uv=clip.xy/clip.w*.5+.5;
    float3 c=fogSky.SampleLevel(atmosphereSampler,uv,0).rgb;
    if(atmosphere.options.x!=0){
        float r=atmosphere.sun.w,mu=dot(d,atmosphere.sun.xyz);
        float disk=smoothstep(cos(r*1.08),cos(r*.92),mu);
        if(d.y>sin(atmHorizon()))c+=fogSkyT.SampleLevel(atmosphereSampler,uv,0).rgb*disk/(2*atmPi*(1-cos(r)))*atmosphere.illumination.rgb*atmosphere.illumination.w;
    }
    if(fog.depth.w!=0){FogTransport n=fogNear(uv,fog.depth.x);
        if(fog.depth.w==1)return n.t;if(fog.depth.w==2)return n.s;if(fog.depth.w==3)return n.coverage.xxx;return n.reactive.xxx;}
    return min(max(c,0),65504);
}
#endif

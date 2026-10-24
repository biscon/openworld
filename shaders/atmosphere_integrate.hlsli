// Atmosphere algorithms adapted from Hillaire (EGSR 2020).
// Copyright (c) 2020 Epic Games, Inc. MIT attribution: third_party/sky_atmosphere/LICENSE.
#include "atmosphere_common.hlsli"
struct AtmTransport {float3 t,s,bounce;};
AtmTransport atmIntegrateSegment(float3 p,float3 d,float3 sun,float start,float distance,uint count,bool multi,bool groundLight,bool isotropic,bool haze) {
    AtmTransport o;o.t=1;o.s=0;o.bounce=0;
    bool ground;float end=atmEnd(p,d,ground);ground=ground && end<=distance;end=min(distance,end);
    float mu=dot(d,sun),g=atmosphere.mie.w;
    float pr=isotropic?1/(4*atmPi):3*(1+mu*mu)/(16*atmPi);
    float pm=isotropic?1/(4*atmPi):(1-g*g)/(4*atmPi*pow(max(1e-4,1+g*g-2*g*mu),1.5));
    [loop]for(uint i=0;i<count;++i){
        float a=float(i)/count,b=float(i+1)/count;
        float begin=min(start,end);float t0=lerp(begin,end,a*a),t1=lerp(begin,end,b*b),dt=t1-t0;
        float3 q=p+d*((t0+t1)*.5),ext,r;float m;atmMedium(q,ext,r,m);
        float3 source=(r*pr+m*pm)*atmSunT(q,sun);
        if(multi && atmosphere.planet.w>0)source+=(r+m)*atmosphereMultiple.SampleLevel(atmosphereSampler,atmMultiUv(q,sun),0).rgb;
        if(haze && atmosphere.haze.x>0){
            float3 local=q-p;float density=atmosphere.haze.x*smoothstep(.65,1,max(abs(local.x),abs(local.z))/max(.001,atmosphere.haze.y))*exp(-atmHeight(q)/2);
            ext+=density;source+=density*atmAnalytic(d);
        }
        float3 stepT=exp(-ext*dt),integral=(1-stepT)/max(ext,1e-9);
        integral=lerp(integral,dt.xxx,float3(ext.x<1e-9,ext.y<1e-9,ext.z<1e-9));
        o.s+=o.t*source*integral;o.bounce+=o.t*(r+m)*integral;o.t*=stepT;
    }
    if(ground && groundLight){float3 q=p+d*end,n=normalize(q);
        o.s+=o.t*atmosphere.planet.z*max(0,dot(n,sun))*atmSunT(q+n*.001,sun)/atmPi;
    }
    return o;
}

AtmTransport atmIntegrate(float3 p,float3 d,float3 sun,float distance,uint count,bool multi,bool groundLight,bool isotropic,bool haze) {
    return atmIntegrateSegment(p,d,sun,0,distance,count,multi,groundLight,isotropic,haze);
}

// Atmosphere algorithms adapted from Hillaire (EGSR 2020).
// Copyright (c) 2020 Epic Games, Inc. MIT attribution: third_party/sky_atmosphere/LICENSE.
#define ATM_SET 0
#define ATM_COMPUTE
#include "atmosphere_integrate.hlsli"
[[vk::binding(25,0), vk::image_format("rgba16f")]] RWTexture2D<float4> output2D;
[[vk::binding(26,0), vk::image_format("rgba16f")]] RWTexture3D<float4> outputT;
[[vk::binding(27,0), vk::image_format("rgba16f")]] RWTexture3D<float4> outputS;
[[vk::binding(28,0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> outputCube;
[[vk::binding(29,0), vk::image_format("rgba16f")]] RWTexture2D<float4> scene;
[[vk::binding(30,0)]] Texture2D<float> depth;
struct Push {uint width,height,pass,mip;};
[[vk::push_constant]] ConstantBuffer<Push> push;
float3 cubeRay(uint f,float2 uv){float u=uv.x,v=uv.y;
    if(f==0)return normalize(float3(1,-v,-u));if(f==1)return normalize(float3(-1,-v,u));
    if(f==2)return normalize(float3(u,1,v));if(f==3)return normalize(float3(u,-1,-v));
    if(f==4)return normalize(float3(u,-v,1));return normalize(float3(-u,-v,-1));}
float3 fibonacci(uint i,uint count){float y=1-2*(i+.5)/count,a=i*2.39996323;return float3(sqrt(1-y*y)*cos(a),y,sqrt(1-y*y)*sin(a));}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID){
    uint width=push.width,height=push.height,mip=push.mip;
    if(any(id.xy>=uint2(width,height)))return;
    if(ATM_PASS==0){
        float3 p,d;atmTransRay(float2(id.xy)/float2(width-1,height-1),p,d);
        bool ground;float distance=atmEnd(p,d,ground);float3 tau=0;
        [loop]for(uint i=0;i<128;++i){float3 ext,r;float m;atmMedium(p+d*((i+.5)*distance/128),ext,r,m);tau+=ext*distance/128;}
        output2D[id.xy]=float4(exp(-tau),1);
    }else if(ATM_PASS==1){
        float h=float(id.y)/(height-1)*atmosphere.planet.y,mu=float(id.x)/(width-1)*2-1;
        float3 p=float3(0,atmosphere.planet.x+max(.001,h),0),sun=float3(sqrt(saturate(1-mu*mu)),mu,0),sum=0,factor=0;
        [loop]for(uint j=0;j<64;++j){AtmTransport t=atmIntegrate(p,fibonacci(j,64),sun,1e6,24,false,true,true,false);sum+=t.s;factor+=t.bounce;}
        output2D[id.xy]=float4(sum/64/max(.02,1-factor/64),1);
    }else if(ATM_PASS==2){
        float3 d=atmSkyDirection(float2((id.x+.5)/width,float(id.y)/(height-1)));
        output2D[id.xy]=float4(atmIntegrate(atmOrigin(),d,atmosphere.sun.xyz,1e6,40,true,true,false,true).s,1);
    }else if(ATM_PASS==3){
        uint n=uint(atmosphere.extent.w);if(id.z>=n)return;
        float2 uv=(id.xy+.5)/float2(width,height);
        float4 p=mul(atmosphere.inverseRaster,float4(uv*2-1,0,1));float3 ray=normalize(p.xyz/p.w);
        float viewDepth=pow(float(id.z)/(n-1),2)*atmosphere.view.y;
        float directionDepth=mul(atmosphere.current,float4(ray,0)).w;
        AtmTransport t=atmIntegrate(atmOrigin(),ray,atmosphere.sun.xyz,viewDepth/max(directionDepth,.01)*.001,32,true,false,false,true);
        outputT[id]=float4(t.t,1);outputS[id]=float4(t.s,1);
    }else if(ATM_PASS==4){
        if(id.z>=6)return;float3 n=cubeRay(id.z,(id.xy+.5)/float2(width,height)*2-1),c=0;
        if(mip==0)c=atmSampleSky(n);
        else{
            float3 up=abs(n.y)<.99?float3(0,1,0):float3(1,0,0),t=normalize(cross(up,n)),b=cross(n,t);
            float rough=float(mip)/6,alpha=rough*rough,total=0;
            [loop]for(uint i=0;i<64;++i){float u=(i+.5)/64,v=frac(i*.61803398875),a=2*atmPi*v;
                float cosH=sqrt((1-u)/(1+(alpha*alpha-1)*u)),sinH=sqrt(saturate(1-cosH*cosH));
                float3 h=t*cos(a)*sinH+b*sin(a)*sinH+n*cosH,l=reflect(-n,h);float w=saturate(dot(n,l));
                c+=atmosphereEnvironment.SampleLevel(atmosphereSampler,l,0).rgb*w;total+=w;
            }c/=max(total,1e-6);
        }
        outputCube[id]=float4(c,1);
    }else if(ATM_PASS==5){
        if(id.y!=0 || id.x>=9)return;float3 c=0;
        [loop]for(uint i=0;i<1024;++i){float3 n=fibonacci(i,1024);
            float basis[9]={.2820947918,.4886025119*n.y,.4886025119*n.z,.4886025119*n.x,
            1.0925484306*n.x*n.y,1.0925484306*n.y*n.z,.3153915653*(3*n.z*n.z-1),1.0925484306*n.x*n.z,.5462742153*(n.x*n.x-n.y*n.y)};
            c+=atmosphereEnvironment.SampleLevel(atmosphereSampler,n,0).rgb*basis[id.x];
        }
        float convolution=id.x==0?atmPi:(id.x<4?2*atmPi/3:atmPi/4);
        atmosphereIrradiance[id.x]=float4(c*(4*atmPi/1024)*convolution,0);
    }else if(ATM_PASS==6){
        float z=depth.Load(int3(id.xy,0));if(z==0)return;
        float4 p=mul(atmosphere.inverseRaster,float4((id.xy+.5)/float2(width,height)*2-1,z,1));
        float4 c=scene[id.xy];scene[id.xy]=float4(atmApply(c.rgb,p.xyz/p.w),c.a);
    }
}

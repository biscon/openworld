#include "volumetrics_common.hlsli"
#include "atmosphere_integrate.hlsli"
#include "clusters.hlsli"
#include "shadow_types.hlsli"
[[vk::binding(10,1)]] ConstantBuffer<ShadowUniform> fogShadow;
[[vk::binding(11,1)]] StructuredBuffer<uint> fogCoverage;
[[vk::binding(12,1)]] Texture2DArray<float> fogShadowDepth;
[[vk::binding(13,1)]] SamplerComparisonState fogShadowComparison;
struct FogVolumeRecord {column_major float4x4 inverse;float4 extent,albedoDensity,options;};
[[vk::binding(0,0)]] StructuredBuffer<FogVolumeRecord> volumes;
[[vk::binding(1,0)]] StructuredBuffer<uint4> selected;
[[vk::binding(2,0)]] StructuredBuffer<float4> rejections;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> fogCounts;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> fogIndices;
[[vk::binding(5,0)]] RWStructuredBuffer<uint> counters;
[[vk::binding(6,0),vk::image_format("rgba16f")]] RWTexture3D<float4> outExt;
[[vk::binding(7,0),vk::image_format("rgba16f")]] RWTexture3D<float4> outRaw;
[[vk::binding(8,0),vk::image_format("rgba16f")]] RWTexture3D<float4> outSource;
[[vk::binding(9,0),vk::image_format("rgba16f")]] RWTexture3D<float4> outT;
[[vk::binding(10,0),vk::image_format("rgba16f")]] RWTexture3D<float4> outS;
[[vk::binding(11,0),vk::image_format("rgba16f")]] RWTexture2D<float4> outSky;
[[vk::binding(12,0),vk::image_format("rgba16f")]] RWTexture2D<float4> outSkyT;
[[vk::binding(13,0)]] Texture3D<float4> previousSource;
[[vk::binding(14,0)]] Texture3D<float4> previousExt;
[[vk::binding(15,0),vk::image_format("rgba16f")]] RWTexture2D<float4> scene;
[[vk::binding(16,0)]] Texture2D<float> sceneDepth;
struct Push {uint width,height,depth,flags;};
[[vk::push_constant]] ConstantBuffer<Push> push;
float phaseHG(float cosine,float g){return (1-g*g)/(4*atmPi*pow(max(1e-4,1+g*g-2*g*cosine),1.5));}
uint cellIndex(uint3 p){uint3 g=(uint3(fog.grid.xyz)+3)/4;return (p.z*g.y+p.y)*g.x+p.x;}
float3 positionAt(float2 uv,float depth){float3 ray=fogRay(uv);return ray*(depth/fogDirectionDepth(ray));}
void cellBounds(uint3 cell,out float3 low,out float3 high){
    uint3 a=cell*4,b=min(a+4,uint3(fog.grid.xyz));low=1e30;high=-1e30;
    [unroll]for(uint k=0;k<8;++k){float3 p=positionAt(float2((k&1)?b.x:a.x,(k&2)?b.y:a.y)/fog.grid.xy,fogDepth((k&4)?b.z:a.z));low=min(low,p);high=max(high,p);}
}
float volumeDensity(FogVolumeRecord v,float3 geo){
    float3 p=mul(v.inverse,float4(geo,1)).xyz,a=abs(p)/v.extent.xyz;
    float d=v.options.x==0?length(p)/v.extent.x:max(a.x,max(a.y,a.z));
    float f=saturate((1-d)/v.options.y);return v.albedoDensity.w*f*f*(3-2*f);
}
float shadowComparison(uint layer,float3 q){
    float2 uv=q.xy*.5+.5;
    float reference=q.z+fogShadow.sampling.x*fogShadow.ranges[layer].z*fogShadow.ranges[layer].w;
    if(fogShadow.options.w!=0)
        return fogShadowDepth.SampleCmpLevelZero(fogShadowComparison,float3(uv,layer),max(0,reference));
    float2 tex=uv*fogShadow.sampling.z-.5;int2 a=int2(floor(tex));float2 f=frac(tex);
    float v[4];[unroll]for(uint k=0;k<4;++k){float z=fogShadowDepth.Load(int4(a+int2(k&1,k>>1),layer,0));v[k]=(z==0||reference>=z)?1:0;}
    return lerp(lerp(v[0],v[1],f.x),lerp(v[2],v[3],f.x),f.y);
}
float shadowLayer(uint layer,float3 p,float3 dx,float3 dy,float3 dz,out float coverage){
    coverage=0;if((fogShadow.options.z&(1u<<layer))==0)return 1;
    float3 q=mul(fogShadow.matrices[layer],float4(p,1)).xyz;float2 uv=q.xy*.5+.5;
    float3 sx=mul((float3x3)fogShadow.matrices[layer],dx);
    float3 sy=mul((float3x3)fogShadow.matrices[layer],dy);
    float3 sz=mul((float3x3)fogShadow.matrices[layer],dz);
    float3 radius=(abs(sx)+abs(sy)+abs(sz))*.5;
    float guard=2/fogShadow.sampling.z;
    if(any(uv-radius.xy*.5<guard)||any(uv+radius.xy*.5>1-guard)||q.z-radius.z<0||q.z+radius.z>1)return 1;
    coverage=1;
    if((fogShadow.options.z&(1u<<(layer+8)))!=0){float2 c=clamp(uv*32-.5,0,31);uint2 a=min(uint2(c),30);float2 f=c-a;
        uint at=layer*1024+a.y*32+a.x;
        coverage=lerp(lerp(float(fogCoverage[at]),float(fogCoverage[at+1]),f.x),lerp(float(fogCoverage[at+32]),float(fogCoverage[at+33]),f.x),f.y);}
    // A fog cell represents a volume, not a point. Integrate visibility over
    // its XY footprint and this density stratum before storing the source.
    // Fixed samples preserve stationary opacity/lighting and cannot blink.
    // Spend a bounded budget along the longest projected axis. A fixed 2x2
    // lateral lattice aliases coarse cells; a fixed depth-heavy lattice wastes
    // samples when looking along the light. Two depth taps also handle crossing
    // a caster when the ray projects to almost zero shadow-map area.
    float3 footprint=float3(length(sx.xy),length(sy.xy),length(sz.xy))*(fogShadow.sampling.z*.5);
    uint3 taps=uint3(1,1,2);
    [unroll]for(uint split=0;split<3;++split){
        float3 spacing=footprint/float3(taps);
        if(max(spacing.x,max(spacing.y,spacing.z))<=1)break;
        if(spacing.x>=spacing.y&&spacing.x>=spacing.z)taps.x*=2;
        else if(spacing.y>=spacing.z)taps.y*=2;
        else taps.z*=2;
    }
    float sum=0;
    [loop]for(uint iz=0;iz<taps.z;++iz) [loop]for(uint iy=0;iy<taps.y;++iy) [loop]for(uint ix=0;ix<taps.x;++ix){
        float3 offset=(float3(ix,iy,iz)+.5)/float3(taps)-.5;
        sum+=shadowComparison(layer,q+sx*offset.x+sy*offset.y+sz*offset.z);
    }
    return sum/(taps.x*taps.y*taps.z);
}
float volumeShadow(float3 p,float3 dx,float3 dy,float3 dz,out float coverage){
    coverage=0;if(fogShadow.options.x==0)return 1;
    float distance=dot(p,fogShadow.forward.xyz);uint count=fogShadow.options.y,layer=count-1;
    [unroll]for(uint k=0;k<5;++k)if(k<count&&distance<fogShadow.ranges[k].y){layer=k;break;}
    float c,v=shadowLayer(layer,p,dx,dy,dz,c),weight=1;
    if(layer+1<count)weight=1-smoothstep(fogShadow.ranges[layer+1].x,fogShadow.ranges[layer].y,distance);
    float total=v*c*weight,known=c*weight;
    [loop]for(uint k=layer+1;k<count&&known<.9999;++k){v=shadowLayer(k,p,dx,dy,dz,c);float w=(1-known)*c;total+=v*w;known+=w;}
    float far=fogShadow.ranges[count-1].y,fade=(1-smoothstep(far*.9,far,distance))*fogShadow.sun.w;
    coverage=known*fade;return lerp(1,total+1-known,fade);
}
// Per-metre physical atmosphere source in current pre-exposed units.
void atmosphericCell(float3 position,float3 ray,out float3 ext,out float3 source){
    ext=0;source=0;if(atmosphere.options.x!=2)return;
    float3 p=atmOrigin()+position*.001;bool ground;float end=atmEnd(atmOrigin(),ray,ground);
    if(length(position)*.001>end)return;
    float3 r;float m;atmMedium(p,ext,r,m);
    float mu=dot(ray,atmosphere.sun.xyz);
    source=(r*(3*(1+mu*mu)/(16*atmPi))+m*phaseHG(mu,atmosphere.mie.w))*atmSunT(p,atmosphere.sun.xyz);
    if(atmosphere.planet.w>0)source+=(r+m)*atmosphereMultiple.SampleLevel(atmosphereSampler,atmMultiUv(p,atmosphere.sun.xyz),0).rgb;
    if(atmosphere.haze.x>0){float density=atmosphere.haze.x*smoothstep(.65,1,max(abs(position.x),abs(position.z))*.001/max(.001,atmosphere.haze.y))*exp(-atmHeight(p)/2);
        ext+=density;source+=density*atmAnalytic(ray);}
    ext*=.001;source*=.001*atmosphere.illumination.rgb*atmosphere.illumination.w;
}
float3 localRadiance(LocalLightRecord light,float3 position,float3 ray){
    float3 delta=light.positionRange.xyz-position;float d2=dot(delta,delta),range2=light.positionRange.w*light.positionRange.w;
    if(d2>=range2)return 0;float3 l=d2>1e-12?delta*rsqrt(d2):float3(0,1,0);
    float a=saturate(1-(d2/range2)*(d2/range2)),cone=1;
    if(light.parameters.z!=0&&d2>1e-12)cone=saturate((dot(-l,light.directionOuter.xyz)-light.directionOuter.w)/(light.parameters.y-light.directionOuter.w));
    return light.colourIntensity.rgb*light.colourIntensity.w*light.parameters.w*(a*a*cone*cone/max(d2,light.parameters.x*light.parameters.x))*phaseHG(dot(ray,l),fog.height.w)*fog.illumination.w;
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(any(id>=uint3(push.width,push.height,push.depth)))return;
#if FOG_PASS == 0
    float3 lo,hi;cellBounds(id,lo,hi);uint at=cellIndex(id),n=0;
    [loop]for(uint k=0;k<uint(fog.counts.y);++k){uint index=selected[k].x;LocalLightRecord l=localLights[index];
        float3 d=max(max(lo-l.positionRange.xyz,l.positionRange.xyz-hi),0);
        if(dot(d,d)<=l.positionRange.w*l.positionRange.w){if(n<uint(fog.counts.z))fogIndices[at*uint(fog.counts.z)+n]=index;++n;}}
    uint reject=0;
    [loop]for(uint k=0;k<uint(fog.counts.w);++k){float4 b=rejections[k];float3 delta=max(max(lo-b.xyz,b.xyz-hi),0);
        if(dot(delta,delta)<=b.w*b.w){reject=0x80000000u;break;}}
    fogCounts[at]=min(n,uint(fog.counts.z))|reject;if(n>uint(fog.counts.z))InterlockedAdd(counters[0],1);
#elif FOG_PASS == 1
    float3 ext=0,source=0;float coverage=0,reactive=0;
    float z0=fogDepth(id.z),z1=fogDepth(id.z+1);uint count=uint(fog.depth.z);
    float2 uv=(id.xy+.5)/fog.grid.xy;
    float3 ray=fogRay(uv),along=ray/fogDirectionDepth(ray),dxStep=0,dyStep=0;
    if(fogShadow.options.x!=0){
        dxStep=positionAt(uv+float2(1/fog.grid.x,0),1)-along;
        dyStep=positionAt(uv+float2(0,1/fog.grid.y),1)-along;
    }
    uint cluster=cellIndex(id/4);reactive=(fogCounts[cluster]&0x80000000u)!=0?1:0;
    [loop]for(uint sample=0;sample<count;++sample){
        // Current extinction is not accumulated: moving these samples in time
        // would change opacity every frame. Lighting uses the same fixed strata.
        float z=lerp(z0,z1,(sample+.5)/count);
        float3 p=along*z,geo=fogUncurve(p,fog.curvature);
        float density=fog.height.x*exp(clamp((fog.height.y-geo.y)/fog.height.z,-80,16));
        density=min(density,10);float3 scattering=density*fog.albedo.rgb;
        [loop]for(uint k=0;k<uint(fog.counts.x);++k){float d=volumeDensity(volumes[k],geo);density+=d;scattering+=d*volumes[k].albedoDensity.rgb;}
        float fade=1-smoothstep(fog.depth.x*.9,fog.depth.x,z);density*=fade;scattering*=fade;
        float c,visibility=volumeShadow(p,dxStep*z,dyStep*z,along*((z1-z0)/count),c);coverage+=c;
        float3 sunlight=fog.sun.w*fog.illumination.rgb*fog.illumination.w*phaseHG(dot(ray,fog.sun.xyz),fog.height.w)*visibility;
        if(atmosphere.options.x==2)sunlight*=atmSunT(atmOrigin()+p*.001,atmosphere.sun.xyz);
        float3 local=sunlight;
        [loop]for(uint k=0;k<(fogCounts[cluster]&0x7fffffffu);++k)local+=localRadiance(localLights[fogIndices[cluster*uint(fog.counts.z)+k]],p,ray);
        float3 e=0,s=0;if(atmosphere.options.y!=0)atmosphericCell(p,ray,e,s);
        ext+=density+e;source+=scattering*local+s;
    }
    outExt[id]=float4(min(ext/count,65504),coverage/count);
    outRaw[id]=float4(min(source/count,65504),reactive);
#elif FOG_PASS == 2
    float4 raw=outRaw[id],ext=outExt[id];float3 result=raw.rgb;bool accept=fog.temporal.x!=0&&raw.a==0;
    float depth=(fogDepth(id.z)+fogDepth(id.z+1))*.5;
    float3 p=positionAt((id.xy+.5)/fog.grid.xy,depth);
    float3 old=curvePosition(fogUncurve(p,fog.curvature)+fog.originDelta.xyz,fog.previousCurvature);
    float4 clip=mul(fog.previous,float4(old,1));float3 uv=float3(clip.xy/max(clip.w,1e-6)*.5+.5,fogHistoryDepth(clip.w));
    accept=accept&&clip.w>0&&all(uv>=0)&&all(uv<=1);
    if(accept){float3 pe=previousExt.SampleLevel(atmosphereSampler,uv,0).rgb;
        accept=all(abs(pe-ext.rgb)<=max(1e-6,ext.rgb*.1));}
    if(accept){float3 lo=raw.rgb,hi=raw.rgb;
        [unroll]for(uint k=0;k<6;++k){int3 offset=0;offset[k/2]=(k&1)?1:-1;
            float3 s=outRaw[clamp(int3(id)+offset,0,int3(fog.grid.xyz)-1)].rgb;lo=min(lo,s);hi=max(hi,s);}
        float3 history=previousSource.SampleLevel(atmosphereSampler,uv,0).rgb*fog.temporal.z;
        float change=length(history-raw.rgb)/max(1e-6,length(raw.rgb));
        if(change>1)accept=false;
        result=lerp(raw.rgb,clamp(history,lo,hi),accept?fog.temporal.y*(1-saturate(change)):0);
    }
    // The spatial reference disables only volume accumulation. Image TAA still
    // receives genuine moving-light rejection, rather than a global rejection.
    bool rejected=fog.jitter.z!=0&&!accept;
    float reactive=rejected?1:raw.a;if(rejected)InterlockedAdd(counters[1],1);
    outSource[id]=float4(min(max(result,0),65504),reactive);
#elif FOG_PASS == 3
    float3 t=1,s=0;float reactive=0;outT[uint3(id.xy,0)]=float4(1,1,1,0);outS[uint3(id.xy,0)]=0;
    float scale=1/fogDirectionDepth(fogRay((id.xy+.5)/fog.grid.xy));
    [loop]for(uint k=0;k<uint(fog.grid.z);++k){float3 ext=outExt[uint3(id.xy,k)].rgb;float4 source=outSource[uint3(id.xy,k)];
        float dt=(fogDepth(k+1)-fogDepth(k))*scale;s+=t*source.rgb*fogIntegral(ext,dt);t*=exp(-ext*dt);reactive=max(reactive,source.a);
        outT[uint3(id.xy,k+1)]=float4(t,reactive);outS[uint3(id.xy,k+1)]=float4(min(s,65504),0);}
#elif FOG_PASS == 4
    float3 ray=fogRay((id.xy+.5)/fog.grid.xy);float scale=1/fogDirectionDepth(ray),distance=fog.depth.x*scale;
    float3 t=1,s=0;
    bool ground;float end=atmosphere.options.x==2?atmEnd(atmOrigin(),ray,ground)*1000:distance;
    [loop]for(uint k=0;k<uint(fog.grid.z);++k){float z0=fogDepth(k),z1=fogDepth(k+1),dt=max(0,min(z1*scale,end)-min(z0*scale,end));
        float3 ext=outExt[uint3(id.xy,k)].rgb,source=outSource[uint3(id.xy,k)].rgb;
        if(atmosphere.options.x==2&&atmosphere.options.y==0){float3 e,a;atmosphericCell(ray*((z0+z1)*.5*scale),ray,e,a);ext+=e;source+=a;}
        s+=t*source*fogIntegral(ext,dt);t*=exp(-ext*dt);}
    float3 tail=fog.background.rgb,diskT=1;
    if(atmosphere.options.x==2){AtmTransport a=atmIntegrateSegment(atmOrigin(),ray,atmosphere.sun.xyz,distance*.001,1e6,40,true,true,false,true);
        tail=a.s*atmosphere.illumination.rgb*atmosphere.illumination.w;
        bool hitsGround;float atmosphereEnd=atmEnd(atmOrigin(),ray,hitsGround);
        diskT=hitsGround?0:atmSunT(atmOrigin()+ray*min(distance*.001,atmosphereEnd),ray);
    }else if(atmosphere.options.x==1)tail=atmAnalytic(ray)*atmosphere.illumination.rgb*atmosphere.illumination.w;
    outSky[id.xy]=float4(min(s+t*tail,65504),0);outSkyT[id.xy]=float4(t*diskT,0);
#elif FOG_PASS == 5
    float z=sceneDepth.Load(int3(id.xy,0));if(z==0)return;
    float2 uv=(id.xy+.5)/float2(push.width,push.height)-fog.jitter.xy;
    float4 p=mul(fog.inverseViewProjection,float4(uv*2-1,z,1));
    float4 c=scene[id.xy];scene[id.xy]=float4(mediaApply(c.rgb,p.xyz/p.w),c.a);
#endif
}

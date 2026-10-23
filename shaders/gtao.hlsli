// Horizon integration, edge/normal and depth-filter formulas adapted from Intel XeGTAO.
// Copyright (C) 2016-2021, Intel Corporation. SPDX-License-Identifier: MIT
// Attribution and license: third_party/xegtao/.
struct GtaoPush { uint4 dims, pass; float4 projection, effect; };
[[vk::push_constant]] ConstantBuffer<GtaoPush> ao;
[[vk::binding(0,0)]] Texture2D<float> sourceDepth;
[[vk::binding(1,0)]] Texture2D<float> viewDepth;
[[vk::binding(2,0)]] Texture2D<float> rawAO;
[[vk::binding(3,0)]] Texture2D<float> edges;
[[vk::binding(4,0)]] Texture2D<float> filteredAO;
[[vk::binding(5,0)]] Texture2D<uint> offsets;
[[vk::binding(6,0)]] RWTexture2D<float> depthOut;
#ifdef OW_GTAO_COMPACT
[[vk::image_format("r8")]] [[vk::binding(7,0)]] RWTexture2D<float> rawOut;
[[vk::image_format("r8")]] [[vk::binding(8,0)]] RWTexture2D<float> edgeOut;
[[vk::image_format("r8")]] [[vk::binding(9,0)]] RWTexture2D<float> filterOut;
[[vk::image_format("r8ui")]] [[vk::binding(10,0)]] RWTexture2D<uint> offsetOut;
#else
[[vk::binding(7,0)]] RWTexture2D<float> rawOut;
[[vk::binding(8,0)]] RWTexture2D<float> edgeOut;
[[vk::binding(9,0)]] RWTexture2D<float> filterOut;
[[vk::binding(10,0)]] RWTexture2D<uint> offsetOut;
#endif
[[vk::image_format("rgba8")]] [[vk::binding(11,0)]] RWTexture2D<float4> resultOut;
static const float PI=3.14159265358979323846;
float3 unit(float3 v) { return v*rsqrt(max(dot(v,v),1e-20)); }
bool nativeValid(int2 p) { return all(p>=0)&&all(p<int2(ao.dims.xy)); }
bool workValid(int2 p) { return all(p>=0)&&all(p<int2(ao.dims.zw)); }
float linearDepth(int2 p) {
    if(!nativeValid(p)) return 0;
    float d=sourceDepth.Load(int3(p,0));
    return d>0 ? ao.projection.z/(d+ao.projection.w) : 0;
}
float3 positionAt(float2 pixel,float z) {
    float2 ndc=(pixel+.5-ao.effect.yz)/float2(ao.dims.xy)*2-1;
    return float3(ndc.x*ao.projection.x*z,-ndc.y*ao.projection.y*z,z);
}
float4 edgeWeights(float c,float l,float r,float t,float b) {
    float4 delta=float4(l,r,t,b)-c;
    float2 slope=float2(r-l,b-t)*.5;
    float4 adjusted=delta+float4(slope.x,-slope.x,slope.y,-slope.y);
    return saturate(1.25-min(abs(delta),abs(adjusted))/max(c*.011,1e-5))*float4(l>0,r>0,t>0,b>0);
}
float3 geometricNormal(int2 p) {
    float z=linearDepth(p);
    if(z<=0) return float3(0,0,-1);
    float l=linearDepth(p+int2(-1,0)),r=linearDepth(p+int2(1,0));
    float t=linearDepth(p+int2(0,-1)),b=linearDepth(p+int2(0,1));
    float4 e=edgeWeights(z,l,r,t,b);
    float3 c=positionAt(p,z);
    float3 L=unit(positionAt(p+int2(-1,0),l)-c),R=unit(positionAt(p+int2(1,0),r)-c);
    float3 T=unit(positionAt(p+int2(0,-1),t)-c),B=unit(positionAt(p+int2(0,1),b)-c);
    float3 n=e.x*e.z*cross(L,T)+e.z*e.y*cross(T,R)+e.y*e.w*cross(R,B)+e.w*e.x*cross(B,L);
    // A missing reliable triangle at a silhouette is unknown, not a view-facing surface.
    return dot(n,n)>1e-12 ? unit(n) : float3(0,0,0);
}
uint2 representative(uint2 p) {
    uint o=offsets.Load(int3(p,0));
    return p*ao.pass.x+uint2(o&1,o>>1);
}
float depthAt(int2 p) { return workValid(p)?viewDepth.Load(int3(p,0)):0; }
float4 unpackEdges(int2 p) {
    if(!workValid(p))return 0;
    uint e=uint(round(edges.Load(int3(p,0))*255));
    return float4((e>>6)&3,(e>>4)&3,(e>>2)&3,e&3)/3.;
}
float fade(float z) {
    float radiusPixels=ao.effect.x*ao.dims.y/(2*ao.projection.y*max(z,1e-5));
    return (1-smoothstep(100,200,z))*smoothstep(1,4,radiusPixels);
}
[numthreads(8,8,1)]
void prepare(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=ao.dims.zw))return;
    uint2 base=p*ao.pass.x;float nearest=0;uint selected=0;
    [unroll]for(uint y=0;y<2;++y)[unroll]for(uint x=0;x<2;++x) {
        if(x>=ao.pass.x||y>=ao.pass.x)continue;
        int2 q=base+uint2(x,y);if(!nativeValid(q))continue;
        float d=sourceDepth.Load(int3(q,0));
        if(d>nearest){nearest=d;selected=x+2*y;}
    }
    offsetOut[p]=selected;
    depthOut[p]=nearest>0?ao.projection.z/(nearest+ao.projection.w):0;
}
[numthreads(8,8,1)]
void reduceDepth(uint3 tid:SV_DispatchThreadID) {
    uint level=ao.pass.y;uint2 prev=max(ao.dims.zw>>(level-1),1),size=max(ao.dims.zw>>level,1);
    uint2 p=tid.xy;if(any(p>=size))return;
    uint2 begin=p*2,end=min(begin+2,prev);
    if(p.x+1==size.x)end.x=prev.x;if(p.y+1==size.y)end.y=prev.y;
    float farthest=0;
    [loop]for(uint y=begin.y;y<end.y;++y)[loop]for(uint x=begin.x;x<end.x;++x)
        farthest=max(farthest,viewDepth.Load(int3(uint2(x,y),level-1)));
    float sum=0,weight=0;
    [loop]for(uint y=begin.y;y<end.y;++y)[loop]for(uint x=begin.x;x<end.x;++x) {
        float z=viewDepth.Load(int3(uint2(x,y),level-1));
        float w=z>0?saturate(1-(farthest-z)/(.75*ao.effect.x*1.457*.615)):0;
        sum+=z*w;weight+=w;
    }
    depthOut[p]=weight>0?sum/weight:0;
}
[numthreads(8,8,1)]
void evaluate(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=ao.dims.zw))return;
    float z=depthAt(p);
    float4 e=edgeWeights(z,depthAt(int2(p)+int2(-1,0)),depthAt(int2(p)+int2(1,0)),
        depthAt(int2(p)+int2(0,-1)),depthAt(int2(p)+int2(0,1)));
    edgeOut[p]=dot(round(e*2.9),float4(64,16,4,1))/255.;
    if(z<=0||fade(z)<=0){rawOut[p]=1;return;}
    uint2 np=representative(p);
    float3 n=geometricNormal(np),c=positionAt(np,z*.99999),v=-unit(c);
    if(dot(n,n)<.5){rawOut[p]=1;return;}
    float radius=ao.effect.x*1.457;
    float search=min(128.,radius*ao.dims.y/(2*ao.projection.y*z));
    // Fixed interleaved gradient/R2-style spatial rotations: never vary with frame index.
    float noise=frac(52.9829189*frac(dot(float2(np),float2(.06711056,.00583715))));
    noise=frac(noise+ao.effect.w*.618033989);
    float noiseStep=frac(noise+.754877666);
    float visibility=0,validSamples=0;
    [unroll]for(uint slice=0;slice<3;++slice) {
        float phi=(slice+noise)*PI/3;
        float2 direction=float2(cos(phi),sin(phi));
        float3 dir=float3(direction,0),ortho=dir-dot(dir,v)*v;
        float3 axis=unit(cross(ortho,v)),projected=n-axis*dot(n,axis);
        float len=length(projected),cosN=saturate(dot(projected,v)/max(len,1e-6));
        float angle=sign(dot(ortho,projected))*acos(cosN);
        float2 low=cos(float2(angle+PI*.5,angle-PI*.5)),horizon=low;
        [unroll]for(uint step=0;step<3;++step) {
            float s=(step+frac(noiseStep+(slice+step*3)*.618033989))/3;
            float distancePixels=min(search,max(1.3,s*s*search));
            uint mip=uint(clamp(floor(log2(max(distancePixels/ao.pass.x,1))-3.3),0,float(ao.pass.z-1)));
            float2 delta=round(float2(direction.x,-direction.y)*distancePixels);
            [unroll]for(uint side=0;side<2;++side) {
                int2 q=int2(np)+int2(delta)*(side==0?1:-1);
                if(!nativeValid(q))continue;
                validSamples+=1;
                uint2 msize=max(ao.dims.zw>>mip,1);
                uint2 cell=min((uint2(q)/ao.pass.x)>>mip,msize-1);
                // Native taps keep the horizon sample on its actual slice. A nearest
                // half-resolution representative can be off-slice and falsely occlude a plane.
                float sampleZ=mip==0 ? linearDepth(q) : viewDepth.Load(int3(cell,mip));
                if(sampleZ<=0)continue;
                float2 samplePixel = float2(q);
                float3 d=positionAt(samplePixel,sampleZ)-c;float dist=length(d);
                float weight=saturate((radius-dist)/(radius*.615));
                float h=dot(d/max(dist,1e-6),v);
                horizon[side]=max(horizon[side],lerp(low[side],h,weight));
            }
        }
        float h0=-acos(clamp(horizon.y,-1,1)),h1=acos(clamp(horizon.x,-1,1));
        float i0=(cosN+2*h0*sin(angle)-cos(2*h0-angle))*.25;
        float i1=(cosN+2*h1*sin(angle)-cos(2*h1-angle))*.25;
        visibility+=lerp(len,1,.05)*(i0+i1);
    }
    visibility=max(.03,pow(saturate(visibility/3),2.2));
    rawOut[p]=lerp(1,visibility,validSamples/18.);
}
// One 8x8 output group shares a 12x12 source tile, including its two-pixel halo.
// Fixed scratch: 2,884 bytes. Partial groups must reach both barriers.
groupshared float filterRaw[144];
groupshared float4 filterEdges[144];
groupshared uint filterHasOcclusion;
float4 axisPaths(float4 a, float4 b, float4 c, float4 d, float4 e, uint axis) {
    uint neg=axis*2,pos=neg+1;
    float left=c[neg]*b[pos],right=c[pos]*d[neg];
    return float4(left*b[neg]*a[pos],left,right,right*d[pos]*e[neg]);
}
[numthreads(8,8,1)]
void denoise(uint3 tid:SV_DispatchThreadID,uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID,
             uint lane:SV_GroupIndex) {
    if(lane==0)filterHasOcclusion=0;
    GroupMemoryBarrierWithGroupSync();
    bool occluded=false;
    for(uint i=lane;i<144;i+=64) {
        int2 q=int2(group.xy*8)+int2(i%12,i/12)-2;
        float v=workValid(q)?rawAO.Load(int3(q,0)):1;
        filterRaw[i]=v;
        filterEdges[i]=unpackEdges(q);
        occluded=occluded||v<1;
    }
    if(occluded)InterlockedOr(filterHasOcclusion,1);
    GroupMemoryBarrierWithGroupSync();
    int2 p=tid.xy;if(!workValid(p))return;
    if(filterHasOcclusion==0||depthAt(p)<=0){filterOut[p]=1;return;}

    // Cache each row/column's paths from its centre. The two L-shaped paths
    // to every tap then need only two products, instead of repeated edge walks.
    float4 horizontal[5],vertical[5];
    uint start=local.y*12+local.x;
    [unroll]for(uint j=0;j<5;++j) {
        uint row=start+j*12,col=start+j;
        horizontal[j]=axisPaths(filterEdges[row],filterEdges[row+1],filterEdges[row+2],
                                filterEdges[row+3],filterEdges[row+4],0);
        vertical[j]=axisPaths(filterEdges[col],filterEdges[col+12],filterEdges[col+24],
                              filterEdges[col+36],filterEdges[col+48],1);
    }
    static const float gaussian[5]={.367879441,.778800783,1,.778800783,.367879441};
    float sum=0,weights=0;
    [unroll]for(uint y=0;y<5;++y)[unroll]for(uint x=0;x<5;++x) {
        float centreX=x==2?1:horizontal[2][x<2?x:x-1];
        float centreY=y==2?1:vertical[2][y<2?y:y-1];
        float rowX=x==2?1:horizontal[y][x<2?x:x-1];
        float colY=y==2?1:vertical[x][y<2?y:y-1];
        // Outside-image edges are zero; explicitly exclude those taps so the
        // normalisation matches the truncated kernel at image boundaries.
        if(!workValid(p+int2(x,y)-2))continue;
        float w=gaussian[x]*gaussian[y]*.5*(centreX*colY+centreY*rowX);
        sum+=filterRaw[start+y*12+x]*w;weights+=w;
    }
    filterOut[p]=saturate(sum/max(weights,1e-6));
}
[numthreads(8,8,1)]
void resolve(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=ao.dims.xy))return;
    float z=linearDepth(p);
    if(z<=0){resultOut[p]=1;return;}
    // Diagnostics are independent of the lighting fade and need no upsampling.
    if(ao.pass.w==2){resultOut[p]=float4(rawAO.Load(int3(p/ao.pass.x,0)).xxx,1);return;}
    if(ao.pass.w==3){resultOut[p]=float4(saturate(geometricNormal(p)*.5+.5),1);return;}
    if(ao.pass.w==4){resultOut[p]=float4((1-dot(unpackEdges(p/ao.pass.x),.25.xxxx)).xxx,1);return;}
    float strength=fade(z);
    if(strength<=0){resultOut[p]=1;return;}
    float visibility=1;
    if(ao.pass.x==1) {
        visibility=filteredAO.Load(int3(p,0));
        if(visibility>=1){resultOut[p]=1;return;}
        float3 n=geometricNormal(p);
        if(dot(n,n)<.5)visibility=1;
    } else {
        int2 base=int2(floor((float2(p)+.5)/2-.5));
        float4 taps=1;
        [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x) {
            int2 q=base+int2(x,y);
            if(workValid(q))taps[y*2+x]=filteredAO.Load(int3(q,0));
        }
        if(all(taps>=1)){resultOut[p]=1;return;}
        float3 n=geometricNormal(p),c=positionAt(p,z);
        if(dot(n,n)<.5){resultOut[p]=1;return;}
        float total=0,weights=0;
        [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x) {
            int2 q=base+int2(x,y);if(!workValid(q))continue;
            float qz=depthAt(q);if(qz<=0)continue;
            uint2 qp=representative(q);float3 qn=geometricNormal(qp),delta=positionAt(qp,qz)-c;
            float tolerance=max(.01,max(ao.effect.x*.02,z*.0002));
            float error=max(abs(dot(delta,n)),abs(dot(delta,qn)));
            float normalWeight=smoothstep(.8,.98,dot(n,qn));
            float2 spatial=max(0,1-abs(float2(qp)-float2(p))/3);
            float w=spatial.x*spatial.y*normalWeight*(1-smoothstep(tolerance,tolerance*2,error));
            total+=taps[y*2+x]*w;weights+=w;
        }
        if(weights>1e-5)visibility=total/weights;
    }
    resultOut[p]=float4(saturate(lerp(1,visibility,strength)).xxx,1);
}

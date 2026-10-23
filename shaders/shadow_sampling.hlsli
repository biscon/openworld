#ifndef OW_SHADOW_SAMPLING
#define OW_SHADOW_SAMPLING
#include "shadow_types.hlsli"
[[vk::binding(10,1)]] ConstantBuffer<ShadowUniform> sunShadow;
[[vk::binding(11,1)]] StructuredBuffer<uint> sunCoverage;
[[vk::binding(12,1)]] Texture2DArray<float> sunDepth;
[[vk::binding(13,1)]] SamplerComparisonState sunComparison;
float shadowCoverage(uint layer,float2 uv) {
    if((sunShadow.options.z&(1u<<(layer+8)))==0)return 1;
    float2 p=clamp(uv*32-.5,0,31);uint2 a=min(uint2(p),30);float2 f=p-a;
    uint at=layer*1024+a.y*32+a.x;
    return lerp(lerp(float(sunCoverage[at]),float(sunCoverage[at+1]),f.x),
                lerp(float(sunCoverage[at+32]),float(sunCoverage[at+33]),f.x),f.y);
}
float comparison(uint layer,float2 uv,float reference) {
    if(sunShadow.options.w!=0)return sunDepth.SampleCmpLevelZero(sunComparison,float3(uv,layer),reference);
    float2 p=uv*sunShadow.sampling.z-.5;int2 a=int2(floor(p));float2 f=frac(p);
    int size=int(sunShadow.sampling.z);float4 values;
    [unroll]for(uint i=0;i<4;++i) {
        int2 q=a+int2(i&1,i>>1);
        float depth=all(q>=0)&&all(q<size)?sunDepth.Load(int4(q,layer,0)):0;
        values[i]=reference>=depth?1:0;
    }
    return lerp(lerp(values.x,values.y,f.x),lerp(values.z,values.w,f.x),f.y);
}
float cascadeVisibility(uint layer,float3 position,float3 geometricNormal,float3 worldDx,float3 worldDy,out float coverage) {
    coverage=0;
    if((sunShadow.options.z&(1u<<layer))==0)return 1;
    float4 range=sunShadow.ranges[layer];
    float3 offset=geometricNormal*(sunShadow.sampling.y*range.z);
    float3 p=mul(sunShadow.matrices[layer],float4(position+offset,1)).xyz;
    float2 uv=p.xy*.5+.5;
    float guard=2/sunShadow.sampling.z;
    if(any(uv<guard)||any(uv>1-guard)||p.z<0||p.z>1)return 1;
    coverage=shadowCoverage(layer,uv);
    // Receiver plane correction removes PCF slope error without material-normal bias.
    float3 dx=mul((float3x3)sunShadow.matrices[layer],worldDx),dy=mul((float3x3)sunShadow.matrices[layer],worldDy);float determinant=dx.x*dy.y-dx.y*dy.x;
    float2 gradient=abs(determinant)>1e-12?float2(dx.z*dy.y-dy.z*dx.y,dx.x*dy.z-dy.x*dx.z)/determinant:0;
    float bound=4*range.z*range.w;
    // A bilinear comparison uses four neighbours with one reference depth.
    // Cover their half-texel plane variation as well as the explicit tap offset.
    float samplingError=min(bound,(abs(gradient.x)+abs(gradient.y))/sunShadow.sampling.z);
    float sum=0;
    [unroll]for(uint i=0;i<4;++i) {
        float2 delta=(float2(i&1,i>>1)*2-1)*.5/sunShadow.sampling.z;
        float correction=clamp(dot(gradient,delta*2),-bound,bound);
        sum+=comparison(layer,uv+delta,p.z+sunShadow.sampling.x*range.z*range.w+samplingError+correction);
    }
    return sum*.25;
}
float sunVisibility(float3 position,float3 normal) {
    if(sunShadow.options.x==0)return 1;
    float distance=dot(position,sunShadow.forward.xyz);
    if(distance<0)return 1;
    float3 worldDx=ddx(position),worldDy=ddy(position);
    float3 geometric=cross(worldDx,worldDy);
    geometric=dot(geometric,geometric)>1e-16?normalize(geometric):normalize(normal);
    if(dot(geometric,normal)<0)geometric=-geometric;
    uint count=sunShadow.options.y,selected=count-1;
    [unroll]for(uint i=0;i<5;++i)if(i<count&&distance<sunShadow.ranges[i].y){selected=i;break;}
    float coverage,visibility=cascadeVisibility(selected,position,geometric,worldDx,worldDy,coverage);
    float weight=1;
    if(selected+1<count) {
        float blendStart=sunShadow.ranges[selected+1].x;
        weight=1-smoothstep(blendStart,sunShadow.ranges[selected].y,distance);
    }
    float total=visibility*coverage*weight,known=coverage*weight;
    // Only adjacent overlap normally samples twice. Invalid coverage can use a farther map.
    for(uint next=selected+1;next<count&&known<.9999;++next) {
        float otherCoverage;float other=cascadeVisibility(next,position,geometric,worldDx,worldDy,otherCoverage);
        float contribution=(1-known)*otherCoverage;total+=contribution*other;known+=contribution;
    }
    float far=sunShadow.ranges[count-1].y;
    float fade=(1-smoothstep(far*.9,far,distance))*sunShadow.sun.w;
    if(sunShadow.sampling.w==2)return float(selected+1)/5;
    if(sunShadow.sampling.w==3)return known*fade;
    return lerp(1,total+1-known,fade);
}
#endif

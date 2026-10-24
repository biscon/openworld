#include "lighting.hlsli"
#include "materials.hlsli"
struct MeshSurface {
#ifdef OW_VEGETATION
[[vk::location(10)]]nointerpolation float4 plant:TEXCOORD9;
[[vk::location(11)]]nointerpolation uint identity:TEXCOORD10;
[[vk::location(12)]]nointerpolation float4 foliage:TEXCOORD11;
#endif
[[vk::location(9)]]float3 renderedPosition:TEXCOORD8;[[vk::location(8)]]float4 previousClip:TEXCOORD7;float4 position:SV_Position;[[vk::location(0)]]float3 relative:TEXCOORD0;[[vk::location(1)]]float3 normal:NORMAL;[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD1;[[vk::location(4)]]nointerpolation uint material:TEXCOORD2;};
float4 meshTexture(uint index,float2 uv,float2 dx,float2 dy){
#ifdef OW_BINDLESS
    return materialImages[NonUniformResourceIndex(index)].SampleGrad(materialSampler,uv,dx,dy);
#else
    switch(index){
    case 0:return materialImages[0].SampleGrad(materialSampler,uv,dx,dy);
    case 1:return materialImages[1].SampleGrad(materialSampler,uv,dx,dy);
    case 2:return materialImages[2].SampleGrad(materialSampler,uv,dx,dy);
    case 3:return materialImages[3].SampleGrad(materialSampler,uv,dx,dy);
    case 4:return materialImages[4].SampleGrad(materialSampler,uv,dx,dy);
    case 5:return materialImages[5].SampleGrad(materialSampler,uv,dx,dy);
    case 6:return materialImages[6].SampleGrad(materialSampler,uv,dx,dy);
    default:return materialImages[7].SampleGrad(materialSampler,uv,dx,dy);}
#endif
}
float4 meshBase(MaterialRecord r,float2 uv,float2 dx,float2 dy){
    float4 base=r.base*meshTexture(r.meshMaps.x,uv,dx,dy);
    if(r.alpha.x==1 && base.a<r.alpha.y)discard;
    return base;
}
SurfaceMaterial meshSurface(MeshSurface i,MaterialRecord r,float4 base,float2 dx,float2 dy,bool front) {
    SurfaceMaterial m;m.baseColour=base.rgb;m.opacity=r.alpha.x==2?base.a:1;
    float3 n=safeNormal(i.normal),t=safeNormal(i.tangent.xyz-n*dot(n,i.tangent.xyz));
    float3 b=cross(n,t)*i.tangent.w;
    float2 xy=0;
    if(r.meshMaps.y!=1 && r.factors.w>0)xy=(meshTexture(r.meshMaps.y,i.uv,dx,dy).xy*2-1)*r.factors.w;
    float3 tn=float3(xy,sqrt(saturate(1-dot(xy,xy))));
    m.normal=safeNormal(t*tn.x+b*tn.y+n*tn.z);if(r.alpha.z!=0&&!front)m.normal=-m.normal;
    float3 orm=meshTexture(r.meshMaps.z,i.uv,dx,dy).rgb;
    m.ao=orm.r*r.factors.z;m.roughness=orm.g*r.factors.x;m.metallic=orm.b*r.factors.y;
    m.emissive=r.emission.rgb*meshTexture(r.meshMaps.w,i.uv,dx,dy).rgb;return m;
}

#ifdef OW_VEGETATION
void vegetationMask(MeshSurface i) {
    // Stable plant-local coverage noise shared by camera/depth/shadow evaluation.
    uint2 q=uint2(saturate(i.uv)*128);
    uint h=q.x*1973+q.y*9277+i.identity*26699;h^=h>>16;h*=0x7feb352d;h^=h>>15;
    if(float(h>>8)*(1.0/16777216.0)>=i.plant.x)discard;
}
#endif

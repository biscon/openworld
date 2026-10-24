#include "vegetation_types.hlsli"
#include "lighting.hlsli"
[[vk::binding(2,0)]] StructuredBuffer<uint> visiblePlants;
[[vk::binding(3,0)]] StructuredBuffer<uint> work;
struct Input {[[vk::location(0)]]float3 position:POSITION;[[vk::location(1)]]float3 normal:NORMAL;[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD0;};
struct Output {float4 position:SV_Position;
[[vk::location(0)]]float3 relative:TEXCOORD0;[[vk::location(1)]]float3 normal:NORMAL;
[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD1;
[[vk::location(4)]]nointerpolation uint material:TEXCOORD2;
[[vk::location(8)]]float4 previousClip:TEXCOORD7;[[vk::location(9)]]float3 renderedPosition:TEXCOORD8;
[[vk::location(10)]]nointerpolation float4 plant:TEXCOORD9;
};
Output main(Input i,uint id:SV_InstanceID){
    uint view=plantPush.view,bin=plantPush.bin;
    uint index=visiblePlants[view*vegetation.limits.x+work[view*97+bin*8+7]+id];Plant p=plants[index];Output o;
    bool card=(bin%6)>=3;
    o.relative=plantPosition(p,i.position,vegetation.cameraTime,vegetation.settings.y,vegetation.cards.x,card);
    o.renderedPosition=curvePosition(o.relative,temporal.curvature);
    o.position=view==0?mul(lighting.viewProjection,float4(o.renderedPosition,1)):mul(vegetation.views[view],float4(o.renderedPosition,1));
    o.normal=rotatePlant(i.normal,p.yaw);
    float2 shear=float2(1,.45)*plantBend(p,vegetation.cameraTime.w,vegetation.settings.y)*2*i.position.y/p.rootScale.w;
    o.normal.y-=dot(o.normal.xz,shear);
    o.tangent=float4(rotatePlant(i.tangent.xyz,p.yaw),i.tangent.w);
    o.uv=i.uv;o.material=vegetation.geometry[bin].z;
    float d=length(p.rootScale.xyz-vegetation.cameraTime.xyz);
    float oldD=length(p.rootScale.xyz-vegetation.previousCameraTime.xyz);
    uint oldBin=plantBin(p,oldD,vegetation.modes.y);
    float coverage=plantCoverage(p,d,bin,vegetation.settings,vegetation.cards);
    if(view!=0)coverage*=1-smoothstep(max(0,vegetation.settings.w-5),vegetation.settings.w,d);
    bool valid=vegetation.modes.z!=0&&oldBin==bin&&plantCoverage(p,oldD,oldBin,vegetation.previousSettings,vegetation.previousCards)>0;
    float3 previous=plantPosition(p,i.position,vegetation.previousCameraTime,vegetation.previousSettings.y,vegetation.previousCards.x,card);
    o.previousClip=valid?mul(temporal.previous,float4(curvePosition(previous,temporal.previousCurvature),1)):0;
    o.plant=float4(coverage,p.variation,vegetation.cards.z,float(p.identity));return o;
}

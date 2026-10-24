#include "vegetation_types.hlsli"
#include "lighting.hlsli"
[[vk::binding(2,0)]] StructuredBuffer<uint> visiblePlants;
[[vk::binding(3,0)]] StructuredBuffer<uint> work;
struct Input {
[[vk::location(0)]]float3 position:POSITION;[[vk::location(1)]]float3 normal:NORMAL;
[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD0;
[[vk::location(4)]]float4 deformation:TEXCOORD3;
};
struct Output {
float4 position:SV_Position;
[[vk::location(0)]]float3 relative:TEXCOORD0;[[vk::location(1)]]float3 normal:NORMAL;
[[vk::location(2)]]float4 tangent:TANGENT;[[vk::location(3)]]float2 uv:TEXCOORD1;
[[vk::location(4)]]nointerpolation uint material:TEXCOORD2;
[[vk::location(8)]]float4 previousClip:TEXCOORD7;[[vk::location(9)]]float3 renderedPosition:TEXCOORD8;
[[vk::location(10)]]nointerpolation float4 plant:TEXCOORD9;
[[vk::location(11)]]nointerpolation uint identity:TEXCOORD10;
[[vk::location(12)]]nointerpolation float4 foliage:TEXCOORD11;
};
Output main(Input i,uint id:SV_InstanceID) {
    uint view=plantPush.view,bin=plantPush.bin;
    uint index=visiblePlants[vegetation.lists[view].x+work[view*plantViewWords+bin*8+7]+id];
    Plant p=loadPlant(index);Cell cell=cells[p.assetCell>>3];Output o;
    float d=length(p.rootScale.xyz+cell.origin.xyz),oldD=length(p.rootScale.xyz+cell.previousOrigin.xyz);
    uint asset=vegetation.geometry[bin].w,lod=plantLod(p,d,asset),oldAsset=plantAsset(p,vegetation.modes.y,oldD),oldLod=plantLod(p,oldD,oldAsset,true);
    o.relative=plantPosition(p,cell,i.position,i.deformation,vegetation.cameraTime.w,vegetation.settings.y,vegetation.wind.xy,asset,lod,false);
    o.renderedPosition=curvePosition(o.relative,temporal.curvature);
    o.position=view==0?mul(lighting.viewProjection,float4(o.renderedPosition,1)):mul(vegetation.views[view],float4(o.renderedPosition,1));
    o.normal=rotatePlant(i.normal,p.yaw);float3 tangent=rotatePlant(i.tangent.xyz,p.yaw);
    float2 shear=0;
    if(vegetation.profile.x!=0)shear=float2(1,.45)*plantBend(p,vegetation.cameraTime.w,vegetation.settings.y)*2*i.position.y/p.rootScale.w;
    else if(i.position.y>1e-5) {
        float bend=windBend(p,cell,i.deformation,vegetation.cameraTime.w,vegetation.settings.y,lod);
        shear=vegetation.wind.xy*(2*bend/i.position.y);
    }
    o.normal.y-=dot(o.normal.xz,shear);tangent.xz+=shear*tangent.y;
    o.normal.xz-=plantSlope*o.normal.y;tangent.y+=dot(plantSlope,tangent.xz);
    o.tangent=float4(tangent,i.tangent.w);o.uv=i.uv;o.material=vegetation.geometry[bin].z;
    float coverage=plantCoverage(p,d,asset,vegetation.settings,vegetation.cards,view);
    float oldCoverage=plantCoverage(p,oldD,oldAsset,vegetation.previousSettings,vegetation.previousCards);
    bool valid=vegetation.modes.z!=0&&(vegetation.inspection.y==0||cell.previousOrigin.w!=0)&&oldAsset==asset&&oldLod==lod&&oldCoverage>0;
    float3 previous=plantPosition(p,cell,i.position,i.deformation,vegetation.previousCameraTime.w,vegetation.previousSettings.y,vegetation.previousWind.xy,asset,lod,true);
    o.previousClip=valid?mul(temporal.previous,float4(curvePosition(previous,temporal.previousCurvature),1)):0;
    o.plant=float4(coverage,p.variation,vegetation.cards.z,oldCoverage);o.identity=p.identity;
    o.foliage=vegetation.foliage[bin];o.foliage.w*=vegetation.profile.w;
    return o;
}

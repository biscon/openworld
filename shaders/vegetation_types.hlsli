#ifndef OW_VEGETATION_TYPES
#define OW_VEGETATION_TYPES
static const uint plantViewWords=385;
struct Plant {float4 rootScale;float yaw;float variation;uint identity;uint assetCell;};
struct VegetationUniform {
    column_major float4x4 views[6];
    float4 cameraTime,previousCameraTime,settings,previousSettings,cards,previousCards;
    uint4 modes,limits,profile,inspection;
    float4 wind,previousWind;
    uint4 lists[6],geometry[48];
    float4 foliage[48],ranges[8],deformation[8],envelope[8];
    uint4 lods[24];
};
[[vk::binding(0,0)]] StructuredBuffer<Plant> plants;
[[vk::binding(1,0)]] ConstantBuffer<VegetationUniform> vegetation;
struct Cell {float4 lo,hi,origin,previousOrigin,phase,casterLo,casterHi;};
[[vk::binding(4,0)]] StructuredBuffer<Cell> cells;
[[vk::binding(7,0)]] StructuredBuffer<float4> attachments;
static float2 plantSlope=0;
Plant loadPlant(uint index) {
    Plant p=plants[index];
    if(vegetation.inspection.y!=0) {
        float4 a=attachments[index];
        p.rootScale.y=a.x;plantSlope=a.yz;
        if(a.w==0)p.rootScale.w=0;
    }
    return p;
}
struct VegetationPush {uint view,bin,phase,unused;};
[[vk::push_constant]] ConstantBuffer<VegetationPush> plantPush;
uint plantHash(uint x){x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);}
float plantRandom(uint x){return (plantHash(x)>>8)*(1.0/16777216.0);}
float3 rotatePlant(float3 p,float yaw){float s,c;sincos(yaw,s,c);return float3(c*p.x-s*p.z,p.y,s*p.x+c*p.z);}
uint plantAsset(Plant p,uint representation,float d) {
    if(vegetation.profile.x==0)return p.assetCell&7;
    bool card=representation==1||(representation==2&&d>lerp(35,45,plantRandom(p.identity+781)));
    return (p.identity%2)*2+(card?1:0);
}
uint plantLod(Plant p,float d,uint asset) {
    if(vegetation.profile.x==0&&vegetation.profile.z!=0xffffffff)return vegetation.profile.z;
    float jitter=(plantRandom(p.identity+781)-.5)*4;
    float2 ranges=vegetation.profile.x!=0?float2(20,45):vegetation.ranges[asset].xy;
    return d>ranges.y+jitter?2:d>ranges.x+jitter?1:0;
}
float plantCoverage(Plant p,float d,uint asset,float4 settings,float4 cards) {
    if(vegetation.profile.x==0) {
        if(vegetation.inspection.x!=0xffffffff&&vegetation.inspection.x!=p.identity)return 0;
        if(vegetation.profile.y==0xfffffffe) {if(vegetation.deformation[asset].z!=0)return 0;}
        else if(vegetation.profile.y!=0xffffffff&&vegetation.profile.y!=asset)return 0;
    }
    float thinning=vegetation.profile.x!=0||vegetation.deformation[asset].z==0?lerp(1,.4,smoothstep(18,50,d)):1;
    if(plantRandom(p.identity+1927)>settings.x*thinning*(vegetation.profile.x!=0&&asset%2!=0?cards.y:1))return 0;
    float end=vegetation.profile.x!=0?settings.z:min(settings.z,vegetation.ranges[asset].z);
    return 1-smoothstep(max(0,end-15),end,d);
}
float plantBend(Plant p,float time,float wind) {
    float phase=dot(p.rootScale.xz,float2(.17,.11))+time*1.6;
    return wind*(.16*sin(phase)+.045*sin(phase*2.3+p.variation*6.28));
}
// Returns displacement along wind; metadata guarantees <= .45 local metres at wind=2.
float windBend(Plant p,Cell cell,float4 weights,float time,float strength,uint lod) {
    float2 anchor=cell.phase.xy+p.rootScale.xz;
    float phase=dot(anchor,float2(7,5)*(6.2831853/256))+time*1.6;
    float gust=.16*sin(phase)+.035*sin(dot(anchor,float2(3,2)*(6.2831853/256))+time*.37);
    float flutter=.025*sin(time*4.7+weights.w*6.28+p.variation*6.28);
    return strength*vegetation.deformation[p.assetCell&7].x*(gust*weights.y+flutter*weights.z*(lod==0?1:lod==1?.35:0));
}
float3 plantPosition(Plant p,Cell cell,float3 vertex,float4 weights,float time,float strength,float2 direction,uint asset,uint lod,bool previous) {
    if(vegetation.profile.x!=0) {
        if(asset%2!=0)vertex.xz*=previous?vegetation.previousCards.x:vegetation.cards.x;
        float3 pos=rotatePlant(vertex*p.rootScale.w,p.yaw);
        pos.xz+=float2(1,.45)*plantBend(p,time,strength)*vertex.y*vertex.y;
        return pos+p.rootScale.xyz+(previous?cell.previousOrigin.xyz:cell.origin.xyz);
    }
    float3 pos=rotatePlant(vertex,p.yaw);
    pos.xz+=direction*windBend(p,cell,weights,time,strength,lod);
    pos.y+=dot(plantSlope,pos.xz);
    return pos*p.rootScale.w+p.rootScale.xyz+(previous?cell.previousOrigin.xyz:cell.origin.xyz);
}
bool plantBoxVisible(float3 lo,float3 hi,uint view) {
    float3 centre=(lo+hi)*.5,extent=(hi-lo)*.5;
    float4 c=mul(vegetation.views[view],float4(centre,1));
    float4 e=mul(abs(vegetation.views[view]),float4(extent,0));
    return c.x+e.x>=-c.w-e.w&&c.x-e.x<=c.w+e.w&&c.y+e.y>=-c.w-e.w&&c.y-e.y<=c.w+e.w&&c.z+e.z>=0&&c.z-e.z<=c.w+e.w;
}
#endif

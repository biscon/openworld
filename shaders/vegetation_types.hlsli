#ifndef OW_VEGETATION_TYPES
#define OW_VEGETATION_TYPES
struct Plant {float4 rootScale;float yaw;float variation;uint identity;uint cell;};
struct VegetationUniform {
    column_major float4x4 views[6];
    float4 cameraTime, previousCameraTime;
    float4 settings, previousSettings; // density,wind,range,shadowRange
    float4 cards, previousCards; // width,density,coverage,unused
    uint4 modes; // representation,previous representation,history,candidates
    uint4 limits; // list capacity,active views,side,enabled
    uint4 geometry[12]; // index count,first index,material,unused
};
[[vk::binding(0,0)]] StructuredBuffer<Plant> plants;
[[vk::binding(1,0)]] ConstantBuffer<VegetationUniform> vegetation;
struct VegetationPush {uint view;uint bin;uint phase;uint unused;};
[[vk::push_constant]] ConstantBuffer<VegetationPush> plantPush;
uint plantHash(uint x){x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);}
float plantRandom(uint x){return (plantHash(x)>>8)*(1.0/16777216.0);}
float3 rotatePlant(float3 p,float yaw){float s,c;sincos(yaw,s,c);return float3(c*p.x-s*p.z,p.y,s*p.x+c*p.z);}
uint plantBin(Plant p,float distance,uint representation) {
    float random=plantRandom(p.identity+781);
    uint lod=distance>lerp(18,22,random)?1:0;
    if(distance>lerp(43,47,random))lod=2;
    bool card=representation==1||(representation==2&&distance>lerp(35,45,random));
    return (p.identity%2)*6+(card?3:0)+lod;
}
float plantCoverage(Plant p,float d,uint bin,float4 settings,float4 cards) {
    float thinning=lerp(1,.4,smoothstep(18,50,d));
    if(plantRandom(p.identity+1927)>settings.x*thinning*((bin%6)>=3?cards.y:1))return 0;
    return 1-smoothstep(max(0,settings.z-15),settings.z,d);
}
float plantBend(Plant p,float time,float wind) {
    float phase=dot(p.rootScale.xz,float2(.17,.11))+time*1.6;
    return wind*(.16*sin(phase)+.045*sin(phase*2.3+p.variation*6.28));
}
float3 plantPosition(Plant p,float3 vertex,float4 cameraTime,float wind,float cardWidth,bool card) {
    if(card)vertex.xz*=cardWidth;
    float h=vertex.y;
    float3 pos=rotatePlant(vertex*p.rootScale.w,p.yaw);
    float bend=plantBend(p,cameraTime.w,wind);
    pos.xz+=float2(1,.45)*bend*h*h; // <= .45 m at maximum configured wind
    return pos+p.rootScale.xyz-cameraTime.xyz;
}
bool plantBoxVisible(float3 lo,float3 hi,uint view) {
    float3 centre=(lo+hi)*.5,extent=(hi-lo)*.5;
    float4 c=mul(vegetation.views[view],float4(centre,1));
    float4 e=mul(abs(vegetation.views[view]),float4(extent,0));
    return c.x+e.x>=-c.w-e.w && c.x-e.x<=c.w+e.w && c.y+e.y>=-c.w-e.w && c.y-e.y<=c.w+e.w && c.z+e.z>=0 && c.z-e.z<=c.w+e.w;
}
#endif

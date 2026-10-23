#include "terrain_common.hlsli"
#include "terrain_surface.hlsli"
[[vk::binding(2,0)]] StructuredBuffer<Sample> samples;
[[vk::binding(10,0)]] StructuredBuffer<uint> topology;
struct View { column_major float4x4 viewProjection; float4 observer; };
[[vk::binding(9,0)]] ConstantBuffer<View> view;
struct Draw {
    float4 offset;
    uint4 options;
    uint4 patch; // origin x,z, sampling step, cells (zero selects legacy full grid)
    float4 morph; // start, end, level, enabled
    uint4 normals; // current/next halo, normal subdivisions, reserved
    uint4 indices; // first index, unused
};
#ifdef TERRAIN_SHADOW
#include "shadow_resources.hlsli"
#else
[[vk::push_constant]] ConstantBuffer<Draw> drawPush;
#endif
struct StoredDraw { Draw data; uint4 reserved0, reserved1; };
[[vk::binding(0,3)]] StructuredBuffer<StoredDraw> drawRecords;
static Draw draw;
float4 heightSample(uint2 p) {
    Sample s=samples[p.y*(params.dimensions.x+1)+p.x];
    return draw.options.x!=0?s.eroded:s.base;
}
float4 interpolateSample(float2 p) {
    uint2 a=min(uint2(p),params.dimensions.x-1);
    float2 f=p-float2(a);
    return lerp(lerp(heightSample(a),heightSample(a+uint2(1,0)),f.x),
                lerp(heightSample(a+uint2(0,1)),heightSample(a+uint2(1,1)),f.x),f.y);
}
#ifdef TERRAIN_SHADOW
float4 main(uint id:SV_VertexID,uint instance:SV_InstanceID) : SV_Position {
#else
TerrainSurface main(uint id:SV_VertexID,uint instance:SV_InstanceID) {
#endif
#ifdef TERRAIN_SHADOW
    draw=drawRecords[shadowRecord(instance)].data;
#else
    draw=drawPush; if((drawPush.options.w&0x80000000u)!=0)draw=drawRecords[instance].data;
#endif
    bool wire=(draw.options.w&2)!=0;
    uint vertex=wire?topology[draw.indices.x+id]:id;
    uint width=(draw.patch.w!=0?draw.patch.w:params.dimensions.x)+1;
    uint2 local=uint2(vertex%width,vertex/width);
    float2 grid=float2(draw.patch.xy+local*draw.patch.z);
    float4 s=heightSample(uint2(grid));
    float3 position=float3(grid.x*params.shape.x,s.w,grid.y*params.shape.x)+draw.offset.xyz;
    // A shared coarse-grid anchor keeps each collapsing group coherent,
    // including the final triangles approaching zero area.
    uint2 anchor=uint2(grid)-(local&1)*draw.patch.z;
    float3 reference=float3(anchor.x*params.shape.x,heightSample(anchor).w,anchor.y*params.shape.x)+draw.offset.xyz;
    float m=draw.morph.w!=0?saturate((distance(reference,view.observer.xyz)-draw.morph.x)/(draw.morph.y-draw.morph.x)):0;
    grid-=float2(local&1)*float(draw.patch.z)*m;
    s=interpolateSample(grid);
    position=float3(grid.x*params.shape.x,s.w,grid.y*params.shape.x)+draw.offset.xyz;
#ifdef TERRAIN_SHADOW
    return shadowPosition(position);
#else
    TerrainSurface o;
    o.relativePosition=position;
    o.position=mul(view.viewProjection,float4(position,1));
    o.gridPosition=grid;
    o.normal=s.xyz;
    o.options=draw.options;
    o.normalInfo=float4(float(draw.normals.z)/float(draw.patch.z),draw.normals.x,draw.normals.y,draw.morph.z);
    // Collapsed vertices may have different original distances. Evaluate the
    // shading blend at their final shared position to avoid a lighting split.
    o.morph=draw.morph.w!=0?saturate((distance(position,view.observer.xyz)-draw.morph.x)/(draw.morph.y-draw.morph.x)):0;
    o.barycentric=float3(id%3==0?1:0,id%3==1?1:0,id%3==2?1:0);
    return o;
#endif
}

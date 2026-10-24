#include "temporal_surface.hlsli"
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
#include "terrain_morph.hlsli"
float3 fixedPosition(uint2 grid) {
    return float3(grid.x*params.shape.x,heightSample(grid).w,grid.y*params.shape.x)+draw.offset.xyz;
}
float3 fixedSurface(uint vertex,float3 observer,float4 curvature,out float2 grid,out float3 rendered,out float morph) {
    uint width=(draw.patch.w!=0?draw.patch.w:params.dimensions.x)+1;
    grid=float2(draw.patch.xy+uint2(vertex%width,vertex/width)*draw.patch.z);
    float3 fine=fixedPosition(uint2(grid));
    morph=draw.morph.w!=0?saturate((distance(fine,observer)-draw.morph.x)/(draw.morph.y-draw.morph.x)):0;
    rendered=curvePosition(fine,curvature);
    if(morph==0)return fine;
    uint2 anchor=uint2(grid)/(draw.patch.z*2)*(draw.patch.z*2);
    if(morph==1 && distance(fixedPosition(anchor),observer)>=draw.morph.y) {
        grid=float2(anchor);
        float3 endpoint=fixedPosition(uint2(grid));rendered=curvePosition(endpoint,curvature);return endpoint;
    }
    uint2 a,b,c;float3 w;
    terrainParentTriangle(uint2(grid),draw.patch.z*2,params.dimensions.x,a,b,c,w);
    float3 pa=fixedPosition(a),pb=fixedPosition(b),pc=fixedPosition(c);
    float3 flat=terrainEndpointBlend(fine,terrainTrianglePoint(pa,pb,pc,w),morph);flat.xz=fine.xz;
    rendered=curvature.z!=0?terrainEndpointBlend(rendered,terrainTrianglePoint(curvePosition(pa,curvature),curvePosition(pb,curvature),curvePosition(pc,curvature),w),morph):flat;
    return flat;
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
    float2 grid;float3 rendered;float morph;
    float3 position=fixedSurface(vertex,view.observer.xyz,temporal.curvature,grid,rendered,morph);
    float4 s=heightSample(uint2(grid));
#ifdef TERRAIN_SHADOW
    return shadowPosition(rendered);
#else
    TerrainSurface o;
    o.relativePosition=position;
    o.renderedPosition=rendered;
    o.position=mul(view.viewProjection,float4(o.renderedPosition,1));
    o.gridPosition=grid;
    o.normal=s.xyz;
    o.options=draw.options;
    o.normalInfo=float4(float(draw.normals.z)/float(draw.patch.z),draw.normals.x,draw.normals.y,draw.morph.z);
    o.morph=morph;
    o.barycentric=float3(id%3==0?1:0,id%3==1?1:0,id%3==2?1:0);
    o.previousClip=0;
    if(temporal.state.x!=0 && instance<uint(temporal.state.w)) {
        TemporalTerrainRecord old=temporalTerrain[instance];
        if(old.valid.x>0) {
            Draw current=draw;
            draw.offset=asfloat(old.draw[0]);draw.options=old.draw[1];draw.patch=old.draw[2];
            draw.morph=asfloat(old.draw[3]);draw.normals=old.draw[4];draw.indices=old.draw[5];
            float2 unused;float3 previous;float previousMorph;
            fixedSurface(vertex,old.observer.xyz,temporal.previousCurvature,unused,previous,previousMorph);
            o.previousClip=mul(temporal.previous,float4(previous,1));
            draw=current;
        }
    }
    return o;
#endif
}

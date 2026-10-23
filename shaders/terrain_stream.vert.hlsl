#include "terrain_stream_common.hlsli"
#include "terrain_surface.hlsli"
float3 surfacePosition(uint vertex,float3 observer,out float2 grid) {
    uint width=draw.patch.w+1;
    uint2 local=uint2(vertex%width,vertex/width);
    grid=float2(draw.patch.xy+local*draw.patch.z);
    uint2 anchor=uint2(grid)-(local&1)*draw.patch.z;
    float3 reference=renderPosition(float2(anchor),terrainHeight(anchor));
    float dist=draw.tile.y!=0?distance(reference.xz,observer.xz):distance(reference,observer);
    precise float m=draw.morph.w!=0?saturate((dist-draw.morph.x)/(draw.morph.y-draw.morph.x)):0;
    if(draw.parent.x!=0xffffffff && draw.patch.z==1) m=max(m,1-heightAvailability(float2(anchor)));
    grid-=float2(local&1)*float(draw.patch.z)*m;
    return renderPosition(grid,interpolateHeight(grid,m));
}
#ifdef TERRAIN_SHADOW
#include "shadow_terrain_surface.hlsli"
ShadowTerrainSurface main(uint id:SV_VertexID, uint instance:SV_InstanceID) {
#else
TerrainSurface main(uint id:SV_VertexID, uint instance:SV_InstanceID) {
#endif
    loadDraw(instance);
    uint vertex=(draw.options.w&2)!=0?topology[draw.indices.x+id]:id;
    float2 grid;float3 position=surfacePosition(vertex,view.observer.xyz,grid);
    float dist;
#ifdef TERRAIN_SHADOW
    ShadowTerrainSurface o;o.position=shadowPosition(position);o.gridPosition=grid;o.recordID=shadowRecord(instance);
    return o;
#else
    TerrainSurface o;o.recordID=instance;o.relativePosition=position;
    o.position=mul(view.viewProjection,float4(position,1));
    o.gridPosition=grid; o.options=draw.options;
    o.normalInfo=float4(0,0,0,draw.morph.z+draw.tile.z);
    dist=draw.tile.y!=0?distance(position.xz,view.observer.xz):distance(position,view.observer.xyz);
    o.morph=draw.morph.w!=0?saturate((dist-draw.morph.x)/(draw.morph.y-draw.morph.x)):0;
    o.normal=draw.options.y==0?sampledNormal(grid,o.morph):float3(0,1,0);
    o.barycentric=float3(id%3==0?1:0,id%3==1?1:0,id%3==2?1:0);
    o.previousClip=0;
    #ifndef TERRAIN_VERTEX_PROBE
    if(temporal.state.x!=0 && instance<uint(temporal.state.w)) {
        previousGeometry=temporalTerrain[instance];
        if(previousGeometry.valid.x>0) {
            Draw current=draw;previousGeometryActive=true;
            draw.offset=asfloat(previousGeometry.draw[0]);draw.options=previousGeometry.draw[1];draw.patch=previousGeometry.draw[2];
            draw.morph=asfloat(previousGeometry.draw[3]);draw.normals=previousGeometry.draw[4];draw.indices=previousGeometry.draw[5];
            draw.parent=previousGeometry.draw[6];draw.tile=asfloat(previousGeometry.draw[7]);
            float2 unused;float3 old=surfacePosition(vertex,previousGeometry.observer.xyz,unused);
            o.previousClip=mul(temporal.previous,float4(old,1));
            draw=current;previousGeometryActive=false;
        }
    }
    #endif
    return o;
#endif
}

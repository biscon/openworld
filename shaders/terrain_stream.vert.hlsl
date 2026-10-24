#include "terrain_stream_common.hlsli"
#include "terrain_surface.hlsli"
#include "terrain_morph.hlsli"
float parentHeight(uint2 p,uint level) {
    // Legacy flat streaming stores only level zero; hierarchical tiles have
    // independently filtered geometry levels.
    if(draw.tile.y==0)return terrainHeight(p);
    return levelHeight(p>>level,level);
}
float3 surfacePosition(uint vertex,float3 observer,float4 curvature,out float2 grid,out float3 rendered,out float morph) {
    uint width=draw.patch.w+1;
    grid=float2(draw.patch.xy+uint2(vertex%width,vertex/width)*draw.patch.z);
    if((draw.options.w&128)!=0) {
        // A stationary mesh: only explicit shared boundary contracts select
        // another height representation. The observer never moves a vertex.
        uint2 local=uint2(vertex%width,vertex/width);
        uint edges=(draw.options.w>>8)&15,corners=(draw.options.w>>12)&15;
        bool coarse=(local.x==0 && (edges&1)!=0) || (local.x==draw.patch.w && (edges&2)!=0) ||
                    (local.y==0 && (edges&4)!=0) || (local.y==draw.patch.w && (edges&8)!=0);
        if((local.x==0 || local.x==draw.patch.w) && (local.y==0 || local.y==draw.patch.w))
            coarse=coarse || (corners&(1u<<((local.x!=0?1:0)+(local.y!=0?2:0))))!=0;
        float h=terrainHeight(uint2(grid));
        if(coarse) {
            uint level=uint(draw.morph.z)+1;uint2 a,b,c;float3 w;
            terrainParentTriangle(uint2(grid),1u<<level,draw.options.z,a,b,c,w);
            h=terrainTrianglePoint(parentHeight(a,level).xxx,parentHeight(b,level).xxx,parentHeight(c,level).xxx,w).x;
        }
        float3 flat=renderPosition(grid,h);rendered=curvePosition(flat,curvature);morph=0;return flat;
    }
    float3 fine=renderPosition(grid,terrainHeight(uint2(grid)));
    float dist=draw.tile.y!=0?distance(fine.xz,observer.xz):distance(fine,observer);
    morph=draw.morph.w!=0?saturate((dist-draw.morph.x)/(draw.morph.y-draw.morph.x)):0;
    if(draw.parent.x!=0xffffffff && draw.patch.z==((draw.options.w&64)!=0?2:1))
        morph=max(morph,1-heightAvailability(grid));
    rendered=curvePosition(fine,curvature);
    if(morph==0)return fine;
    uint level=uint(draw.morph.z)+1;
    // At the exact endpoint coalesce redundant vertices. This changes no
    // parent surface, but avoids raster subpixel cracks at T-junctions.
    uint2 anchor=(uint2(grid)>>level)<<level;
    float3 anchorPosition=renderPosition(float2(anchor),terrainHeight(anchor));
    float anchorDistance=draw.tile.y!=0?distance(anchorPosition.xz,observer.xz):distance(anchorPosition,observer);
    bool endpoint=draw.morph.w!=0 && anchorDistance>=draw.morph.y;
    if(draw.parent.x!=0xffffffff && draw.patch.z==((draw.options.w&64)!=0?2:1))
        endpoint=endpoint || heightAvailability(float2(anchor))==0;
    if(morph==1 && endpoint) {
        grid=float2(anchor);
        float3 endpoint=renderPosition(grid,parentHeight(uint2(grid),level));
        rendered=curvePosition(endpoint,curvature);return endpoint;
    }
    uint2 a,b,c;float3 w;
    terrainParentTriangle(uint2(grid),1u<<level,draw.options.z,a,b,c,w);
    float3 pa=renderPosition(float2(a),parentHeight(a,level));
    float3 pb=renderPosition(float2(b),parentHeight(b,level));
    float3 pc=renderPosition(float2(c),parentHeight(c,level));
    float3 target=terrainTrianglePoint(pa,pb,pc,w);
    float3 flat=terrainEndpointBlend(fine,target,morph);flat.xz=fine.xz;
    if(curvature.z!=0)
        rendered=terrainEndpointBlend(rendered,terrainTrianglePoint(curvePosition(pa,curvature),curvePosition(pb,curvature),curvePosition(pc,curvature),w),morph);
    else rendered=flat;
    return flat;
}
#ifdef TERRAIN_SHADOW
#include "shadow_terrain_surface.hlsli"
ShadowTerrainSurface main(uint id:SV_VertexID, uint instance:SV_InstanceID) {
#else
TerrainSurface main(uint id:SV_VertexID, uint instance:SV_InstanceID) {
#endif
    loadDraw(instance);
    uint vertex=(draw.options.w&2)!=0?topology[draw.indices.x+id]:id;
    float2 grid;float3 rendered;float morph;
    float3 position=surfacePosition(vertex,view.observer.xyz,view.curvature,grid,rendered,morph);
#ifdef TERRAIN_SHADOW
    ShadowTerrainSurface o;o.position=shadowPosition(rendered);o.gridPosition=grid;o.recordID=shadowRecord(instance);
    return o;
#else
    TerrainSurface o;o.recordID=instance;o.relativePosition=position;
    o.renderedPosition=rendered;
    o.position=mul(view.viewProjection,float4(o.renderedPosition,1));
    o.gridPosition=grid; o.options=draw.options;
    o.normalInfo=float4(0,0,0,draw.morph.z+draw.tile.z);
    o.morph=morph;
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
            float2 unused;float3 old;float oldMorph;
            surfacePosition(vertex,previousGeometry.observer.xyz,temporal.previousCurvature,unused,old,oldMorph);
            o.previousClip=mul(temporal.previous,float4(old,1));
            draw=current;previousGeometryActive=false;
        }
    }
    #endif
    return o;
#endif
}

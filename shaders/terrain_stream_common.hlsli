#ifndef OW_STREAM_COMMON
#define OW_STREAM_COMMON
#include "temporal_surface.hlsli"
static bool previousGeometryActive=false;
static TemporalTerrainRecord previousGeometry;
#define TERRAIN_RECORD_ID
struct View { column_major float4x4 viewProjection; float4 observer; float4 grid; float4 camera; float4 clip; float4 curvature; float4 shadowClip; };
[[vk::binding(0,0)]] ConstantBuffer<View> view;
[[vk::binding(1,0)]] StructuredBuffer<float2> heights;
struct Weights { float4 rows[4]; float4 heights[4]; float4 parentRows[4]; };
[[vk::binding(2,0)]] ConstantBuffer<Weights> weights;
[[vk::binding(3,0)]] Texture2DArray<float4> coarseNormals;
[[vk::binding(4,0)]] Texture2DArray<float4> fineNormals;
[[vk::binding(5,0)]] Texture2DArray<float4> nextNormals;
[[vk::binding(6,0)]] SamplerState normalSampler;
[[vk::binding(7,0)]] StructuredBuffer<uint> topology;
#ifdef TERRAIN_VERTEX_PROBE
[[vk::binding(8,0)]] RWStructuredBuffer<float4> vertexOutput;
#endif
[[vk::binding(9,0)]] Texture2DArray<float4> parentFineNormals;
[[vk::binding(10,0)]] Texture2DArray<float4> parentNextNormals;
[[vk::binding(11,0)]] Texture2DArray<float4> thirdNormals;
[[vk::binding(12,0)]] Texture2DArray<float4> parentThirdNormals;
struct Draw { float4 offset; uint4 options; uint4 patch; float4 morph; uint4 normals; uint4 indices; uint4 parent; float4 tile; };
#if defined(TERRAIN_SHADOW) || defined(TERRAIN_SHADOW_FRAGMENT)
[[vk::binding(0,3)]] StructuredBuffer<Draw> drawRecords;
static Draw draw;
#ifdef TERRAIN_SHADOW
#include "shadow_resources.hlsli"
void loadDraw(uint id) {draw=drawRecords[shadowRecord(id)];}
#else
void loadDraw(uint id) {draw=drawRecords[id];}
#endif
#elif defined(TERRAIN_VERTEX_PROBE)
[[vk::push_constant]] ConstantBuffer<Draw> draw;
void loadDraw(uint id) {}
#else
[[vk::push_constant]] ConstantBuffer<Draw> drawPush;
[[vk::binding(0,3)]] StructuredBuffer<Draw> drawRecords;
static Draw draw;
void loadDraw(uint id) { draw=drawPush; if((drawPush.options.w&0x80000000u)!=0) draw=drawRecords[id]; }
#endif
float3 renderPosition(float2 grid,float height) {
    precise float3 local=float3(grid.x*draw.tile.x,height,grid.y*draw.tile.x);
    precise float3 world=local+draw.offset.xyz;
    precise float3 relative=world-(previousGeometryActive?previousGeometry.camera.xyz:view.camera.xyz);
    return relative;
}
float exactLerp(float a,float b,float f) {
    if(f==0) return a;
    if(f==1) return b;
    precise float value=a+(b-a)*f;
    return value;
}
uint heightOffset(uint level) {
    uint result=0;
    for(uint l=(draw.options.w&32)!=0?1:0;l<level;++l){uint w=(draw.options.z>>l)+1;result+=w*w;}
    return result;
}
float levelHeight(uint2 p,uint level) {
    uint width=(draw.options.z>>level)+1;
    float2 h=heights[heightOffset(level)+p.y*width+p.x];
    return draw.options.x!=0?h.y:h.x;
}
float terrainHeight(uint2 p) {
    uint level=draw.tile.y!=0?uint(draw.morph.z):0;
    return levelHeight(p>>level,level);
}
float2 weightCoordinate(float2 p) {
    float cells=draw.options.z, band=min(view.grid.w,cells*0.5);
    float2 q;
    [unroll] for(uint axis=0;axis<2;++axis) {
        float x=p[axis];
        q[axis]=x<band?smoothstep(0,band,x):x>cells-band?2+smoothstep(cells-band,cells,x):
            1+(x-band)/max(cells-2*band,1e-6);
    }
    return clamp(q,0.0,3.0);
}
float availability(float2 p) {
    float2 q=weightCoordinate(p); uint2 a=min(uint2(q),2); float2 f=q-float2(a);
    return exactLerp(exactLerp(weights.rows[a.y][a.x],weights.rows[a.y][a.x+1],f.x),
                exactLerp(weights.rows[a.y+1][a.x],weights.rows[a.y+1][a.x+1],f.x),f.y);
}

float heightAvailability(float2 p) {
    float2 q=weightCoordinate(p); uint2 a=min(uint2(q),2); float2 f=q-float2(a);
    return exactLerp(exactLerp((previousGeometryActive?previousGeometry.weights[4+a.y][a.x]:weights.heights[a.y][a.x]),(previousGeometryActive?previousGeometry.weights[4+a.y][a.x+1]:weights.heights[a.y][a.x+1]),f.x),
                exactLerp((previousGeometryActive?previousGeometry.weights[5+a.y][a.x]:weights.heights[a.y+1][a.x]),(previousGeometryActive?previousGeometry.weights[5+a.y][a.x+1]:weights.heights[a.y+1][a.x+1]),f.x),f.y);
}
float3 parentNormal(float2 grid,float morph) {
    uint width,height,layers; coarseNormals.GetDimensions(width,height,layers);
    float2 p=grid*.5+float2(draw.parent.yz);
    float2 uv=(p*view.grid.z/float(1u<<draw.indices.w)+.5)/float2(width,height);
    float3 coarse=coarseNormals.SampleLevel(normalSampler,float3(uv,draw.parent.x*2+draw.options.x),0).xyz;
    if(draw.indices.z==0xffffffff || draw.indices.w==0) return coarse;
    float2 q=weightCoordinate(p); uint2 a=min(uint2(q),2); float2 f=q-float2(a);
    float weight=lerp(lerp(weights.parentRows[a.y][a.x],weights.parentRows[a.y][a.x+1],f.x),
                      lerp(weights.parentRows[a.y+1][a.x],weights.parentRows[a.y+1][a.x+1],f.x),f.y);
    uint level=uint(max(0,draw.morph.z-1));
    uint halo=(1u<<(draw.indices.w-level))-1;
    parentFineNormals.GetDimensions(width,height,layers);
    uv=(p*view.grid.z/float(1u<<level)+halo+.5)/float2(width,height);
    float3 fine=parentFineNormals.SampleLevel(normalSampler,float3(uv,draw.indices.z*2+draw.options.x),0).xyz;
    if(draw.morph.z>0 && morph>0) {
        parentNextNormals.GetDimensions(width,height,layers);
        halo=(1u<<(draw.indices.w-level-1))-1;
        uv=(p*view.grid.z/float(2u<<level)+halo+.5)/float2(width,height);
        uint slot=level+1==draw.indices.w?draw.parent.x:draw.indices.z;
        fine=lerp(fine,parentNextNormals.SampleLevel(normalSampler,float3(uv,slot*2+draw.options.x),0).xyz,morph);
    }
    return lerp(coarse,fine,weight);
}
float3 ownNormal(float2 grid,float morph) {
    uint width,height,layers; coarseNormals.GetDimensions(width,height,layers);
    float2 uv=(grid*view.grid.z/float(1u<<draw.indices.w)+0.5)/float2(width,height);
    float3 coarse=coarseNormals.SampleLevel(normalSampler,float3(uv,draw.indices.y*2+draw.options.x),0).xyz;
    float weight=availability(grid);
    if(draw.normals.w==0xffffffff || draw.morph.z>=float(draw.indices.w) || weight<=0) return coarse;
    fineNormals.GetDimensions(width,height,layers);
    uv=(grid*view.grid.z/float(draw.patch.z)+draw.normals.x+0.5)/float2(width,height);
    float3 fine=fineNormals.SampleLevel(normalSampler,float3(uv,draw.normals.w*2+draw.options.x),0).xyz;
    if(morph>0) {
        nextNormals.GetDimensions(width,height,layers);
        uv=(grid*view.grid.z/float(draw.patch.z*2)+draw.normals.y+0.5)/float2(width,height);
        uint slot=uint(draw.morph.z)+1==draw.indices.w?draw.indices.y:draw.normals.w;
        fine=lerp(fine,nextNormals.SampleLevel(normalSampler,float3(uv,slot*2+draw.options.x),0).xyz,morph);
    }
    return lerp(coarse,fine,weight);
}
float normalField(float distance) {
    float level=max(0,floor(log2(max(distance/64,1e-6)))+1);
    return level+saturate((distance/(64*exp2(level))-.75)*4);
}
float3 stationaryNormalLevel(float2 grid,uint relative,uint base,uint detailSlot,uint coverageSlot,bool parent) {
    uint maximum=draw.indices.w;
    uint width,height,layers;
    uint halo=(1u<<(maximum-relative))-1;
    uint slot=relative==maximum?coverageSlot:detailSlot;
    float2 texel=grid*view.grid.z/float(1u<<relative)+halo+.5;
    uint choice=relative-base;
    if(parent) {
        if(choice==0) {parentFineNormals.GetDimensions(width,height,layers);return parentFineNormals.SampleLevel(normalSampler,float3(texel/float2(width,height),slot*2+draw.options.x),0).xyz;}
        if(choice==1) {parentNextNormals.GetDimensions(width,height,layers);return parentNextNormals.SampleLevel(normalSampler,float3(texel/float2(width,height),slot*2+draw.options.x),0).xyz;}
        parentThirdNormals.GetDimensions(width,height,layers);return parentThirdNormals.SampleLevel(normalSampler,float3(texel/float2(width,height),slot*2+draw.options.x),0).xyz;
    }
    if(choice==0) {fineNormals.GetDimensions(width,height,layers);return fineNormals.SampleLevel(normalSampler,float3(texel/float2(width,height),slot*2+draw.options.x),0).xyz;}
    if(choice==1) {nextNormals.GetDimensions(width,height,layers);return nextNormals.SampleLevel(normalSampler,float3(texel/float2(width,height),slot*2+draw.options.x),0).xyz;}
    thirdNormals.GetDimensions(width,height,layers);return thirdNormals.SampleLevel(normalSampler,float3(texel/float2(width,height),slot*2+draw.options.x),0).xyz;
}
float3 stationaryNormalAt(float2 grid,float absolute,bool parent) {
    uint base=parent?max(1u,draw.normals.z)-1:draw.normals.z;
    uint coverageSlot=parent?draw.parent.x:draw.indices.y;
    uint detailSlot=parent?draw.indices.z:draw.normals.w;
    uint width,height,layers;coarseNormals.GetDimensions(width,height,layers);
    float2 uv=(grid*view.grid.z/float(1u<<draw.indices.w)+.5)/float2(width,height);
    float3 coarse=coarseNormals.SampleLevel(normalSampler,float3(uv,coverageSlot*2+draw.options.x),0).xyz;
    if(detailSlot==0xffffffff || base>=draw.indices.w)return coarse;
    float weight=availability(grid);
    if(parent) {
        float2 q=weightCoordinate(grid);uint2 a=min(uint2(q),2);float2 f=q-a;
        weight=lerp(lerp(weights.parentRows[a.y][a.x],weights.parentRows[a.y][a.x+1],f.x),lerp(weights.parentRows[a.y+1][a.x],weights.parentRows[a.y+1][a.x+1],f.x),f.y);
    }
    float level=clamp(absolute-draw.tile.z-(parent?1:0),float(base),float(min(base+2,draw.indices.w)));
    uint lo=uint(level),hi=min(lo+1,draw.indices.w);
    float3 fine=stationaryNormalLevel(grid,lo,base,detailSlot,coverageSlot,parent);
    if(frac(level)>0)fine=lerp(fine,stationaryNormalLevel(grid,hi,base,detailSlot,coverageSlot,parent),frac(level));
    return lerp(coarse,fine,weight);
}
float3 sampledNormal(float2 grid,float morph) {
    if((draw.options.w&128)!=0) {
        float absolute=normalField(distance(renderPosition(grid,0).xz,view.observer.xz));
        float3 n=stationaryNormalAt(grid,absolute,false);
        if(draw.parent.x!=0xffffffff) {
            float weight=heightAvailability(grid);
            if(weight<1)n=lerp(stationaryNormalAt(grid*.5+float2(draw.parent.yz),absolute,true),n,weight);
        }
        return normalize(n);
    }
    float3 n=ownNormal(grid,morph);
    if(draw.parent.x!=0xffffffff) {
        float weight=heightAvailability(grid);
        if(weight<1) n=lerp(parentNormal(grid,morph),n,weight);
    }
    return normalize(n);
}
#endif

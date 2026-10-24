#include "terrain_stationary.hlsli"
struct Plant {float4 rootScale;float yaw;float variation;uint identity;uint assetCell;};
struct Patch {float4 rectangle;uint4 geometry,heights,topology;};
struct Job {
    uint4 key,config,canonical; // config: slot,seed,generate,patch count
    float4 canonicalGrid,habitat,elevation,clearing;
    uint4 assets;
    Patch patches[16];
};
[[vk::binding(0,0)]] RWStructuredBuffer<Plant> population;
[[vk::binding(1,0)]] RWStructuredBuffer<float4> attachment;
[[vk::binding(2,0)]] StructuredBuffer<Job> jobs;
[[vk::binding(3,0)]] StructuredBuffer<float2> heights;
uint hash(uint x){x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);}
float random(uint x){return (hash(x)>>8)*(1.0/16777216.0);}
float heightAt(uint offset,uint width,uint2 p,uint eroded){float2 h=heights[offset+p.y*width+p.x];return eroded!=0?h.y:h.x;}
float3 vertex(Patch p,uint2 local) {
    local=terrainStitchedVertex(local,p.geometry.w,p.topology.x);
    uint2 grid=p.geometry.xy+local*p.geometry.z;
    float h=heightAt(p.heights.x,p.heights.z,grid>>p.topology.z,p.topology.w);
    if(terrainStationaryCoarse(local,p.geometry.w,p.topology.x,p.topology.y)) {
        uint2 a,b,c;float3 w;uint level=p.topology.z+1;
        terrainParentTriangle(grid,1u<<level,(p.heights.z-1)<<p.topology.z,a,b,c,w);
        h=terrainTrianglePoint(heightAt(p.heights.y,p.heights.w,a>>level,p.topology.w).xxx,
                              heightAt(p.heights.y,p.heights.w,b>>level,p.topology.w).xxx,
                              heightAt(p.heights.y,p.heights.w,c>>level,p.topology.w).xxx,w).x;
    }
    float2 xz=p.rectangle.xy+float2(grid)*p.rectangle.z;
    return float3(xz.x,h,xz.y);
}
bool sampleTriangle(float2 queryPosition,float3 a,float3 b,float3 c,out float height,out float2 slope) {
    float2 e=b.xz-a.xz,f=c.xz-a.xz,q=queryPosition-a.xz;
    float det=e.x*f.y-e.y*f.x;
    height=0;slope=0;
    if(abs(det)<1e-10)return false;
    float u=(q.x*f.y-q.y*f.x)/det,v=(e.x*q.y-e.y*q.x)/det;
    if(u<-.00001||v<-.00001||u+v>1.00001)return false;
    height=a.y+u*(b.y-a.y)+v*(c.y-a.y);
    float3 n=cross(b-a,c-a);slope=-n.xz/n.y;return true;
}
bool surface(Job job,float2 queryPosition,out float height,out float2 slope) {
    height=0;slope=0;
    for(uint i=0;i<job.config.w;++i) {
        Patch p=job.patches[i];
        float2 q=((queryPosition-p.rectangle.xy)/p.rectangle.z-float2(p.geometry.xy))/p.geometry.z;
        if(any(q<-.0001)||any(q>float(p.geometry.w)+.0001))continue;
        int2 base=clamp(int2(floor(q)),0,int(p.geometry.w)-1);
        // A stitched edge sampleTriangle can span its adjacent collapsed quad.
        for(int z=max(0,base.y-1);z<=min(int(p.geometry.w)-1,base.y+1);++z)
            for(int x=max(0,base.x-1);x<=min(int(p.geometry.w)-1,base.x+1);++x) {
                float3 a=vertex(p,uint2(x,z)),b=vertex(p,uint2(x+1,z));
                float3 c=vertex(p,uint2(x,z+1)),d=vertex(p,uint2(x+1,z+1));
                if(sampleTriangle(queryPosition,a,c,d,height,slope)||sampleTriangle(queryPosition,a,d,b,height,slope))return true;
            }
    }
    return false;
}
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    Job job=jobs[group.x];uint index=job.config.x*128+lane;
    Plant p;
    if(job.config.z!=0) {
        uint species=lane<126?0:lane-125,candidate=lane<126?lane:0;
        uint h=hash(job.config.y^0x56330001u);
        h=hash(h^job.key.x);h=hash(h^job.key.y);h=hash(h^job.key.z);h=hash(h^job.key.w);
        h=hash(h^species);h=hash(h^candidate);
        p.identity=h;p.rootScale=float4(4*random(h+1),0,4*random(h+2),species==0?.8+.4*random(h+3):1);
        p.yaw=random(h+4)*6.2831853;p.variation=random(h+5);
        uint asset=species==0?(random(h+6)<.5?job.assets.x:job.assets.y):species==1?job.assets.z:job.assets.w;
        p.assetCell=job.config.x*8+(asset&7);
        float probability=dot(job.habitat.xyz,species==0?float3(1,.65,.35):species==1?float3(.15,.25,1):float3(.15,.75,.4));
        float2 grid=(p.rootScale.xz-job.canonicalGrid.xy)/job.canonicalGrid.z;
        uint2 a=min(uint2(grid),job.canonical.y-2);float2 f=grid-a;
        uint2 b=f.x>=f.y?a+uint2(1,0):a+1,c=f.x>=f.y?a+1:a+uint2(0,1);
        float3 v0=float3(a.x*job.canonicalGrid.z,heightAt(job.canonical.x,job.canonical.y,a,job.canonical.z),a.y*job.canonicalGrid.z);
        float3 v1=float3(b.x*job.canonicalGrid.z,heightAt(job.canonical.x,job.canonical.y,b,job.canonical.z),b.y*job.canonicalGrid.z);
        float3 v2=float3(c.x*job.canonicalGrid.z,heightAt(job.canonical.x,job.canonical.y,c,job.canonical.z),c.y*job.canonicalGrid.z);
        float height;float2 slope;bool valid=sampleTriangle(grid*job.canonicalGrid.z,v0,v1,v2,height,slope);
        float up=rsqrt(1+dot(slope,slope));
        if(!valid||asset==0xffffffff||random(h+7)>probability||up<job.habitat.w||height<job.elevation.x||height>job.elevation.y||
           (job.clearing.z>0&&distance(p.rootScale.xz,job.clearing.xy)<job.clearing.z))p.rootScale.w=0;
        population[index]=p;
    } else p=population[index];
    float height;float2 slope;
    bool valid=p.rootScale.w>0&&surface(job,p.rootScale.xz,height,slope);
    valid=valid&&rsqrt(1+dot(slope,slope))>=job.habitat.w;
    if(valid) {
        uint species=lane<126?0:lane-125;
        float footprint=(species==0?.25:species==1?.45:.6)*p.rootScale.w;
        float tolerance=species==2?.05:.02;
        for(uint i=0;i<4;++i) {
            float2 delta=(i==0?float2(1,0):i==1?float2(-1,0):i==2?float2(0,1):float2(0,-1))*footprint;
            float testHeight;float2 unused;
            if(!surface(job,p.rootScale.xz+delta,testHeight,unused)||abs(testHeight-height-dot(slope,delta))>tolerance)valid=false;
        }
    }
    attachment[index]=valid?float4(height,slope,1):0;
}

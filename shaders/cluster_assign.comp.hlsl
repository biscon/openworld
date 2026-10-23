#define OW_CLUSTER_BUILD
#include "clusters.hlsli"
[numthreads(64,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    LocalLightRecord light=localLights[group.x];
    float3 p=mul(clusters.view,float4(light.positionRange.xyz,1)).xyz;
    float radius=light.positionRange.w;
    // Inflate assignment bounds slightly; shading still applies the exact range.
    radius+=max(.001,(max(max(abs(p.x),abs(p.y)),abs(p.z))+radius)*2e-6);
    if(p.z+radius<clusters.depth.x || p.z-radius>clusters.depth.y)return;
    uint z0=sliceForDepth(p.z-radius),z1=sliceForDepth(p.z+radius);
    uint2 lo=0,hi=clusters.grid.xy-1;
    if(p.z-radius>clusters.depth.x) {
        float2 low=1e30,high=-1e30;
        [unroll]for(uint z=0;z<2;++z) [unroll]for(uint y=0;y<2;++y) [unroll]for(uint x=0;x<2;++x){
            float3 q=p+float3(x?radius:-radius,y?radius:-radius,z?radius:-radius);
            float2 ndc=float2(q.x/clusters.projection.z,-q.y/clusters.projection.w)/q.z;
            float2 pixel=(ndc*.5+.5)*clusters.projection.xy;
            low=min(low,pixel);high=max(high,pixel);
        }
        if(any(high<0)||any(low>clusters.projection.xy))return;
        lo=uint2(clamp(floor(low/clusters.grid.w),0,float2(clusters.grid.xy-1)));
        hi=uint2(clamp(floor(high/clusters.grid.w),0,float2(clusters.grid.xy-1)));
    }
    uint nx=hi.x-lo.x+1,ny=hi.y-lo.y+1,total=nx*ny*(z1-z0+1);
    for(uint n=lane;n<total;n+=64){
        uint x=lo.x+n%nx,y=lo.y+(n/nx)%ny,z=z0+n/(nx*ny);
        uint id=(z*clusters.grid.y+y)*clusters.grid.x+x;
        ClusterBounds b=clusterBounds[id];float3 delta=p-clamp(p,b.low.xyz,b.high.xyz);
        if(dot(delta,delta)>radius*radius)continue;
        uint offset;InterlockedAdd(clusterCounts[id],1,offset);
        if(offset<clusters.options.y)clusterIndices[id*clusters.options.y+offset]=group.x;
    }
}

struct Candidate { float4 lo; float4 hi; uint4 geometry; };
struct Command { uint indexCount, instanceCount, firstIndex; int vertexOffset; uint firstInstance; };
[[vk::binding(1,0)]] StructuredBuffer<Candidate> candidates;
[[vk::binding(2,0)]] StructuredBuffer<uint4> batches;
[[vk::binding(3,0)]] RWStructuredBuffer<Command> commands;
[[vk::binding(4,0)]] RWStructuredBuffer<uint4> counts;
#ifndef VISIBILITY_NO_HIZ
[[vk::binding(5,0)]] Texture2D<float> pyramid;
#endif
struct Params { column_major float4x4 vp; uint4 viewport; uint4 options; };
[[vk::push_constant]] ConstantBuffer<Params> params;
groupshared uint scan[128];
groupshared uint reasons[128];
groupshared uint2 triangles[128];
void addTriangles(inout uint2 total,uint n) {uint old=total.x;total.x+=n;total.y+=total.x<old?1:0;}
uint classify(Candidate c) {
    if(c.lo.w == 0) return 0;
    float4 corners[8];
    bool outside[6] = {true,true,true,true,true,true};
    float2 lo=1e30, hi=-1e30; float nearest=0;
    bool uncertain=false;
    [unroll] for(uint i=0;i<8;++i) {
        float3 p=float3(i&1?c.hi.x:c.lo.x,i&2?c.hi.y:c.lo.y,i&4?c.hi.z:c.lo.z);
        float4 h=mul(params.vp,float4(p,1)); corners[i]=h;
        if(!all(isfinite(h))) return 0;
        float e=max(1e-5,9.5367431640625e-7*max(max(abs(h.x),abs(h.y)),max(abs(h.z),abs(h.w))));
        outside[0] &= h.x < -h.w-e; outside[1] &= h.x > h.w+e;
        outside[2] &= h.y < -h.w-e; outside[3] &= h.y > h.w+e;
        outside[4] &= h.z < -e; outside[5] &= h.z > h.w+e;
        uncertain |= h.w <= e || h.z >= h.w-e;
        if(h.w>0) {
            float3 ndc=h.xyz/h.w;
            lo=min(lo,ndc.xy);hi=max(hi,ndc.xy);nearest=max(nearest,ndc.z);
        }
    }
    if(params.viewport.w != 0) {
        [unroll] for(uint p=0;p<6;++p) if(outside[p]) return 1;
    }
#ifdef VISIBILITY_NO_HIZ
    return 0;
#else
    if(params.options.x == 0 || uncertain || (all(c.lo.xyz<=0)&&all(c.hi.xyz>=0))) return 0;
    if(any(hi < -1) || any(lo > 1)) return 0;
    lo=clamp(lo,-1.0,1.0);hi=clamp(hi,-1.0,1.0);
    float2 size=float2(params.viewport.xy);
    int2 a=int2(floor((lo*.5+.5)*size))-1;
    int2 b=int2(ceil((hi*.5+.5)*size))+1;
    a=clamp(a,0,int2(params.viewport.xy)-1);b=clamp(b,0,int2(params.viewport.xy)-1);
    uint level=0; uint2 ma=a,mb=b;
    [loop] while(any(mb-ma>1) && level+1<params.viewport.z) {
        ++level; uint2 extent=max(uint2(1,1),params.viewport.xy>>level);
        ma=min(uint2(a)>>level,extent-1);mb=min(uint2(b)>>level,extent-1);
    }
    float depth=1;
    [loop] for(uint y=ma.y;y<=mb.y;++y)
        [loop] for(uint x=ma.x;x<=mb.x;++x) depth=min(depth,pyramid.Load(int3(x,y,level)));
    float epsilon=max(1e-6,9.5367431640625e-7*max(abs(nearest),abs(depth)));
    return nearest < depth-epsilon ? 2 : 0;
#endif
}
[numthreads(128,1,1)]
void main(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint4 batch=batches[group.x+params.options.y];
    uint reason=0,visible=0;
    if(lane<batch.y) {
        reason=classify(candidates[batch.x+lane]);visible=reason==0?1:0;
        Command empty=(Command)0;commands[batch.x+lane]=empty;
    }
    scan[lane]=visible; reasons[lane]=reason;
    triangles[lane]=0;
    if(visible)triangles[lane]=uint2(candidates[batch.x+lane].geometry.x/3,candidates[batch.x+lane].geometry.w);
    AllMemoryBarrierWithGroupSync();
    [unroll] for(uint step=1;step<128;step<<=1) {
        uint add=lane>=step?scan[lane-step]:0;
        GroupMemoryBarrierWithGroupSync();scan[lane]+=add;GroupMemoryBarrierWithGroupSync();
    }
    if(visible) {
        Candidate c=candidates[batch.x+lane]; Command result;
        result.indexCount=c.geometry.x;result.instanceCount=1;result.firstIndex=c.geometry.y;
        result.vertexOffset=0;result.firstInstance=c.geometry.z;
        commands[batch.x+scan[lane]-1]=result;
    }
    if(lane==0) {
        uint frustum=0,occluded=0;
        for(uint i=0;i<batch.y;++i) {frustum+=reasons[i]==1?1:0;occluded+=reasons[i]==2?1:0;}
        uint at=(group.x+params.options.y)*3;
        counts[at]=uint4(scan[127],frustum,occluded,0);
        uint2 terrain=0,meshes=0;
        for(uint j=0;j<batch.y;++j) {
            if(triangles[j].y!=0)addTriangles(terrain,triangles[j].x);
            else addTriangles(meshes,triangles[j].x);
        }
        uint4 totals=uint4(terrain,meshes);
        if(params.options.x==0)counts[at+1]=totals;
        counts[at+2]=totals;
    }
}

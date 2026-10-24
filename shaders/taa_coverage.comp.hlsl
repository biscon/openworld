#include "post_common.hlsli"
float linearDepth(float d) {
    const float zn=.5,zf=temporal.originDelta.w;
    return d>0?(zn*zf/(zf-zn))/(d+zn/(zf-zn)):0;
}
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID) {
    uint2 p=tid.xy;if(any(p>=settings.dims.xy))return;
    int2 centre=int2(floor(float2(p)+temporal.pixels.zw+.5));
    float count=0,nearZ=1e20,farZ=0,oldNear=1e20,oldFar=0,nearest=0,reactive=0;
    float2 minMotion=1e20,maxMotion=-1e20;
    bool valid=true;float4 selected=0;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        int2 q=clamp(centre+int2(x,y),0,int2(settings.dims.xy)-1);
        float4 v=motion.Load(int3(q,0));float d=sourceDepth.Load(int3(q,0)).r;
        count+=temporalMotionThin(v.w)?1:0;
        valid=valid&&temporalMotionValid(v.w);
        float z=linearDepth(d);
        if(d>0) {
            nearZ=min(nearZ,z);farZ=max(farZ,z);
            float oldZ=v.z*(temporal.curvature.z!=0?1000:1);
            oldNear=min(oldNear,oldZ);oldFar=max(oldFar,oldZ);
        } else if(temporalMotionSky(v.w)) {
            // Sky is the background end of a canopy coverage interval, not an
            // invalid opaque depth. Keep sky reprojection tagged separately.
            farZ=temporal.originDelta.w;oldFar=temporal.originDelta.w;
        }
        if(d>nearest){nearest=d;selected=v;}
        minMotion=min(minMotion,v.xy);maxMotion=max(maxMotion,v.xy);
        reactive=max(reactive,reactivity.Load(int3(q,0)).r);
    }
    float sample=count/9,coverage=sample,confidence=0;
    float2 oldUv=(p+.5)/settings.dims.xy+selected.xy;
    bool partial=count>0&&count<9,historySupport=false;
    bool support=nearest>0&&valid&&reactive<.01&&
                 length((maxMotion-minMotion)*settings.dims.xy)<2;
    // An ordinary foreground surface must not inherit a foliage cluster behind it.
    int2 cp=clamp(centre,0,int2(settings.dims.xy)-1);
    float4 centreMotion=motion.Load(int3(cp,0));
    if(!temporalMotionThin(centreMotion.w)&&sourceDepth.Load(int3(cp,0)).r>=nearest&&count>0)
        support=false;
    if(support&&temporal.state.y!=0&&settings.pass.x!=0&&all(oldUv>0)&&all(oldUv<1)) {
        int2 hp=clamp(int2(oldUv*settings.dims.xy),0,int2(settings.dims.xy)-1);
        float4 old=coverageHistory.Load(int3(hp,0));
        float tolerance=max(.02,oldFar*.01);
        bool compatible=old.w>0&&oldNear<=old.w*1000+tolerance&&oldFar>=old.z*1000-tolerance;
        historySupport=compatible&&old.y>0&&old.x>0&&old.x<1;
        if(compatible&&(partial||historySupport)) {
            // Nine correlated coverage observations: tolerate jitter sampling noise,
            // but reset abrupt coverage changes instead of keeping stale relaxation.
            float limit=max(2./9,3*sqrt(max(old.x*(1-old.x),.01)/9));
            if(abs(sample-old.x)<=limit) {
                coverage=lerp(sample,old.x,.875);
                confidence=min(1,old.y+.125);
            }
        }
    }
    if(!support||(!partial&&!historySupport))confidence=0;
    coverageResult[p]=float4(coverage,confidence,nearest>0?nearZ*.001:0,farZ*.001);
}

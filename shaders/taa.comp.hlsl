#include "post_common.hlsli"
float3 ycocg(float3 c){return float3(dot(c,float3(.25,.5,.25)),(c.r-c.b)*.5,(-c.r+2*c.g-c.b)*.25);}
float3 rgb(float3 c){return float3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
float4 cubicHistory(float2 uv){
    float2 p=uv*settings.dims.xy-.5,base=floor(p),f=p-base;
    float2 f2=f*f,f3=f2*f;
    float2 w0=-.5*f+f2-.5*f3,w1=1-2.5*f2+1.5*f3,w2=.5*f+2*f2-1.5*f3,w3=-.5*f2+.5*f3;
    float2 w12=w1+w2,p12=base+w2/max(w12,1e-6)+.5;
    float2 px[3]={base-.5,p12,base+2.5};float2 weights[3]={w0,w12,w3};
    float4 sum=0;
    [unroll]for(uint y=0;y<3;++y)[unroll]for(uint x=0;x<3;++x)
        sum+=history.SampleLevel(linearClamp,float2(px[x].x,px[y].y)/settings.dims.xy,0)*weights[x].x*weights[y].y;
    return sum;
}
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID){
    uint2 p=tid.xy;if(any(p>=settings.dims.xy))return;
    float2 uv=(p+.5)/settings.dims.xy;
    float2 raster=float2(p)+temporal.pixels.zw;int2 centre=int2(floor(raster+.5));
    float3 current=0,mean=0,moment=0;float total=0,nearest=0,reactive=0;int2 chosen=clamp(centre,0,int2(settings.dims.xy)-1);
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
        int2 q=clamp(centre+int2(x,y),0,int2(settings.dims.xy)-1);
        float3 c=source.Load(int3(q,0)).rgb,v=ycocg(c);mean+=v;moment+=v*v;
        float2 distance=abs(float2(centre+int2(x,y))-raster);
        float w=max(0,1-distance.x/1.5)*max(0,1-distance.y/1.5);current+=c*w;total+=w;
        float d=sourceDepth.Load(int3(q,0)).r;if(d>nearest){nearest=d;chosen=q;}
        reactive=max(reactive,reactivity.Load(int3(q,0)).r);
    }
    current/=max(total,1e-6);mean/=9;float3 sigma=sqrt(max(moment/9-mean*mean,0));
    float4 velocity=motion.Load(int3(chosen,0));float2 oldUv=uv+velocity.xy;
    bool valid=temporal.state.y!=0&&velocity.w>0&&nearest>0&&all(oldUv>0)&&all(oldUv<1);
    float4 old=0;float oldZ=0;
    if(valid){
        int2 hp=clamp(int2(oldUv*settings.dims.xy),0,int2(settings.dims.xy)-1);
        oldZ=abs(historyDepth.Load(int3(hp,0)).r);
        valid=oldZ>0&&abs(oldZ-velocity.z)<=max(.02,velocity.z*.01);
        if(valid){old=cubicHistory(oldUv);old.rgb*=temporal.state.z;reactive=max(reactive,saturate(old.a));valid=all(isfinite(old));}
    }
    float3 clipped=rgb(clamp(ycocg(old.rgb),mean-1.25*sigma,mean+1.25*sigma));
    float speed=length(velocity.xy*settings.dims.xy);
    float disagreement=abs(luminance(current)-luminance(old.rgb))/max(max(luminance(current),luminance(old.rgb)),.01);
    float weight=valid?settings.effect.x*(1-saturate(reactive))*lerp(1,.5,saturate(speed/8))*(1-.5*saturate(disagreement)):0;
    // Invalid/background pixels never read uninitialized history or amplify NaNs.
    float3 colour=weight>0?lerp(current,clipped,weight):current;
    result[p]=float4(finiteHdr(colour),reactivity.Load(int3(chosen,0)).r);
    const float zn=.5,zf=64000;
    float z=nearest>0?(zn*zf/(zf-zn))/(nearest+zn/(zf-zn)):0;
    depthResult[p]=weight>0?z:-z;
}

#include "post_common.hlsli"
float3 extract(float3 c){
    float y=luminance(c),t=settings.effect.x,k=t*settings.effect.y;
    float soft=clamp(y-t+k,0,2*k);soft=soft*soft/max(4*k,1e-6);
    return c*(max(soft,y-t)/max(y,1e-6));
}
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID){
    uint2 p=tid.xy;if(any(p>=settings.dims.xy))return;
    float2 uv=(p+.5)/settings.dims.xy;
    float3 sum=0;float total=0;
    // Overlapping 3x3 footprint includes odd tails and clamps screen edges.
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
        float3 c=max(source.SampleLevel(linearClamp,uv+float2(x,y)/settings.dims.zw,0).rgb,0);
        float weight=(x==0?2:1)*(y==0?2:1);
        if(settings.pass.x==0){
            // Scene-referred weighting: exposure changes do not change the filter.
            weight/=1+luminance(c)/max(settings.effect.x,settings.effect.z*10000);
            c=extract(c);
        }
        sum+=c*weight;total+=weight;
    }
    result[p]=float4(finiteHdr(sum/max(total,1e-8)),1);
}

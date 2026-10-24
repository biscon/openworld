[[vk::binding(0,0)]] StructuredBuffer<float2> previousField;
[[vk::binding(1,0)]] RWStructuredBuffer<float2> currentField;
struct Uniform {int4 scroll;float4 response;uint4 control;float4 endpoints[16],properties[16];};
[[vk::binding(2,0)]] ConstantBuffer<Uniform> field;
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    uint index=id.y*128+id.x;
    if(field.control.z==0){currentField[index]=0;return;}
    int2 old=int2(id.xy)+field.scroll.xy;float2 value=0;
    if(field.control.y==0&&all(old>=0)&&all(old<128))value=previousField[old.y*128+old.x];
    float2 p=(float2(id.xy)+.5)*.25,target=0;float influence=0;
    for(uint i=0;i<field.control.x;++i) {
        float radius=field.properties[i].x;if(radius<=0)continue;
        float2 a=field.endpoints[i].xy,b=field.endpoints[i].zw,v=b-a;
        float2 d=p-(a+v*saturate(dot(p-a,v)/max(dot(v,v),1e-8)));
        float lengthD=length(d),weight=(1-smoothstep(0,radius,lengthD))*field.properties[i].y;
        if(weight>influence){influence=weight;target=(lengthD>1e-5?d/lengthD:float2(1,0))*weight;}
    }
    value=influence>0?lerp(value,target,field.response.y):value*field.response.x;
    currentField[index]=value/max(1,length(value));
}

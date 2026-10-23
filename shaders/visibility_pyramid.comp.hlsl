[[vk::binding(5,0)]] Texture2D<float> source;
[[vk::binding(6,0)]] RWTexture2D<float> destination;
struct Params { uint width,height,base,unused; };
[[vk::push_constant]] ConstantBuffer<Params> params;
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    uint2 size=params.base!=0?uint2(params.width,params.height):max(uint2(1,1),uint2(params.width,params.height)/2);
    if(any(id.xy>=size)) return;
    if(params.base!=0) { destination[id.xy]=source.Load(int3(id.xy,0));return; }
    uint2 first=id.xy*2, end=min(first+2,uint2(params.width,params.height));
    if(id.x+1==size.x)end.x=params.width;
    if(id.y+1==size.y)end.y=params.height;
    float depth=1;
    for(uint y=first.y;y<end.y;++y) for(uint x=first.x;x<end.x;++x)
        depth=min(depth,source.Load(int3(x,y,0)));
    destination[id.xy]=depth;
}

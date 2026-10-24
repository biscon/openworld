#ifndef OW_VEGETATION_HABITAT
#define OW_VEGETATION_HABITAT
uint habitatHash(uint x){x^=x>>16;x*=0x7feb352d;x^=x>>15;x*=0x846ca68b;return x^(x>>16);}
uint2 habitatAdd(uint2 v,int offset){uint lo=v.x+asuint(offset);return uint2(lo,v.y+(offset<0?0xffffffff:0)+(lo<v.x?1:0));}
uint2 habitatShift(uint2 v,uint shift){return uint2((v.x>>shift)|(v.y<<(32-shift)),asuint(asint(v.y)>>shift));}
float habitatLattice(uint2 x,uint2 z,uint seed){return (habitatHash(habitatHash(x.x^seed)^x.y^habitatHash(z.x)^z.y)>>8)*(1.0/16777216.0);}
// key is the signed 64-bit 4 m cell in low/high words. local may span adjacent cells.
float habitatNoise(uint4 key,float2 local,uint shift,uint seed) {
    uint mask=(1u<<shift)-1;float scale=4*(1u<<shift);
    float2 q=(float2(key.x&mask,key.z&mask)*4+local)/scale;
    int2 offset=int2(floor(q));float2 f=frac(q);f=f*f*(3-2*f);
    uint2 x=habitatAdd(habitatShift(key.xy,shift),offset.x),z=habitatAdd(habitatShift(key.zw,shift),offset.y);
    uint2 xx=habitatAdd(x,1),zz=habitatAdd(z,1);
    return lerp(lerp(habitatLattice(x,z,seed),habitatLattice(xx,z,seed),f.x),
                lerp(habitatLattice(x,zz,seed),habitatLattice(xx,zz,seed),f.x),f.y);
}
float3 vegetationHabitatWeights(uint4 key,float2 local,float3 uniformWeights,bool blended,uint seed) {
    if(!blended)return uniformWeights;
    float a=habitatNoise(key,local,7,seed^173u),b=habitatNoise(key,local,7,seed^917u);
    float moor=smoothstep(.25,.75,a),understory=smoothstep(.3,.75,b);
    float3 w=float3(1-moor,moor*(1-understory),moor*understory);
    return w/max(dot(w,1.0.xxx),1e-6);
}
float vegetationClearing(uint4 key,float2 local,bool blended,uint seed) {
    return blended?1-smoothstep(.73,.86,habitatNoise(key,local,3,seed^591u)):1;
}
float vegetationCluster(uint4 key,float2 local,uint species,uint seed,float amount) {
    return lerp(1,smoothstep(.2,.7,habitatNoise(key,local,1,seed^(species*391u+1739u))),amount);
}
float vegetationProbability(float3 weights,uint species) {
    return dot(weights,species==0?float3(1,.65,.35):species==1?float3(.15,.25,1):float3(.15,.75,.4));
}
float vegetationGroundCover(float3 weights,float clearing,bool blended) {
    // Sparse grass does not mean bare soil: moor and understory retain low cover/litter.
    // Small population clearings should not become high-contrast mountain spots.
    return blended?dot(weights,float3(1,.9,.82))*lerp(.85,1,clearing)
                  :vegetationProbability(weights,0)*clearing;
}
bool vegetationExcluded(float2 relative,float3 clearing) {
    return clearing.z>0&&distance(relative,clearing.xy)<clearing.z;
}
#endif

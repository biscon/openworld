// Gradient noise derivative formulas: Copyright (c) 2017 Inigo Quilez.
// Adapted via Luke Mitchell, Copyright (c) 2026.
// MIT License: Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions: The above copyright notice and this permission
// notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
uint hashWord(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float2 hashCell(int2 cell, uint salt = 0) {
    uint h = hashWord(asuint(cell.x) ^ hashWord(asuint(cell.y) ^ params.dimensions.y ^ salt));
    return float2(h >> 8, hashWord(h ^ 0x9e3779b9u) >> 8) * (2.0 / 16777216.0) - 1.0;
}

// Integer world samples multiplied by the exact binary shader frequency.
// Three 32-bit limbs avoid shaderInt64/float64 requirements. The supported
// lattice range is checked on the CPU before dispatch.
uint2 wideMultiply(uint a, uint b) {
    uint a0=a&65535, a1=a>>16, b0=b&65535, b1=b>>16;
    uint t=a0*b0, lo=t&65535, carry=t>>16;
    t=a1*b0+carry; uint mid=t&65535, hi=t>>16;
    t=a0*b1+mid;
    return uint2((t<<16)|lo,a1*b1+hi+(t>>16));
}
uint shiftedWord(uint3 v,uint shift) {
    uint word=shift/32, bit=shift%32;
    uint a=word==0?v.x:word==1?v.y:v.z;
    uint b=word==0?v.y:word==1?v.z:0;
    return bit==0?a:(a>>bit)|(b<<(32-bit));
}
void canonicalAxis(uint2 origin,int offset,float frequency,out int cell,out float f) {
    uint lo=origin.x+asuint(offset);
    uint hi=origin.y+(offset<0?0xffffffff:0)+(lo<origin.x?1:0);
    bool negative=(hi&0x80000000)!=0;
    if(negative) { lo=~lo+1; hi=~hi+(lo==0?1:0); }
    uint bits=asuint(frequency), mantissa=(bits&0x7fffff)|0x800000;
    uint shift=152-((bits>>23)&255); // Quarter metres times mantissa * 2^(exponent-150).
    uint2 a=wideMultiply(lo,mantissa), b=wideMultiply(hi,mantissa);
    uint middle=a.y+b.x;
    uint3 product=uint3(a.x,middle,b.y+(middle<a.y?1:0));
    uint whole=shiftedWord(product,shift);
    uint fraction=shiftedWord(product,shift-24)&0xffffff;
    bool remainder=(shift<32?(product.x&((1u<<shift)-1))!=0:
        product.x!=0 || (product.y&((1u<<(shift-32))-1))!=0);
    f=float(fraction)*(1.0/16777216.0);
    cell=int(whole);
    if(negative) { cell=-cell-(remainder?1:0); f=remainder?1.0-f:0; }
}
void domainPoint(uint octave, float2 local, out int2 cell, out float2 f) {
    NoiseDomain d = params.domains[octave];
    if ((params.sampling.w&1)!=0) {
        #ifdef OW_MATERIAL_CONTINUOUS
        int2 offset=int2(floor(local*4));
#else
        int2 offset=int2(round(local*4));
#endif
        canonicalAxis(params.worldOrigin.xy,offset.x,d.fractionFrequency.z,cell.x,f.x);
        canonicalAxis(params.worldOrigin.zw,offset.y,d.fractionFrequency.z,cell.y,f.y);
#ifdef OW_MATERIAL_CONTINUOUS
        f+=(local*4-float2(offset))*(.25*d.fractionFrequency.z);
        float2 carry=floor(f);cell+=int2(carry);f-=carry;
#endif
        return;
    }
    float2 p = d.fractionFrequency.xy + local * d.fractionFrequency.z;
    float2 whole = floor(p);
    cell = d.lattice.xy + int2(whole);
    f = p - whole;
}
float3 gradientNoise(uint octave, float2 local) {
    int2 i; float2 f;
    domainPoint(octave, local, i, f);
    float2 u = f*f*f*(f*(f*6.0-15.0)+10.0);
    float2 du = 30.0*f*f*(f*(f-2.0)+1.0);
    uint salt = asuint(params.domains[octave].lattice.z);
    float2 a = hashCell(i,salt), b = hashCell(i+int2(1,0),salt);
    float2 c = hashCell(i+int2(0,1),salt), d = hashCell(i+int2(1,1),salt);
    float va = dot(a,f), vb = dot(b,f-float2(1,0));
    float vc = dot(c,f-float2(0,1)), vd = dot(d,f-1.0);
    float k = va-vb-vc+vd;
    return float3(va+u.x*(vb-va)+u.y*(vc-va)+u.x*u.y*k,
        a+u.x*(b-a)+u.y*(c-a)+u.x*u.y*(a-b-c+d)+du*(u.yx*k+float2(vb,vc)-va));
}

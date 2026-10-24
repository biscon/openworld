#ifndef OW_CURVATURE
#define OW_CURVATURE
// sin(a)/a and (1-cos(a))/a^2. Shader sine has an absolute error
// allowance: dividing a tiny sine result by a can visibly move nearby vertices.
// The polynomial path covers the default horizon, including walking distances.
float2 curveArc(float a) {
    float a2=a*a;
    if(a<=.5) {
        float sinc=1+a2*(-1.0/6+a2*(1.0/120+a2*(-1.0/5040+a2/362880)));
        float cosc=.5+a2*(-1.0/24+a2*(1.0/720+a2*(-1.0/40320+a2/3628800)));
        return float2(sinc,cosc);
    }
    float halfSinc=sin(a*.5)/a;
    return float2(sin(a)/a,2*halfSinc*halfSinc);
}
float3 curvePosition(float3 p,float4 c) {
    if(c.z==0)return p;
    float r=length(p.xz),a=r/c.x,h=p.y+c.y;
    float2 arc=curveArc(a);
    float scale=1+h/c.x,k=scale*arc.x;
    return float3(p.x*k,p.y-r*a*scale*arc.y,p.z*k);
}
void curveBasis(float3 p,float4 c,out float3 x,out float3 y,out float3 z) {
    x=float3(1,0,0);y=float3(0,1,0);z=float3(0,0,1);
    if(c.z==0)return;
    float r=length(p.xz),a=r/c.x,h=p.y+c.y;
    float2 u=r>1e-5?p.xz/r:float2(1,0);
    float2 arc=curveArc(a);
    float s=a*arc.x,co=1-a*a*arc.y,k=(1+h/c.x)*arc.x;
    float3 radial=(1+h/c.x)*float3(co*u.x,-s,co*u.y);
    float3 tangent=k*float3(-u.y,0,u.x);
    x=radial*u.x-tangent*u.y;z=radial*u.y+tangent*u.x;
    y=float3(s*u.x,co,s*u.y);
}
float3 curveNormal(float3 p,float3 n,float4 c) {
    float3 x,y,z;curveBasis(p,c,x,y,z);
    return normalize(n.x*cross(y,z)+n.y*cross(z,x)+n.z*cross(x,y));
}
float3 curveDirection(float3 p,float3 n,float4 c) {
    float3 x,y,z;curveBasis(p,c,x,y,z);
    return normalize(n.x*x+n.y*y+n.z*z);
}
#endif

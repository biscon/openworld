#ifndef OW_TERRAIN_MORPH
#define OW_TERRAIN_MORPH
// The same diagonal as the indexed patch topology. Geographic XZ never collapses.
void terrainParentTriangle(uint2 grid,uint step,uint cells,out uint2 a,out uint2 b,out uint2 c,out float3 w) {
    a=min(grid/step,cells/step-1)*step;
    float2 f=float2(grid-a)/step;
    if(f.x>=f.y) { b=a+uint2(step,0);c=a+step;w=float3(1-f.x,f.x-f.y,f.y); }
    else { b=a+step;c=a+uint2(0,step);w=float3(1-f.y,f.x,f.y-f.x); }
}
float3 terrainTrianglePoint(float3 a,float3 b,float3 c,float3 w) {
    // Exact endpoints also preserve equality across data tiles and geometry levels.
    if(w.x==1)return a;if(w.y==1)return b;if(w.z==1)return c;
    precise float3 p=a*w.x+b*w.y+c*w.z;return p;
}
float3 terrainEndpointBlend(float3 a,float3 b,float m) {
    if(m==0)return a;if(m==1)return b;
    precise float3 p=a+(b-a)*m;return p;
}
#endif

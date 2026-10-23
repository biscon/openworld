#define TERRAIN_VERTEX_PROBE
#define main terrainVertex
#include "terrain_stream.vert.hlsl"
#undef main
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=(draw.patch.w+1)*(draw.patch.w+1)) return;
    vertexOutput[uint(draw.tile.w)+id.x]=terrainVertex(id.x,0).position;
}

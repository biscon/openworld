#define OW_VEGETATION
#define OW_BINDLESS
#include "mesh_surface.hlsli"
void main(MeshSurface i){float2 dx=ddx(i.uv),dy=ddy(i.uv);vegetationMask(i);meshBase(materialRecords[i.material],i.uv,dx,dy);}

#include "mesh_surface.hlsli"
void main(MeshSurface i){MaterialRecord r=materialRecords[i.material];meshBase(r,i.uv,ddx(i.uv),ddy(i.uv));}

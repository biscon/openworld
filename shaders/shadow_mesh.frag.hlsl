#include "mesh_surface.hlsli"
struct Input {float4 position:SV_Position;[[vk::location(3)]]float2 uv:TEXCOORD1;[[vk::location(4)]]nointerpolation uint material:TEXCOORD2;};
void main(Input i) { MaterialRecord r=materialRecords[i.material];meshBase(r,i.uv,ddx(i.uv),ddy(i.uv)); }

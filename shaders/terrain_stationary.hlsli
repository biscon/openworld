#ifndef OW_TERRAIN_STATIONARY
#define OW_TERRAIN_STATIONARY
#include "terrain_morph.hlsli"
uint2 terrainStitchedVertex(uint2 p,uint cells,uint edges) {
    if((p.x==0&&(edges&1)!=0)||(p.x==cells&&(edges&2)!=0))p.y&=~1u;
    if((p.y==0&&(edges&4)!=0)||(p.y==cells&&(edges&8)!=0))p.x&=~1u;
    return p;
}
bool terrainStationaryCoarse(uint2 local,uint cells,uint edges,uint corners) {
    bool coarse=(local.x==0&&(edges&1)!=0)||(local.x==cells&&(edges&2)!=0)||
                (local.y==0&&(edges&4)!=0)||(local.y==cells&&(edges&8)!=0);
    if((local.x==0||local.x==cells)&&(local.y==0||local.y==cells))
        coarse=coarse||(corners&(1u<<((local.x!=0?1:0)+(local.y!=0?2:0))))!=0;
    return coarse;
}
#endif

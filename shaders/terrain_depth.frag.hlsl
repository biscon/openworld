#include "terrain_stream_common.hlsli"
#include "terrain_surface.hlsli"
#include "terrain_clip.hlsli"
void main(TerrainSurface input) { loadDraw(input.recordID); clipTerrainView(input); }

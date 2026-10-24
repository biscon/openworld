#define TERRAIN_SHADOW_FRAGMENT
#include "terrain_stream_common.hlsli"
#include "shadow_terrain_surface.hlsli"
void main(ShadowTerrainSurface input) {
    loadDraw(input.recordID);
    if(draw.tile.y!=0) {
        float2 p=input.gridPosition*draw.tile.x+draw.offset.xz-view.camera.xz;
        bool inside=all(p>=view.shadowClip.xy)&&all(p<=view.shadowClip.zw);
        // Extra top-level casters fill only the exterior of the camera footprint.
        bool exterior=(draw.options.w&16)!=0;
        if(inside==exterior)discard;
    }
}

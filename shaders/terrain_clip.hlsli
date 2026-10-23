// Shared by depth and colour passes; coverage must agree exactly.
void clipTerrainView(TerrainSurface input) {
    // Selection rejects whole patches; clip the outermost partial patches to
    // the requested square so coarse tails cannot form disconnected islands.
    if(draw.tile.y!=0) {
        float2 p=input.gridPosition*draw.tile.x+draw.offset.xz-view.camera.xz;
        if(any(p<view.clip.xy)||any(p>view.clip.zw)) discard;
    }
}

// Compact endpoint-preserving grids; offsets count float2 height pairs.
uint heightLevelOffset(uint cells,uint level) {
    uint offset=0;
    for(uint l=0;l<level;++l) {uint w=(cells>>l)+1;offset+=w*w;}
    return offset;
}
float2 filteredTerrainHeight(float2 p,uint level) {
    if(level+(params.sampling.w>>8)==0) return evaluateTerrain(p);
    float delta=params.shape.x*float(1u<<level)*.5;
    const float w[5]={1,4,6,4,1};
    float2 h=0;
    [unroll] for(int z=-2;z<=2;++z) [unroll] for(int x=-2;x<=2;++x)
        h+=evaluateTerrain(p+float2(x,z)*delta)*(w[x+2]*w[z+2]);
    return h*(1.0/256.0);
}

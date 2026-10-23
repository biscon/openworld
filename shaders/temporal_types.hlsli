#ifndef OW_TEMPORAL_TYPES
#define OW_TEMPORAL_TYPES
struct TemporalView {
    column_major float4x4 current,previous,raster;
    float4 originDelta,pixels,state;
};
#endif

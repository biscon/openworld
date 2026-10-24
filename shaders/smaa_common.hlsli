#include "spatial_common.hlsli"
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS settings.metrics
#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex,coord) tex.SampleLevel(linearClamp,coord,0)
#define SMAASampleLevelZeroPoint(tex,coord) tex.SampleLevel(pointClamp,coord,0)
#define SMAASampleLevelZeroOffset(tex,coord,offset) tex.SampleLevel(linearClamp,(coord)+(offset)*SMAA_RT_METRICS.xy,0)
#define SMAASample(tex,coord) tex.SampleLevel(linearClamp,coord,0)
#define SMAASamplePoint(tex,coord) tex.SampleLevel(pointClamp,coord,0)
#define SMAASampleOffset(tex,coord,offset) tex.SampleLevel(linearClamp,(coord)+(offset)*SMAA_RT_METRICS.xy,0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#include "../third_party/smaa/SMAA.hlsl"

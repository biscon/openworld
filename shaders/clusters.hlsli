#ifndef OW_CLUSTERS
#define OW_CLUSTERS
struct LocalLightRecord {float4 positionRange,colourIntensity,directionOuter,parameters;};
struct ClusterUniform {column_major float4x4 view;uint4 grid,options;float4 depth,projection;};
[[vk::binding(7,1)]] ConstantBuffer<ClusterUniform> clusters;
[[vk::binding(4,1)]] StructuredBuffer<LocalLightRecord> localLights;
#ifdef OW_CLUSTER_BUILD
[[vk::binding(5,1)]] RWStructuredBuffer<uint> clusterCounts;
[[vk::binding(6,1)]] RWStructuredBuffer<uint> clusterIndices;
struct ClusterBounds {float4 low,high;};
[[vk::binding(8,1)]] StructuredBuffer<ClusterBounds> clusterBounds;
#else
[[vk::binding(5,1)]] StructuredBuffer<uint> clusterCounts;
[[vk::binding(6,1)]] StructuredBuffer<uint> clusterIndices;
#endif
uint sliceForDepth(float depth) {return uint(clamp(floor(log(max(depth,clusters.depth.x)/clusters.depth.x)*clusters.depth.z),0,float(clusters.grid.z-1)));}
#endif

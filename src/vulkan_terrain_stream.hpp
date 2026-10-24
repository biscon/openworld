#pragma once
#include "terrain_cache.hpp"
#include "terrain_detail_policy.hpp"
#include "terrain_generation_budget.hpp"
#include "terrain_ground_lod.hpp"
#include "vegetation_stream.hpp"
#include "vulkan_post.hpp"
#include "vulkan_shadows.hpp"
#include "vulkan_terrain.hpp"
#include "vulkan_visibility.hpp"
#include <chrono>
namespace ow {
struct SceneSettings;
class VulkanTerrainStream {
  public:
    VulkanTerrainStream(VulkanTerrain &, TerrainStreamingConfig, const std::filesystem::path &,
                        uint32_t temporalCapacity = 65536);
    ~VulkanTerrainStream();
    void settings(const TerrainSettings &);
    void beginFrame(uint32_t frame, const Camera &, const SceneSettings &);
    void record(VkCommandBuffer, uint32_t frame);
    void prepareDraw(uint32_t frame, const Camera &, const Camera &observer, const glm::mat4 &,
                     const SceneSettings &, uint32_t viewportHeight);
    void prepareVisibility(VulkanVisibility &, uint32_t frame, const Camera &, const Camera &,
                           const SceneSettings &, VulkanPost *post = nullptr);
    void draw(VkCommandBuffer, uint32_t frame, const Camera &, const Camera &observer, const SceneSettings &,
              bool finalPass = true, VulkanVisibility *visibility = nullptr);
    void prepareShadows(uint32_t frame, const Camera &, const SceneSettings &, VulkanShadows &);
    void drawShadows(VkCommandBuffer, VkPipelineLayout, VulkanShadows &);
    VkDescriptorSetLayout descriptorLayout() const { return descriptorLayout_; }
    void submitted(uint32_t frame);
    void prepareVegetation(const Camera &, bool eroded);
    void vegetationDemandRadius(float radius) { vegetationDemandRadius_ = radius; }
    bool vegetationSurface(VegetationCellKey, VegetationTerrainCell &, bool retain = true);
    VkBuffer vegetationHeights() const { return heights_.handle; }
    uint64_t vegetationRevision() const { return epoch_ * 2 + (vegetationEroded_ ? 1 : 0); }
    uint64_t vegetationSurfaceVersion() const { return vegetationPartition_; }
    uint32_t vegetationSeed() const { return settings_.seed; }
    void vegetationReservation(double milliseconds, uint32_t groups = 0) {
        vegetationReserved_ = milliseconds;
        vegetationGroups_ = groups;
    }
    double vegetationRemaining() const {
        return std::max(0.0,
                        TerrainGenerationBudget::targetMilliseconds - stats_.generationEstimatedMilliseconds);
    }

    VkPipelineLayout graphicsLayout() const { return graphicsLayout_; }
    const TerrainStreamStats &stats() const { return stats_; }
    void reviewQuality(float &scale, float &focal) const {
        scale = qualityScale_;
        focal = qualityFocalPixels_;
    }
    void restoreReviewQuality(float scale, float focal) {
        qualityScale_ = scale;
        qualityFocalPixels_ = focal;
    }
    std::vector<TerrainPatchKey> reviewPatches() const {
        return ground_ ? ground_->snapshot() : std::vector<TerrainPatchKey>{};
    }
    void restoreReviewPatches(const std::vector<TerrainPatchKey> &keys) {
        if (ground_)
            ground_->restore(keys);
    }
    const TerrainRenderStats &renderStats() const { return ground_ ? ground_->stats() : lod_->stats(); }
    const std::vector<TerrainPatchDraw> &patches() const {
        return ground_ ? ground_->draws() : lod_->draws();
    }
    // Diagnostic owned copies, never used by frame processing.
    std::vector<glm::vec2> readHeights(ChunkCoord, uint32_t tileLevel = 0, uint32_t geometryLevel = 0);
    std::vector<TerrainTileSnapshot> residency() const;
    TerrainDetailJobSnapshot detailJob() const;
    std::vector<glm::vec4> readVertexPositions();
    std::vector<glm::vec4> readNormals(ChunkCoord, uint32_t level, uint32_t tileLevel = 0);

  private:
    using Buffer = VulkanTerrain::Buffer;
    struct Image {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        uint32_t width{}, layers{};
        uint64_t bytes{};
    };
    TerrainHistory history_;
    VulkanTerrain &g_;
    TerrainPayloadLayout layout_;
    TerrainStreamingConfig config_;
    std::unique_ptr<TerrainCache> cache_;
    std::unique_ptr<TerrainStreamer> stream_;
    std::unique_ptr<TerrainLod> lod_;
    std::unique_ptr<TerrainGroundLod> ground_;
    TerrainSettings settings_{};
    uint64_t epoch_ = 1, serial_ = 0, completed_ = 0, shaderHash_ = 0, deviceHash_ = 0;
    uint32_t capacity_ = 0, detailCapacity_ = 0;
    std::array<TerrainTileKey, 64> shadowDemand_{};
    uint32_t shadowDemandCount_ = 0, shadowMissingBoundsCount_ = 0;
    std::array<std::array<int64_t, 4>, 4> shadowMissingBounds_{};
    VkDeviceSize heightStride_ = 0, weightStride_ = 0;
    uint32_t nearSlots_ = 0, distantSlots_ = 0;
    VkDeviceSize compactStride_ = 0;
    bool compact(uint32_t slot) const { return slot >= nearSlots_ && slot < nearSlots_ + distantSlots_; }
    VkDeviceSize heightSkip(uint32_t slot) const { return compact(slot) ? layout_.heightOffsets[1] : 0; }
    VkDeviceSize heightOffset(uint32_t slot) const {
        return slot < nearSlots_ ? heightStride_ * slot
               : compact(slot)   ? heightStride_ * nearSlots_ + compactStride_ * (slot - nearSlots_)
                                 : heightStride_ * (slot - distantSlots_) + compactStride_ * distantSlots_;
    }
    Buffer heights_;
    std::array<Buffer, 2> uploads_{}, readbacks_{}, views_{}, weights_{};
    Image coarse_;
    std::array<Image, maxTerrainLodLevels> fine_{};
    VkDescriptorSetLayout descriptorLayout_{};
    VkDescriptorPool descriptorPool_{};
    VkPipelineLayout graphicsLayout_{};
    VkPipeline packPipeline_{}, tileHeightPipeline_{}, tileNormalPipeline_{}, qualityBoundsPipeline_{},
        vertexProbePipeline_{};
    struct DrawRecord {
        std::array<glm::uvec4, 8> constants;
        VkDescriptorSet set;
        uint32_t count = 0, first = 0, group = 0, order = 0, batch = ~0u;
        VisibilityBounds bounds;
    };
    std::vector<DrawRecord> diagnosticDraws_, preparedDraws_, casterRecords_;
    void collectDrawRecords(uint32_t frame, const Camera &, const Camera &, const SceneSettings &,
                            bool casters, std::vector<DrawRecord> &);
    struct ShadowDraw {
        VkDescriptorSet set;
        uint32_t caster;
    };
    std::vector<ShadowDraw> shadowDraws_;
    std::vector<VkDescriptorSet> sets_;
    struct DetailSlot {
        TerrainHandle owner;
        uint64_t retireAfter{};
        bool retiring = false;
    };
    std::vector<DetailSlot> details_;
    struct Meta {
        uint64_t revision = 1, readRevision = 0;
        int readLease = -1;
        bool detailMissing = false, wanted = false;
        double restartAfter = 0;
        TerrainDetailRank rank;
        float range = 0;
        glm::vec2 heights{};
    };
    std::vector<Meta> meta_;
    std::vector<glm::vec2> boundsScratch_;
    std::vector<ChunkCoord> activeCoords_;
    std::vector<uint32_t> activeSlots_, priorities_;
    TerrainStreamStats stats_{};
    float range_ = 64, rangeTarget_ = 64;
    float qualityScale_ = 1, qualityFocalPixels_ = 0;
    std::chrono::steady_clock::time_point previous_ = std::chrono::steady_clock::now();
    float dt_ = 1.f / 60;
    double elapsed_ = 0;
    TerrainDetailPolicy detailPolicy_;
    struct Upload {
        int lease = -1;
        TerrainHandle handle;
        TerrainProduct product{};
        int detail = -1;
        uint64_t offset = 0;
        bool generated = false;
        uint64_t revision = 0, readyAfter = 0;
    } upload_;
    struct Generation {
        TerrainHandle handle;
        TerrainRequest request{};
        bool active = false, detailOnly = false;
        uint32_t stage = 0, level = 0,
                 group = 0; // height, normals, filter, bounds, pack, export
        uint64_t readyAfter = 0, offset = 0;
        TerrainProduct product = TerrainProduct::Coverage;
        int lease = -1;
        bool exportPending = false, published = false;
        uint64_t directOffset = 0;
        int directDetail = -1;
        uint64_t revision = 0, groups = 0;
        double started = 0;
        bool cancelled = false;
    } gen_;
    struct Publication {
        TerrainHandle handle;
        TerrainProduct product{};
        int detail = -1;
        bool pending = false;
        uint64_t revision = 0;
    };
    struct Frame {
        uint64_t serial = 0;
        Publication publish;
        int exportLease = -1;
        uint64_t exportOffset = 0, exportBytes = 0;
        bool timestamp = false, terrainTimestamp = false;
        struct Segment {
            TerrainComputeKernel kernel{};
            uint32_t groups = 0;
        };
        std::array<Segment, TerrainGenerationBudget::maximumSegments> segments{};
        uint32_t segmentCount = 0;
        uint32_t generationGroups = 0;
        TerrainHandle boundsHandle;
    };
    std::array<Frame, 2> frames_{};
    // Terrain pair, then one pair per stage segment. First/last stage stamps
    // also measure overall generation without redundant timestamps.
    static constexpr uint32_t queriesPerFrame = 2 + 2 * TerrainGenerationBudget::maximumSegments;
    struct Read {
        TerrainHandle handle;
        uint64_t revision = 0;
        bool active = false, cancelled = false;
    };
    std::array<Read, 2> reads_{};
    VkQueryPool queryPool_{};
    double timestampPeriod_ = 0;
    TerrainGenerationBudget generationBudget_;
    double vegetationReserved_ = 0;
    uint32_t vegetationGroups_ = 0;
    struct VegetationBucket {
        std::array<uint32_t, 16> indices{};
        uint32_t count = 0;
    };
    static constexpr int vegetationIndexRadius = 55, vegetationIndexSide = 111;
    std::array<VegetationBucket, vegetationIndexSide * vegetationIndexSide> vegetationBuckets_{};
    // Owned records keep bucket indices valid when the renderer changes traversal order.
    std::vector<TerrainPatchDraw> vegetationPatches_;
    VegetationCellKey vegetationCentre_;
    uint64_t vegetationPartition_ = 0;
    bool vegetationEroded_ = true, vegetationReady_ = false;
    float vegetationDemandRadius_ = 0;
    bool imagesInitialized_ = false, generatorImagesInitialized_ = false;
    uint64_t imageBytes_ = 0, ioBackoffUntil_ = 0;
    TerrainCacheKey key(TerrainHandle, TerrainProduct) const;
    int allocateDetail(TerrainHandle);
    void setCoverageBounds(TerrainHandle, int lease);
    void applyBounds(TerrainHandle, const glm::vec2 *);
    bool directPublication(VkCommandBuffer, uint32_t, uint64_t &);
    void pollCache();
    void schedule();
    bool detailValid(TerrainHandle, uint64_t) const;
    bool detailRunnable(uint32_t) const;
    double generationProgress() const;
    void cancelGeneration(bool preempted = false);
    void updateDetailDemand(const Camera &, const SceneSettings &);
    void generate(VkCommandBuffer, uint32_t);
    void transfer(VkCommandBuffer, uint32_t, uint64_t &budget);
    uint64_t copyPayload(VkCommandBuffer, uint32_t, TerrainProduct, bool uploading, uint64_t offset,
                         uint64_t budget, uint32_t slot, int detail);
    void createImage(Image &, uint32_t width, uint32_t layers);
    void initialize();
    void warmGeneration();
    void cleanup();
    void error();
};
} // namespace ow

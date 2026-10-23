#pragma once
#include "terrain_cache.hpp"
#include "terrain_detail_policy.hpp"
#include "terrain_generation_budget.hpp"
#include "vulkan_shadows.hpp"
#include "vulkan_terrain.hpp"
#include "vulkan_visibility.hpp"
#include <chrono>
namespace ow {
struct SceneSettings;
class VulkanTerrainStream {
  public:
    VulkanTerrainStream(VulkanTerrain &, TerrainStreamingConfig, const std::filesystem::path &);
    ~VulkanTerrainStream();
    void settings(const TerrainSettings &);
    void beginFrame(uint32_t frame, const Camera &, const SceneSettings &);
    void record(VkCommandBuffer, uint32_t frame);
    void prepareDraw(uint32_t frame, const Camera &, const Camera &observer, const glm::mat4 &,
                     const SceneSettings &, uint32_t viewportHeight);
    void prepareVisibility(VulkanVisibility &, uint32_t frame, const Camera &, const Camera &,
                           const SceneSettings &);
    void draw(VkCommandBuffer, uint32_t frame, const Camera &, const Camera &observer, const SceneSettings &,
              bool finalPass = true, VulkanVisibility *visibility = nullptr);
    void prepareShadows(uint32_t frame, const Camera &, const SceneSettings &, VulkanShadows &);
    void drawShadows(VkCommandBuffer, VkPipelineLayout, VulkanShadows &);
    VkDescriptorSetLayout descriptorLayout() const { return descriptorLayout_; }
    void submitted(uint32_t frame);
    VkPipelineLayout graphicsLayout() const { return graphicsLayout_; }
    const TerrainStreamStats &stats() const { return stats_; }
    const TerrainRenderStats &renderStats() const { return lod_->stats(); }
    const std::vector<TerrainPatchDraw> &patches() const { return lod_->draws(); }
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
    VulkanTerrain &g_;
    TerrainPayloadLayout layout_;
    TerrainStreamingConfig config_;
    std::unique_ptr<TerrainCache> cache_;
    std::unique_ptr<TerrainStreamer> stream_;
    std::unique_ptr<TerrainLod> lod_;
    TerrainSettings settings_{};
    uint64_t epoch_ = 1, serial_ = 0, completed_ = 0, shaderHash_ = 0, deviceHash_ = 0;
    uint32_t capacity_ = 0, detailCapacity_ = 0;
    std::array<TerrainTileKey, 64> shadowDemand_{};
    uint32_t shadowDemandCount_ = 0, shadowMissingBoundsCount_ = 0;
    std::array<std::array<int64_t, 4>, 4> shadowMissingBounds_{};
    VkDeviceSize heightStride_ = 0, weightStride_ = 0;
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

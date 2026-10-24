#pragma once
#include "terrain_lod.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
struct TerrainDevice {
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    VkCommandPool commandPool;
};
// Fixed resident slots with shared generation scratch and topology. No pointers
// to storage escape into the application.
class VulkanTerrain {
  public:
    VulkanTerrain(TerrainDevice device, TerrainGrid grid, uint32_t capacity, uint32_t normalSubdivisions,
                  uint32_t chunkCapacity, const std::filesystem::path &shaders,
                  TerrainLodConfig lod = {}, VkDescriptorSetLayout lightingLayout = VK_NULL_HANDLE,
                  VkDescriptorSetLayout materialLayout = VK_NULL_HANDLE,
                  VkDescriptorSetLayout visibilityLayout = VK_NULL_HANDLE);
    ~VulkanTerrain();
    VulkanTerrain(const VulkanTerrain &) = delete;
    VulkanTerrain &operator=(const VulkanTerrain &) = delete;
    void setGrid(TerrainGrid grid);
    void request(const TerrainRequest &r) { request(terrainNeighbourhood(r, true)); }
    void request(const TerrainBatchRequest &r) { updates_.request(r); }
    uint32_t record(VkCommandBuffer cmd, uint32_t frame);
    void submitted(uint32_t generated);
    void bind(VkCommandBuffer cmd, uint32_t frame, uint32_t slot, uint32_t level = 0) const;
    void consumeBounds(uint32_t frame); // Only after this frame slot fence.
    void setView(uint32_t frame, const glm::mat4 &, glm::vec3 observer);
    TerrainLod *lod() { return lod_.get(); }
    bool boundsReady() const;
    uint32_t normalHalo(uint32_t level) const { return (1u << (normalLevels_ - 1 - level)) - 1; }
    uint32_t normalLevels() const { return normalLevels_; }
    TerrainIndexRange patchRange(uint32_t mask) const { return patchTopology_.ranges[mask]; }
    uint64_t contentRevision(uint32_t slot) const { return slots_.at(slot).revision; }
    uint64_t generationCount() const { return updates_.count(); }
    uint64_t allocatedBytes() const { return allocatedBytes_; }
    uint32_t chunkCount() const { return updates_.requested().count; }
    uint32_t indexCount() const { return grid_.cells * grid_.cells * 6; }
    ChunkCoord coordinate(uint32_t slot) const { return updates_.requested().chunks[slot]; }
    VkDescriptorSetLayout descriptorLayout() const { return descriptorLayout_; }
    VkDescriptorSet descriptor(uint32_t frame, uint32_t slot) const {
        return slots_[slot].descriptors[frame][0];
    }
    VkBuffer shadowIndices() const { return lod_ ? patchIndices_.handle : indices_.handle; }
    VkPipelineLayout graphicsLayout() const { return graphicsLayout_; }
    std::vector<uint32_t> readIndices();
    std::vector<TerrainSample>
    readback(uint32_t slot = 0); // Blocking diagnostics, never the frame path.
    std::vector<glm::vec4> readNormals(uint32_t slot,
                                       uint32_t level = 0); // base layer followed by eroded layer
    // Blocking diagnostics; reuses scratch and invalidates slot zero.
    // Non-streamed, non-LOD only.
    std::vector<TerrainGeographySample> sampleTerrainGeography(TerrainGrid, const TerrainRequest &,
                                                               bool canonical = true);
    std::vector<glm::vec2> sampleTerrainHeightLevel(TerrainGrid, const TerrainRequest &, uint32_t level,
                                                    bool ridgeFixture);
    std::vector<TerrainSample> sampleTerrainNormalFixture(TerrainGrid, uint32_t level,
                                                          uint32_t tileLevel);
    std::vector<TerrainSample> normalsForFixture(const std::vector<glm::vec2> &heights);
    std::vector<TerrainSample> sampleCurvatureFixture(const std::vector<TerrainSample> &,
                                                     glm::vec4 curvature, bool reducedTrigPrecision);

  private:
    friend class VulkanTerrainStream;
    struct Buffer {
        VkBuffer handle{};
        VkDeviceMemory memory{};
        void *mapped{};
        VkDeviceSize size{}, allocationSize{};
    };
    TerrainDevice context_;
    VkDescriptorSetLayout visibilityLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout_ = VK_NULL_HANDLE; // Borrowed from renderer.
    TerrainGrid grid_;
    uint32_t capacity_, normalSubdivisions_, chunkCapacity_, indexCells_ = 0;
    TerrainBatchUpdates updates_;
    struct Slot {
        bool valid = false, normalImageInitialized = false;
        TerrainParameters parameters{};
        bool boundsValid = false;
        uint64_t revision = 0;
        std::array<VkImageView, maxTerrainLodLevels> normalViews{};
        std::array<std::array<VkDescriptorSet, maxTerrainLodLevels>, 2> descriptors{};
    };
    std::array<Slot, maxTerrainChunks> slots_{};
    VkDeviceSize sampleStride_ = 0, sampleBytes_ = 0, parameterStride_ = 0;
    uint64_t allocatedBytes_ = 0;
    Buffer heights_, samples_, indices_, patchIndices_, bounds_;
    std::unique_ptr<TerrainLod> lod_;
    TerrainPatchTopology patchTopology_;
    uint32_t normalLevels_ = 1;
    VkDeviceSize boundsStride_ = 0;
    std::array<VkImage, maxTerrainLodLevels> normalImages_{};
    std::array<VkDeviceMemory, maxTerrainLodLevels> normalMemories_{};
    std::array<Buffer, 2> views_{}, boundsReadbacks_{};
    struct BoundsTicket {
        bool pending = false;
        uint64_t revision{};
        TerrainGrid grid;
        TerrainRequest request;
    };
    std::array<std::array<BoundsTicket, maxTerrainChunks>, 2> tickets_{};
    VkSampler normalSampler_{};
    std::array<Buffer, 2> parameters_{};
    VkDescriptorSetLayout descriptorLayout_{};
    VkDescriptorPool descriptorPool_{};
    VkPipelineLayout computeLayout_{}, graphicsLayout_{};
    VkPipeline heightPipeline_{}, normalPipeline_{}, filterPipeline_{}, boundsPipeline_{};
    std::array<VkPipeline, 2> heightLevelPipelines_{};
    VkPipeline normalFixturePipeline_{};
    VkPipeline curvatureFixturePipeline_{};
    VkPipeline geographyPipeline_{}; // Lazily created for blocking diagnostics only.
    std::filesystem::path geographyShader_;
    void readBuffer(const Buffer &, void *out, VkDeviceSize bytes, VkDeviceSize offset = 0);
    void createNormalImage(uint32_t level);
    void transitionNormals(VkCommandBuffer, uint32_t slot, uint32_t level, bool writing);
    void requireReadable(uint32_t slot) const;
    void initialize(const std::filesystem::path &shaders);
    void cleanup();
    void createBuffer(Buffer &, VkDeviceSize, VkBufferUsageFlags, bool host = false);
    void destroyBuffer(Buffer &);
    VkPipeline pipeline(const std::filesystem::path &, VkPipelineLayout layout = VK_NULL_HANDLE);
    VkCommandBuffer beginImmediate();
    void endImmediate(VkCommandBuffer);
    void recordNormals(VkCommandBuffer, uint32_t frame, uint32_t slot);
};
} // namespace ow

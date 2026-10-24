#pragma once
#include "render_resources.hpp"
#include "vegetation.hpp"
#include "vulkan_terrain_stream.hpp"
namespace ow {
struct VegetationGpuCell {
    // phase.xy is the stable wind phase; phase.w selects the saved pre-repair attachment.
    glm::vec4 lo{}, hi{}, origin{}, previousOrigin{}, phase{}, casterLo{}, casterHi{};
};
static_assert(sizeof(VegetationGpuCell) == 112);
class VulkanVegetationStream {
  public:
    VulkanVegetationStream(VkPhysicalDevice, VkDevice, const std::filesystem::path &, VkBuffer population,
                           uint64_t populationBytes, VegetationStreamConfig, const VegetationLibrary &,
                           uint64_t budget);
    ~VulkanVegetationStream();
    void prepare(uint32_t frame, WorldPosition camera, WorldPosition previous, const VegetationSettings &,
                 VulkanTerrainStream *, VegetationGpuCell *, VegetationStats &);
    void restrictPopulation(double remaining, VegetationGpuCell *, VegetationStats &);
    void record(VkCommandBuffer);
    void submitted();
    void observe(double milliseconds, uint32_t updates);
    const render_detail::Buffer &attachments() const { return attachments_; }
    const render_detail::Buffer &previousAttachments() const { return previousAttachments_; }
    uint64_t bytes() const { return bytes_; }
    float demandRadius(float distance) const {
        return std::min(config_.demandRadius, std::max(distance, shadowDistance_) + 16);
    }
    const auto &orderedCells() const { return residency_.ordered(); }
    uint32_t updates() const { return updateCount_; }
    uint32_t identity(uint32_t index) const;
    void copySnapshot(VkCommandBuffer);
    std::vector<VegetationCellSnapshot> snapshot() const;

  private:
    struct Job {
        glm::uvec4 key{}, config{}, canonical{};
        glm::vec4 canonicalGrid{}, habitat{}, elevation{}, clearing{};
        glm::uvec4 assets{}, extraAssets{};
        glm::vec4 habitatMode{};
        std::array<glm::vec4, 8> affinities{}, placement{};
        std::array<VegetationTerrainPatch, 16> patches{};
        TerrainParameters geography{};
    };
    static_assert(sizeof(Job) == 1936);
    struct Frame {
        render_detail::Buffer jobs;
        VkDescriptorSet set{};
        VkBuffer heightBuffer{};
        uint64_t serial = 0;
    };
    VkPhysicalDevice physical_;
    VkDevice device_;
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool pool_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    std::array<Frame, 2> frames_;
    std::array<Job, 32> jobs_;
    std::array<VkBufferCopy, 32> previousCopies_{};
    uint32_t previousCopyCount_ = 0;
    render_detail::Buffer attachments_, previousAttachments_;
    std::vector<uint64_t> publishedSignatures_;
    std::vector<uint32_t> publishedGenerations_;
    std::vector<glm::vec2> publishedBounds_;
    struct SurfaceCache {
        VegetationTerrainCell surface;
        uint64_t version = 0;
        uint32_t generation = 0;
        bool ready = false;
    };
    std::vector<SurfaceCache> surfaces_;
    VegetationResidency residency_;
    VegetationStreamConfig config_;
    uint64_t bytes_ = 0, budget_ = 0, serial_ = 0, completed_ = 0;
    render_detail::Buffer diagnostic_;
    VkBuffer population_{};
    std::vector<VegetationTerrainCell> diagnosticSurfaces_;
    std::vector<uint8_t> diagnosticHistory_;
    uint32_t frame_ = 0, updateCount_ = 0, seed_ = 0;
    glm::uvec4 assets_{~0u}, extraAssets_{~0u};
    std::array<glm::vec4, 8> affinities_{}, placement_{};
    uint32_t libraryRevision_ = 0;
    float envelope_ = 3, shadowDistance_ = 0;
    double cellCost_ = .5 / 4;
    std::vector<uint64_t> previousSignatures_, currentSignatures_;
    std::vector<uint32_t> previousGenerations_, currentGenerations_;
    void allocate(render_detail::Buffer &, uint64_t, VkBufferUsageFlags, bool host = false);
};
} // namespace ow

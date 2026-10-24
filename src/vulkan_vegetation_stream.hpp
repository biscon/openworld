#pragma once
#include "render_resources.hpp"
#include "vegetation.hpp"
#include "vulkan_terrain_stream.hpp"
namespace ow {
struct VegetationGpuCell {
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
    const render_detail::Buffer &attachments(uint32_t frame) const { return frames_[frame].attachments; }
    uint64_t bytes() const { return bytes_; }
    uint32_t updates() const { return updateCount_; }
    uint32_t identity(uint32_t index) const;
    void copySnapshot(VkCommandBuffer);
    std::vector<VegetationCellSnapshot> snapshot() const;

  private:
    struct Job {
        glm::uvec4 key{}, config{}, canonical{};
        glm::vec4 canonicalGrid{}, habitat{}, elevation{}, clearing{};
        glm::uvec4 assets{};
        std::array<VegetationTerrainPatch, 16> patches{};
    };
    static_assert(sizeof(Job) == 1152);
    struct Frame {
        render_detail::Buffer attachments, jobs;
        VkDescriptorSet set{};
        VkBuffer heightBuffer{};
        uint64_t serial = 0;
        std::vector<uint64_t> signatures;
        std::vector<uint32_t> generations;
    };
    VkPhysicalDevice physical_;
    VkDevice device_;
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool pool_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    std::array<Frame, 2> frames_;
    std::array<Job, 32> jobs_;
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
    glm::uvec4 assets_{~0u};
    float envelope_ = 3;
    double cellCost_ = .5 / 4;
    std::vector<uint64_t> previousSignatures_, currentSignatures_;
    std::vector<uint32_t> previousGenerations_, currentGenerations_;
    void allocate(render_detail::Buffer &, uint64_t, VkBufferUsageFlags, bool host = false);
};
} // namespace ow

#pragma once
#include "temporal.hpp"
#include "trees.hpp"
#include "vulkan_shadows.hpp"
#include "vulkan_visibility.hpp"
namespace ow {
class VulkanTerrainStream;
class VulkanTrees {
  public:
    VulkanTrees(VkPhysicalDevice, VkDevice, uint32_t queueFamily, VkDescriptorSetLayout lighting,
                const std::filesystem::path &shaders, bool narrowTemporal, bool anisotropy,
                const TreeOptions &);
    ~VulkanTrees();
    void prepare(uint32_t frame, const Camera &, TerrainGrid, const TemporalView &, const TreeSettings &,
                 VulkanTerrainStream *, bool eroded);
    double budgetStream(double milliseconds);
    VkBuffer canopyBuffer() const;
    void bounds(VulkanShadows &);
    void preparePyramid(VisibilityPyramidView);
    void updateAndSelectCasters(VkCommandBuffer, const ShadowUniform &, uint32_t views);
    void selectCamera(VkCommandBuffer, bool occlusion = false);
    void draw(VkCommandBuffer, VkDescriptorSet lighting, uint32_t pass, uint32_t view = 0);
    void finish(VkCommandBuffer);
    void submitted();
    const TreeStats &stats() const;
    void copySnapshot(VkCommandBuffer);
    TreeSnapshot snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

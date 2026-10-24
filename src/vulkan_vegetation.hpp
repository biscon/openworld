#pragma once
#include "temporal.hpp"
#include "vegetation.hpp"
#include "vulkan_materials.hpp"
#include "vulkan_shadows.hpp"
#include "vulkan_visibility.hpp"
namespace ow {
class VulkanTerrainStream;
class VulkanVegetation {
  public:
    VulkanVegetation(VkPhysicalDevice, VkDevice, uint32_t, VkDescriptorSetLayout, VulkanMaterials &,
                     const std::filesystem::path &, bool, bool, const VegetationOptions &);
    ~VulkanVegetation();
    void prepare(uint32_t, const Camera &, TerrainGrid, const TemporalView &, const VegetationSettings &);
    float streamingRadius() const;
    void prepareStream(VulkanTerrainStream *, const VegetationSettings &,
                       double terrainPreparationMilliseconds = 0);
    void budgetStream(double remaining);
    void bounds(VulkanShadows &);
    void preparePyramid(VisibilityPyramidView); // Before any command binds this
                                                // frame's descriptors.
    void updateAndSelectCasters(VkCommandBuffer, const ShadowUniform &, uint32_t);
    void selectCamera(VkCommandBuffer, VisibilityPyramidView = {});
    void draw(VkCommandBuffer, VkDescriptorSet, uint32_t pass, uint32_t view = 0);
    void finish(VkCommandBuffer);
    void submitted();
    const VegetationStats &stats() const;
    void copySnapshot(VkCommandBuffer);
    VegetationSnapshot snapshot() const; // caller has waited for GPU
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

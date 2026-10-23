#pragma once
#include "scene.hpp"
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
class VulkanClusters {
  public:
    VulkanClusters(VkPhysicalDevice, VkDevice, const SceneCapacity &, uint32_t queueFamily,
                   VkDescriptorSetLayout lighting, VkDescriptorSetLayout empty,
                   const std::filesystem::path &);
    ~VulkanClusters();
    ScenePool<LocalLight, LightHandle> lights;
    void resize(uint32_t width, uint32_t height); // GPU idle boundary.
    void prepare(uint32_t frame, const Camera &, TerrainGrid, VkDescriptorSet, LocalLightMode,
                 uint32_t diagnostic);
    void record(VkCommandBuffer, uint32_t frame, VkDescriptorSet);
    void timestamp(VkCommandBuffer, uint32_t frame, uint32_t point);
    const SceneStats &stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

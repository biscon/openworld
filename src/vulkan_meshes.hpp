#pragma once
#include "vulkan_materials.hpp"
#include "vulkan_shadows.hpp"
#include "vulkan_visibility.hpp"
#include <memory>
namespace ow {
class VulkanMeshes {
  public:
    VulkanMeshes(VkPhysicalDevice, VkDevice, const SceneCapacity &, VkDescriptorSetLayout lighting,
                 VulkanMaterials &, const std::filesystem::path &, bool bindless,
                 VkDescriptorSetLayout shadowLayout);
    ~VulkanMeshes();
    ScenePool<MeshInstance, InstanceHandle> instances;
    MeshHandle upload(const MeshData &);
    bool release(MeshHandle);
    void prepare(uint32_t frame, const Camera &, TerrainGrid);
    void uploads(VkCommandBuffer, uint32_t frame);
    void prepareVisibility(uint32_t frame, VulkanVisibility &);
    // pass: 0 regular opaque, 1 equal opaque, 2 depth only, 3 transparent.
    void draw(VkCommandBuffer, uint32_t frame, VkDescriptorSet lighting, uint32_t pass,
              VulkanVisibility *visibility = nullptr);
    void prepareShadows(uint32_t frame, VulkanShadows &);
    void drawShadows(VkCommandBuffer, uint32_t frame, VkDescriptorSet lighting, VulkanShadows &);
    uint64_t bytes() const;
    uint32_t skipped() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

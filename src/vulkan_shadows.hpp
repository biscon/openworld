#pragma once
#include "shadows.hpp"
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
struct ShadowCapabilities {
    bool depth = false, filtered = false, indirect = false, multi = false;
    uint32_t maxDraws = 1, maxResolution = 1;
};
ShadowCapabilities shadowCapabilities(VkPhysicalDevice);
class VulkanShadows {
  public:
    VulkanShadows(VkPhysicalDevice, VkDevice, uint32_t queueFamily, ShadowCapabilities, ShadowCapacity,
                  uint32_t externalCasterCapacity = 0);
    ~VulkanShadows();
    VkDescriptorSetLayout layout() const;
    void initialize(VkCommandBuffer);
    void begin(uint32_t frame, const Camera &, TerrainGrid, float aspect, glm::vec3 sun,
               const ShadowSettings &, bool disabled, float farDistance);
    uint32_t append(VisibilityBounds, uint32_t count, uint32_t first, uint32_t source,
                    const void *record = nullptr, uint32_t bytes = 0);
    void includeCasterBounds(VisibilityBounds); // bounds-only GPU-instanced casters, max 4096
    const ShadowUniform &uniform() const;       // finalized by prepare()
    void missing(VisibilityBounds);
    void prepare(double collectionMilliseconds = 0);
    bool active() const;
    uint32_t cascadeCount() const;
    void writeLighting(VkDescriptorSet);
    void beginRaster(VkCommandBuffer, uint32_t cascade);
    void endRaster(VkCommandBuffer);
    void bind(VkCommandBuffer, VkPipelineLayout);
    void drawRange(VkCommandBuffer, uint32_t first, uint32_t count);
    void finish(VkCommandBuffer);
    uint64_t snapshotBytes() const;
    void copySnapshot(VkCommandBuffer, VkBuffer) const;
    ShadowSnapshot decodeSnapshot(const void *) const;
    const ShadowStats &stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

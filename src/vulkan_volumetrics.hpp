#pragma once
#include "lighting.hpp"
#include "temporal.hpp"
#include "volumetrics.hpp"
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
class VulkanVolumetrics {
  public:
    VulkanVolumetrics(VkPhysicalDevice, VkDevice, uint32_t family, const VolumetricCapacity &,
                      uint32_t sceneLights, bool forceUnavailable, VkDescriptorSetLayout lighting,
                      const std::filesystem::path &);
    ~VulkanVolumetrics();
    ScenePool<FogVolume, FogVolumeHandle> volumes;
    void edited();
    void reset();
    bool supported() const;
    bool historyChanged(const VolumetricSettings &) const;
    bool needsResize(uint32_t, uint32_t, const VolumetricSettings &) const;
    void resize(uint32_t, uint32_t, const VolumetricSettings &); // Idle boundary.
    void invalidateSceneViews();
    void begin(uint32_t, const Camera &, TerrainGrid, const VolumetricSettings &, const LightingSettings &,
               const ScenePool<LocalLight, LightHandle> &, LocalLightMode, const TemporalView &, float,
               bool diagnostic, bool atmosphereChanged, VkDescriptorSet, VkImageView, VkImageView,
               glm::vec3 background);
    void record(VkCommandBuffer);
    void composite(VkCommandBuffer, uint32_t, uint32_t);
    void submitted();
    bool active() const;
    const VolumetricStats &stats() const;
    uint64_t snapshotBytes() const;
    void copySnapshot(VkCommandBuffer, VkBuffer);
    VolumetricSnapshot decodeSnapshot(const void *) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

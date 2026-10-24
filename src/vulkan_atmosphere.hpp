#pragma once
#include "atmosphere.hpp"
#include "camera.hpp"
#include "lighting.hpp"
#include "temporal.hpp"
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
class VulkanAtmosphere {
  public:
    VulkanAtmosphere(VkPhysicalDevice, VkDevice, uint32_t queueFamily, uint64_t budget,
                     bool forceUnavailable, const std::filesystem::path &, bool extended = false);
    ~VulkanAtmosphere();
    bool supported() const;
    void invalidateSceneViews(); // Caller recreated scene HDR/depth at an idle boundary.
    bool needsResize(const AtmosphereSettings &) const;
    void resize(const AtmosphereSettings &); // Device idle boundary.
    bool historyChanged(const AtmosphereSettings &, bool diagnostic) const;
    void begin(uint32_t frame, const Camera &, const AtmosphereSettings &, const LightingSettings &,
               const TemporalView &, float preExposure, bool diagnostic, float outerDistance,
               VkDescriptorSet lighting, VkImageView hdr, VkImageView depth, float localDistance = 0);
    void record(VkCommandBuffer);
    void composite(VkCommandBuffer, uint32_t width, uint32_t height, bool apply = true);
    void submitted();
    bool active() const;
    bool physical() const;
    bool aerial() const;
    const AtmosphereStats &stats() const;
    uint64_t snapshotBytes() const;
    void copySnapshot(VkCommandBuffer, VkBuffer);
    AtmosphereSnapshot decodeSnapshot(const void *) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

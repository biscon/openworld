#pragma once
#include "temporal.hpp"
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
// Renderer-owned spatial output filtering. Resize only when GPU users are idle.
class VulkanSpatialAA {
  public:
    VulkanSpatialAA(VkPhysicalDevice, VkDevice, uint32_t queueFamily, uint64_t budget,
                    const std::filesystem::path &shaders);
    ~VulkanSpatialAA();
    bool needsResize(uint32_t width, uint32_t height, AntialiasingMode) const;
    void resize(uint32_t width, uint32_t height, AntialiasingMode);
    void begin(uint32_t frame, bool diagnostic, const PostStats &temporal);
    void record(VkCommandBuffer, VkImageView hdr, VkImageView bloom, bool bloomActive, float bloomIntensity,
                uint32_t toneMapper);
    VkImageView result() const; // sRGB view: presentation samples linear color.
    const SpatialAaStats &stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

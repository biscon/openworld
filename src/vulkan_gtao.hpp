#pragma once
#include "gtao.hpp"
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
struct GtaoCapabilities {
    bool supported = false, compact = false;
};
GtaoCapabilities gtaoCapabilities(VkPhysicalDevice, bool forceUnavailable = false, bool forceWide = false);
class VulkanGtao {
  public:
    VulkanGtao(VkPhysicalDevice, VkDevice, uint32_t queueFamily, GtaoCapabilities, uint64_t budget,
               const std::filesystem::path &);
    ~VulkanGtao();
    // All submitted users must have finished. Reuses dimensions/mode when
    // unchanged.
    void resize(uint32_t width, uint32_t height, GtaoMode);
    bool needsResize(uint32_t width, uint32_t height, GtaoMode) const;
    // Frame fence must have signalled. Returns a descriptor-valid output or
    // permanent white fallback.
    VkImageView begin(uint32_t frame, const GtaoSettings &, bool wireframe);
    bool active() const;
    // Initialize permanent fallback once, using caller's startup immediate
    // command.
    void initialize(VkCommandBuffer);
    // Source depth must be in DEPTH_READ_ONLY_OPTIMAL; does not transition
    // caller-owned depth.
    void record(VkCommandBuffer, VkImageView sourceDepth);
    const GtaoStats &stats() const;
    VkDeviceSize snapshotBytes(uint32_t frame) const;
    void copySnapshot(VkCommandBuffer, uint32_t frame, VkBuffer) const;
    GtaoSnapshot decodeSnapshot(uint32_t frame, const void *) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

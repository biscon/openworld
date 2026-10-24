#pragma once
#include "visibility.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
// Borrowed for the current frame; the owner initializes and builds the pyramid.
struct VisibilityPyramidView {
    VkImageView image{};
    uint32_t width = 0, height = 0, levels = 0;
};
struct VisibilityCapabilities {
    bool firstInstance = false, multiDraw = false, count = false, hiZ = false;
    uint32_t maxDraws = 1;
};
// Query only. The renderer enables the returned optional features at device
// creation.
VisibilityCapabilities visibilityCapabilities(VkPhysicalDevice);
class VulkanVisibility {
  public:
    VulkanVisibility(VkPhysicalDevice, VkDevice, uint32_t queueFamily, uint32_t capacity,
                     VisibilityCapabilities, const std::filesystem::path &);
    ~VulkanVisibility();
    VkDescriptorSetLayout layout() const;
    void resize(uint32_t width, uint32_t height);
    void begin(uint32_t frame, VisibilityMode, bool wireframe, bool frustum, const glm::mat4 &);
    // newGroup prevents commands with different bound state sharing a batch.
    uint32_t append(VisibilityBounds, uint32_t indexCount, uint32_t firstIndex, uint32_t sourceRecord,
                    const void *terrainRecord, size_t recordBytes, bool newGroup);
    bool indirect() const;
    bool hiZ() const;
    VisibilityPyramidView pyramidView() const;
    void bindRecords(VkCommandBuffer, VkPipelineLayout) const;
    void beginGpu(VkCommandBuffer);
    void cull(VkCommandBuffer, bool occlusion);
    // Caller owns source depth transitions; requires DEPTH_READ_ONLY_OPTIMAL.
    void pyramid(VkCommandBuffer, VkImage depth, VkImageView);
    void draw(VkCommandBuffer, uint32_t batch) const;
    void stamp(VkCommandBuffer, uint32_t index);
    const VisibilityStats &stats() const;
    void copyPyramid(VkCommandBuffer, uint32_t frame, VkBuffer destination) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

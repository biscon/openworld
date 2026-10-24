#pragma once
#include "temporal.hpp"
#include <filesystem>
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
struct PostCapabilities {
    bool taa = false, bloom = false;
};
PostCapabilities postCapabilities(VkPhysicalDevice);
class VulkanPost {
  public:
    VulkanPost(VkPhysicalDevice, VkDevice, uint32_t queueFamily, PostCapabilities, uint64_t taaBudget,
               uint64_t bloomBudget, uint32_t terrainCapacity, const std::filesystem::path &);
    ~VulkanPost();
    bool needsResize(uint32_t, uint32_t, bool taa, bool bloom) const;
    void resize(uint32_t, uint32_t, bool taa, bool bloom); // Idle boundary.
    void begin(uint32_t frame, const Camera &, TerrainGrid, bool taa, const TaaSettings &,
               const BloomSettings &, float exposure, bool diagnostic);
    void reset(TemporalReset reason = TemporalReset::Explicit);
    void submitted();
    const TemporalView &view() const;
    const Camera &previousCamera() const;
    void bindLighting(VkDescriptorSet);
    void terrainRecord(uint32_t, const TemporalTerrainRecord &);
    void beginAttachments(VkCommandBuffer);
    std::array<VkRenderingAttachmentInfo, 2> attachments() const;
    void record(VkCommandBuffer, VkImageView hdr, VkImageView depth);
    VkImageView resolved(VkImageView raw) const;
    VkImageView bloomView(VkImageView fallback) const;
    const PostStats &stats() const;
    uint64_t snapshotBytes() const;
    void copySnapshot(VkCommandBuffer, VkBuffer) const;
    PostSnapshot decodeSnapshot(const void *) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

#pragma once
#include "material_assets.hpp"
#include "render_resources.hpp"
namespace ow {
// Tree-owned images keep the fixed material path independent of grass
// descriptor limits.
class TreeImage {
  public:
    VkDevice device{};
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    uint32_t width = 0, height = 0, levels = 0;
    uint64_t bytes = 0, budgetLimit = ~0ull;
    render_detail::Buffer staging;
    TreeImage() = default;
    TreeImage(const TreeImage &) = delete;
    TreeImage &operator=(const TreeImage &) = delete;
    ~TreeImage();
    void create(VkPhysicalDevice, VkDevice, uint32_t, uint32_t, uint32_t, VkFormat, VkImageUsageFlags,
                VkImageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT);
    void load(VkPhysicalDevice, VkDevice, const TextureData &);
    void upload(VkCommandBuffer);
    static VkFormat format(TextureFormat);
    VkFormat vkFormat{};
    std::vector<VkBufferImageCopy> copies;
};
void treeImageBarrier(VkCommandBuffer, VkImage, VkImageLayout, VkImageLayout, VkPipelineStageFlags2,
                      VkAccessFlags2, VkPipelineStageFlags2, VkAccessFlags2, uint32_t = 1,
                      VkImageAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT);
} // namespace ow

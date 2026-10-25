#include "tree_gpu_resources.hpp"
namespace ow {
using namespace render_detail;
TreeImage::~TreeImage() {
    if (view)
        vkDestroyImageView(device, view, nullptr);
    if (image)
        vkDestroyImage(device, image, nullptr);
    if (memory)
        vkFreeMemory(device, memory, nullptr);
}
VkFormat TreeImage::format(TextureFormat f) {
    switch (f) {
    case TextureFormat::Rgba8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Srgb8:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::Bc7:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::Bc7Srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case TextureFormat::Bc5:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureFormat::R16:
        return VK_FORMAT_R16_UNORM;
    }
    throw std::invalid_argument("Unsupported tree texture format");
}
void TreeImage::create(VkPhysicalDevice p, VkDevice d, uint32_t w, uint32_t h, uint32_t m, VkFormat f,
                       VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    device = d;
    width = w;
    height = h;
    levels = m;
    vkFormat = f;
    VkImageCreateInfo c{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    c.imageType = VK_IMAGE_TYPE_2D;
    c.format = f;
    c.extent = {w, h, 1};
    c.mipLevels = m;
    c.arrayLayers = 1;
    c.samples = VK_SAMPLE_COUNT_1_BIT;
    c.tiling = VK_IMAGE_TILING_OPTIMAL;
    c.usage = usage;
    check(vkCreateImage(d, &c, nullptr, &image));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(d, image, &r);
    bytes = r.size;
    if (bytes > budgetLimit)
        throw std::runtime_error("Tree image allocation exceeds budget");
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(p, &props);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((r.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    if (type == ~0u)
        throw std::runtime_error("Tree image memory unavailable");
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = r.size;
    a.memoryTypeIndex = type;
    check(vkAllocateMemory(d, &a, nullptr, &memory));
    check(vkBindImageMemory(d, image, memory, 0));
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = image;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = f;
    v.subresourceRange = {aspect, 0, m, 0, 1};
    check(vkCreateImageView(d, &v, nullptr, &view));
}
void TreeImage::load(VkPhysicalDevice p, VkDevice d, const TextureData &t) {
    if (t.bytes.size() != textureBytes(t, true))
        throw std::invalid_argument("Tree texture payload size mismatch");
    if (t.layers != 1)
        throw std::invalid_argument("Tree texture requires one layer");
    VkBufferCreateInfo tempInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    tempInfo.size = t.bytes.size();
    tempInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer temp;
    check(vkCreateBuffer(d, &tempInfo, nullptr, &temp));
    VkMemoryRequirements stage;
    vkGetBufferMemoryRequirements(d, temp, &stage);
    vkDestroyBuffer(d, temp, nullptr);
    if (stage.size > budgetLimit)
        throw std::runtime_error("Tree staging allocation exceeds budget");
    budgetLimit -= stage.size;
    create(p, d, t.width, t.height, t.levels, format(t.format),
           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    staging.create(p, d, t.bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, t.bytes.data(), t.bytes.size());
    uint64_t offset = 0;
    copies.resize(t.levels);
    for (uint32_t i = 0; i < t.levels; ++i) {
        auto &w = copies[i];
        w.bufferOffset = offset;
        w.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        w.imageExtent = {std::max(1u, t.width >> i), std::max(1u, t.height >> i), 1};
        offset += textureMipBytes(t.format, w.imageExtent.width, w.imageExtent.height);
    }
}
void treeImageBarrier(VkCommandBuffer c, VkImage image, VkImageLayout old, VkImageLayout next,
                      VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                      VkAccessFlags2 da, uint32_t m, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = sa;
    b.dstStageMask = dst;
    b.dstAccessMask = da;
    b.oldLayout = old;
    b.newLayout = next;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, m, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c, &dep);
}
void TreeImage::upload(VkCommandBuffer c) {
    treeImageBarrier(c, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, levels);
    vkCmdCopyBufferToImage(c, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(),
                           copies.data());
    treeImageBarrier(c, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, levels);
}
} // namespace ow

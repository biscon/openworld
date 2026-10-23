#pragma once
// Small Vulkan resource helpers shared by scene and visibility consumers.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>
namespace ow::render_detail {
inline void check(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Scene Vulkan error: " + std::to_string(r));
}
struct Buffer {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *mapped = nullptr;
    uint64_t bytes = 0, size = 0;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() { reset(); }
    void reset() {
        if (mapped)
            vkUnmapMemory(device, memory);
        if (buffer)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        mapped = nullptr;
        buffer = {};
        memory = {};
        bytes = size = 0;
    }
    void create(VkPhysicalDevice p, VkDevice d, uint64_t n, VkBufferUsageFlags usage, bool host = false) {
        reset();
        device = d;
        size = n;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = n;
        ci.usage = usage;
        check(vkCreateBuffer(d, &ci, nullptr, &buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(d, buffer, &req);
        bytes = req.size;
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(p, &props);
        VkMemoryPropertyFlags flags =
            host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        uint32_t index = ~0u;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
                index = i;
                break;
            }
        if (index == ~0u)
            throw std::runtime_error("Scene buffer memory unavailable");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = index;
        check(vkAllocateMemory(d, &a, nullptr, &memory));
        check(vkBindBufferMemory(d, buffer, memory, 0));
        if (host)
            check(vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    }
};
inline VkShaderModule shader(VkDevice d, const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Missing shader: " + path.string());
    auto n = f.tellg();
    if (n <= 0 || size_t(n) % 4)
        throw std::runtime_error("Invalid shader");
    std::vector<uint32_t> data(size_t(n) / 4);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(data.data()), n))
        throw std::runtime_error("Shader read failed");
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size_t(n);
    ci.pCode = data.data();
    VkShaderModule m;
    check(vkCreateShaderModule(d, &ci, nullptr, &m));
    return m;
}
struct ShaderModule {
    VkDevice device;
    VkShaderModule handle;
    ShaderModule(VkDevice d, const std::filesystem::path &path) : device(d), handle(shader(d, path)) {}
    ShaderModule(const ShaderModule &) = delete;
    ShaderModule &operator=(const ShaderModule &) = delete;
    ~ShaderModule() { vkDestroyShaderModule(device, handle, nullptr); }
};
inline void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = sa;
    b.dstStageMask = dst;
    b.dstAccessMask = da;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
inline void descriptor(VkDevice d, VkDescriptorSet set, uint32_t binding, const Buffer &b,
                       VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
    VkDescriptorBufferInfo bi{b.buffer, 0, b.size};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
}
} // namespace ow::render_detail

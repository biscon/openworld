#include "vulkan_visibility.hpp"
#include "render_resources.hpp"
#include <algorithm>
#include <cstring>
#include <vector>
namespace ow {
using namespace render_detail;
VisibilityCapabilities visibilityCapabilities(VkPhysicalDevice p) {
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &v12;
    vkGetPhysicalDeviceFeatures2(p, &f);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    VkFormatProperties depth, r32;
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_D32_SFLOAT, &depth);
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_R32_SFLOAT, &r32);
    const auto need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    return {bool(f.features.drawIndirectFirstInstance), bool(f.features.multiDrawIndirect),
            bool(v12.drawIndirectCount),
            bool(depth.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                (r32.optimalTilingFeatures & need) == need,
            props.limits.maxDrawIndirectCount};
}
namespace {
struct Candidate {
    glm::vec4 lo, hi;
    glm::uvec4 geometry;
};
static_assert(sizeof(Candidate) == 48);
struct Params {
    glm::mat4 vp;
    glm::uvec4 viewport, options;
};
void imageDependency(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout before,
                     VkImageLayout after, VkPipelineStageFlags2 src, VkAccessFlags2 sa,
                     VkPipelineStageFlags2 dst, VkAccessFlags2 da, uint32_t base = 0, uint32_t levels = 1) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = sa;
    b.dstStageMask = dst;
    b.dstAccessMask = da;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, base, levels, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace
struct VulkanVisibility::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t capacity, limit, width = 1, height = 1, levels = 1, current = 0;
    VisibilityCapabilities caps;
    VisibilityStats stats;
    Params params{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool pool{};
    VkPipeline cullPipeline{}, pyramidPipeline{};
    VkQueryPool queries{};
    double period = 0;
    uint32_t timestampBits = 0;
    struct Frame {
        Buffer candidates, records, batches, commands, counts;
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        uint64_t imageBytes = 0;
        std::vector<VkImageView> mips;
        std::vector<VkDescriptorSet> mipSets;
        VkDescriptorSet set{};
        uint32_t n = 0, b = 0, total = 0;
        bool issued = false;
        uint32_t stamps = 0;
    };
    std::array<Frame, 2> frames;
    Impl(VkPhysicalDevice p, VkDevice d, uint32_t c, VisibilityCapabilities capabilities)
        : physical(p), device(d), capacity(c), caps(capabilities) {
        limit = std::max(1u, std::min(128u, caps.maxDraws));
    }
    void destroyImages() {
        for (auto &f : frames) {
            for (auto v : f.mips)
                vkDestroyImageView(device, v, nullptr);
            f.mips.clear();
            if (f.view)
                vkDestroyImageView(device, f.view, nullptr);
            f.view = {};
            if (f.image)
                vkDestroyImage(device, f.image, nullptr);
            f.image = {};
            if (f.memory)
                vkFreeMemory(device, f.memory, nullptr);
            f.memory = {};
            f.imageBytes = 0;
        }
    }
    ~Impl() {
        destroyImages();
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (cullPipeline)
            vkDestroyPipeline(device, cullPipeline, nullptr);
        if (pyramidPipeline)
            vkDestroyPipeline(device, pyramidPipeline, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
    void buffers(VkDescriptorSet set, Frame &f) {
        descriptor(device, set, 0, f.records);
        descriptor(device, set, 1, f.candidates);
        descriptor(device, set, 2, f.batches);
        descriptor(device, set, 3, f.commands);
        descriptor(device, set, 4, f.counts);
    }
    void imageDescriptor(VkDescriptorSet set, uint32_t binding, VkImageView view, VkImageLayout layout,
                         VkDescriptorType type) {
        VkDescriptorImageInfo ii{{}, view, layout};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
    void collect(Frame &f) {
        stats.countsAvailable = f.issued;
        stats.timingsAvailable = false;
        stats.submitted = stats.frustumRejected = stats.occlusionRejected = 0;
        stats.cullingMilliseconds = stats.depthMilliseconds = stats.pyramidMilliseconds =
            stats.colourMilliseconds = 0;
        if (!f.stamps && !f.issued)
            return;
        const auto *counts = static_cast<const glm::uvec4 *>(f.counts.mapped);
        if (!f.issued)
            stats.submitted = f.total;
        if (f.issued)
            for (uint32_t i = 0; i < f.b; ++i) {
                stats.submitted += counts[i].x;
                stats.frustumRejected += counts[i].y;
                stats.occlusionRejected += counts[i].z;
            }
        if (queries) {
            uint64_t q[20]{};
            auto result =
                vkGetQueryPoolResults(device, queries, current * 10, 10, sizeof(q), q, 16,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (result != VK_SUCCESS && result != VK_NOT_READY)
                check(result);
            auto interval = [&](uint32_t a, uint32_t b) {
                if (!(f.stamps & (1u << a)) || !(f.stamps & (1u << b)) || !q[a * 2 + 1] || !q[b * 2 + 1])
                    return 0.;
                const auto mask = timestampBits == 64 ? ~uint64_t(0) : (uint64_t(1) << timestampBits) - 1;
                return double((q[b * 2] - q[a * 2]) & mask) * period / 1e6;
            };
            stats.cullingMilliseconds = interval(0, 1) + interval(6, 7);
            stats.depthMilliseconds = interval(2, 3);
            stats.pyramidMilliseconds = interval(4, 5);
            stats.colourMilliseconds = interval(8, 9);
            stats.timingsAvailable = q[17] && q[19];
        }
    }
};
VulkanVisibility::VulkanVisibility(VkPhysicalDevice p, VkDevice d, uint32_t family, uint32_t capacity,
                                   VisibilityCapabilities caps, const std::filesystem::path &shaders)
    : impl_(std::make_unique<Impl>(p, d, capacity, caps)) {
    auto &s = *impl_;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    if (!capacity || capacity > 1048576 || uint64_t(capacity) * 128 > props.limits.maxStorageBufferRange)
        throw std::invalid_argument("Visibility capacity exceeds storage limits");
    VkDescriptorSetLayoutBinding bindings[7]{};
    for (uint32_t i = 0; i < 7; ++i)
        bindings[i] = {i,
                       i < 5    ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                       : i == 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                       1,
                       VK_SHADER_STAGE_COMPUTE_BIT |
                           (i == 0 ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : 0u),
                       nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 7;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.setLayout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &s.pipelineLayout));
    auto pipeline = [&](const char *name, VkPipeline &out) {
        ShaderModule module(d, shaders / name);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module.handle;
        ci.stage.pName = "main";
        ci.layout = s.pipelineLayout;
        check(vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &out));
    };
    pipeline(caps.hiZ ? "visibility.comp.spv" : "visibility_frustum.comp.spv", s.cullPipeline);
    pipeline("visibility_pyramid.comp.spv", s.pyramidPipeline);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 170},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 34},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 34}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 34;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &pi, nullptr, &s.pool));
    for (auto &f : s.frames) {
        f.records.create(p, d, uint64_t(capacity) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.candidates.create(p, d, uint64_t(capacity) * sizeof(Candidate), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            true);
        f.batches.create(p, d, uint64_t(capacity) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.commands.create(p, d, uint64_t(capacity) * sizeof(VkDrawIndexedIndirectCommand),
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        f.counts.create(p, d, uint64_t(capacity) * 16,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        s.stats.allocatedBytes +=
            f.records.bytes + f.candidates.bytes + f.batches.bytes + f.commands.bytes + f.counts.bytes;
        f.mips.reserve(16);
        f.mipSets.resize(16);
        for (uint32_t i = 0; i < 17; ++i) {
            auto &set = i == 16 ? f.set : f.mipSets[i];
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s.pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &s.setLayout;
            check(vkAllocateDescriptorSets(d, &ai, &set));
            s.buffers(set, f);
        }
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, queues.data());
    s.timestampBits = queues.at(family).timestampValidBits;
    if (s.timestampBits && props.limits.timestampComputeAndGraphics) {
        s.period = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 20;
        check(vkCreateQueryPool(d, &qi, nullptr, &s.queries));
    }
}
VulkanVisibility::~VulkanVisibility() = default;
VkDescriptorSetLayout VulkanVisibility::layout() const { return impl_->setLayout; }
void VulkanVisibility::resize(uint32_t width, uint32_t height) {
    auto &s = *impl_;
    for (auto &f : s.frames)
        s.stats.allocatedBytes -= f.imageBytes;
    s.destroyImages();
    s.width = width;
    s.height = height;
    s.levels = 1;
    for (auto n = std::max(width, height); n > 1; n >>= 1)
        ++s.levels;
    if (!s.caps.hiZ)
        return;
    for (auto &f : s.frames) {
        f.issued = false;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_SFLOAT;
        ci.extent = {width, height, 1};
        ci.mipLevels = s.levels;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(s.device, &ci, nullptr, &f.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(s.device, f.image, &req);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(s.physical, &memory);
        uint32_t type = ~0u;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        if (type == ~0u)
            throw std::runtime_error("Visibility image memory unavailable");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        check(vkAllocateMemory(s.device, &alloc, nullptr, &f.memory));
        check(vkBindImageMemory(s.device, f.image, f.memory, 0));
        f.imageBytes = req.size;
        s.stats.allocatedBytes += req.size;
        auto view = [&](uint32_t base, uint32_t count) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = f.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R32_SFLOAT;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
            VkImageView result;
            check(vkCreateImageView(s.device, &vi, nullptr, &result));
            return result;
        };
        f.view = view(0, s.levels);
        for (uint32_t l = 0; l < s.levels; ++l)
            f.mips.push_back(view(l, 1));
        s.imageDescriptor(f.set, 5, f.view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        s.imageDescriptor(f.set, 6, f.mips[0], VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        for (uint32_t l = 0; l < s.levels; ++l) {
            s.imageDescriptor(f.mipSets[l], 6, f.mips[l], VK_IMAGE_LAYOUT_GENERAL,
                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            if (l)
                s.imageDescriptor(f.mipSets[l], 5, f.mips[l - 1], VK_IMAGE_LAYOUT_GENERAL,
                                  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        }
    }
}
void VulkanVisibility::begin(uint32_t frame, VisibilityMode mode, bool wire, bool frustum,
                             const glm::mat4 &vp) {
    auto &s = *impl_;
    s.current = frame;
    auto &f = s.frames[frame];
    s.collect(f);
    f.n = f.b = f.total = 0;
    f.issued = false;
    f.stamps = 0;
    s.stats.requested = mode;
    s.stats.effective = mode;
    s.stats.fallback = VisibilityFallback::None;
    if (wire) {
        s.stats.effective = VisibilityMode::Direct;
        s.stats.fallback = VisibilityFallback::Wireframe;
    } else if (mode != VisibilityMode::Direct && !s.caps.firstInstance) {
        s.stats.effective = VisibilityMode::Direct;
        s.stats.fallback = VisibilityFallback::UnsupportedIndirect;
    } else if (mode == VisibilityMode::HiZ && !s.caps.hiZ) {
        s.stats.effective = VisibilityMode::Indirect;
        s.stats.fallback = VisibilityFallback::UnsupportedHiZ;
    }
    s.params = {vp, {s.width, s.height, s.levels, frustum ? 1u : 0u}, {0, 0, 0, 0}};
    s.stats.candidates = s.stats.batches = 0;
}
uint32_t VulkanVisibility::append(VisibilityBounds b, uint32_t count, uint32_t first, uint32_t source,
                                  const void *record, size_t bytes, bool newGroup) {
    auto &s = *impl_;
    auto &f = s.frames[s.current];
    ++s.stats.candidates;
    ++f.total;
    if (f.n == s.capacity) {
        s.stats.effective = VisibilityMode::Direct;
        s.stats.fallback = VisibilityFallback::Capacity;
        return ~0u;
    }
    auto *batches = static_cast<glm::uvec4 *>(f.batches.mapped);
    if (!f.b || newGroup || batches[f.b - 1].y == s.limit)
        batches[f.b++] = {f.n, 0, 0, 0};
    ++batches[f.b - 1].y;
    // Also cover camera-relative terrain arithmetic at the far end of the view.
    const auto padding = glm::vec3(.01f) + glm::max(glm::abs(b.lo), glm::abs(b.hi)) * 1.9073486328125e-6f;
    b.lo -= padding;
    b.hi += padding;
    static_cast<Candidate *>(f.candidates.mapped)[f.n] = {
        glm::vec4(b.lo, b.valid ? 1.f : 0.f), glm::vec4(b.hi, 0), {count, first, record ? f.n : source, 0}};
    if (record) {
        if (bytes > 128)
            throw std::logic_error("Terrain draw record too large");
        auto *out = static_cast<char *>(f.records.mapped) + size_t(f.n) * 128;
        std::memset(out, 0, 128);
        std::memcpy(out, record, bytes);
    }
    ++f.n;
    s.stats.batches = f.b;
    return f.b - 1;
}
bool VulkanVisibility::indirect() const { return impl_->stats.effective != VisibilityMode::Direct; }
bool VulkanVisibility::hiZ() const { return impl_->stats.effective == VisibilityMode::HiZ; }
void VulkanVisibility::bindRecords(VkCommandBuffer cmd, VkPipelineLayout layout) const {
    const auto &s = *impl_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 1, &s.frames[s.current].set, 0,
                            nullptr);
}
void VulkanVisibility::beginGpu(VkCommandBuffer cmd) {
    auto &s = *impl_;
    if (s.queries)
        vkCmdResetQueryPool(cmd, s.queries, s.current * 10, 10);
    if (s.caps.hiZ) { // Valid initialized image descriptors even for the no-occlusion shader branch.
        auto &f = s.frames[s.current];
        imageDependency(cmd, f.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, 0, s.levels);
    }
}
void VulkanVisibility::stamp(VkCommandBuffer cmd, uint32_t index) {
    auto &s = *impl_;
    if (s.queries) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, s.current * 10 + index);
        s.frames[s.current].stamps |= 1u << index;
    }
}
void VulkanVisibility::cull(VkCommandBuffer cmd, bool occlusion) {
    if (!indirect())
        return;
    auto &s = *impl_;
    auto &f = s.frames[s.current];
    if (!f.b)
        return;
    stamp(cmd, occlusion ? 6 : 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.cullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout, 0, 1, &f.set, 0, nullptr);
    s.params.options.x = occlusion ? 1 : 0;
    for (uint32_t offset = 0; offset < f.b; offset += 65535) {
        s.params.options.y = offset;
        vkCmdPushConstants(cmd, s.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params), &s.params);
        vkCmdDispatch(cmd, std::min(65535u, f.b - offset), 1, 1);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
    f.issued = true;
    stamp(cmd, occlusion ? 7 : 1);
}
void VulkanVisibility::pyramid(VkCommandBuffer cmd, VkImage /*depth*/, VkImageView depthView) {
    auto &s = *impl_;
    auto &f = s.frames[s.current];
    stamp(cmd, 4);
    s.imageDescriptor(f.mipSets[0], 5, depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pyramidPipeline);
    uint32_t w = s.width, h = s.height;
    for (uint32_t l = 0; l < s.levels; ++l) {
        const uint32_t push[] = {w, h, l == 0 ? 1u : 0u, 0};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout, 0, 1, &f.mipSets[l], 0,
                                nullptr);
        vkCmdPushConstants(cmd, s.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        if (l) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        imageDependency(cmd, f.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT, l);
    }
    stamp(cmd, 5);
}
void VulkanVisibility::draw(VkCommandBuffer cmd, uint32_t b) const {
    const auto &s = *impl_;
    const auto &f = s.frames[s.current];
    auto batch = static_cast<const glm::uvec4 *>(f.batches.mapped)[b];
    const auto offset = VkDeviceSize(batch.x) * sizeof(VkDrawIndexedIndirectCommand);
    if (s.caps.count && s.caps.multiDraw)
        vkCmdDrawIndexedIndirectCount(cmd, f.commands.buffer, offset, f.counts.buffer, VkDeviceSize(b) * 16,
                                      batch.y, sizeof(VkDrawIndexedIndirectCommand));
    else if (s.caps.multiDraw)
        vkCmdDrawIndexedIndirect(cmd, f.commands.buffer, offset, batch.y,
                                 sizeof(VkDrawIndexedIndirectCommand));
    else
        for (uint32_t i = 0; i < batch.y; ++i)
            vkCmdDrawIndexedIndirect(cmd, f.commands.buffer,
                                     offset + VkDeviceSize(i) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                     sizeof(VkDrawIndexedIndirectCommand));
}
void VulkanVisibility::copyPyramid(VkCommandBuffer cmd, uint32_t frame, VkBuffer destination) const {
    const auto &s = *impl_;
    const auto &f = s.frames.at(frame);
    imageDependency(cmd, f.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, 0, s.levels);
    VkDeviceSize offset = 0;
    uint32_t w = s.width, h = s.height;
    for (uint32_t l = 0; l < s.levels; ++l) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, 1, &copy);
        offset += uint64_t(w) * h * 4;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    imageDependency(cmd, f.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, 0, s.levels);
}
const VisibilityStats &VulkanVisibility::stats() const { return impl_->stats; }
} // namespace ow

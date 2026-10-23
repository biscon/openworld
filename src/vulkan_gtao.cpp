#include "vulkan_gtao.hpp"
#include "camera.hpp"
#include "render_resources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
namespace ow {
using namespace render_detail;
namespace {
bool formatSupports(VkPhysicalDevice p, VkFormat f, VkFormatFeatureFlags flags) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(p, f, &props);
    return (props.optimalTilingFeatures & flags) == flags;
}
void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                VkAccessFlags2 da, uint32_t levels = 1, uint32_t base = 0) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = sa;
    b.dstStageMask = dst;
    b.dstAccessMask = da;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
void imageDescriptor(VkDevice device, VkDescriptorSet set, uint32_t binding, VkImageView view,
                     VkDescriptorType type, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) {
    VkDescriptorImageInfo info{{}, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
}
struct Image {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    std::array<VkImageView, 5> mips{};
    VkMemoryRequirements requirements{};
    uint32_t width = 0, height = 0, levels = 1, texelBytes = 4;
    VkFormat format{};
    Image() = default;
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    ~Image() { reset(); }
    void reset() {
        for (auto v : mips)
            if (v)
                vkDestroyImageView(device, v, nullptr);
        mips = {};
        if (view)
            vkDestroyImageView(device, view, nullptr);
        view = {};
        if (image)
            vkDestroyImage(device, image, nullptr);
        image = {};
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        memory = {};
        requirements = {};
    }
    void create(VkDevice d, uint32_t w, uint32_t h, VkFormat f, uint32_t count = 1,
                VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        reset();
        device = d;
        width = w;
        height = h;
        format = f;
        levels = count;
        texelBytes = (f == VK_FORMAT_R8_UNORM || f == VK_FORMAT_R8_UINT) ? 1 : 4;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = f;
        ci.extent = {w, h, 1};
        ci.mipLevels = count;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        check(vkCreateImage(d, &ci, nullptr, &image));
        vkGetImageMemoryRequirements(d, image, &requirements);
    }
    void allocate(VkPhysicalDevice p) {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(p, &props);
        uint32_t type = ~0u;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        if (type == ~0u)
            throw std::runtime_error("GTAO device memory unavailable");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = requirements.size;
        ai.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &ai, nullptr, &memory));
        check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &view));
        if (levels > 1)
            for (uint32_t l = 0; l < levels; ++l) {
                vi.subresourceRange.baseMipLevel = l;
                vi.subresourceRange.levelCount = 1;
                check(vkCreateImageView(device, &vi, nullptr, &mips[l]));
            }
    }
    VkImageView mip(uint32_t l) const { return levels > 1 ? mips[l] : view; }
    uint64_t payload() const { return uint64_t(width) * height * texelBytes; }
};
struct Push {
    glm::uvec4 dims, pass;
    glm::vec4 projection, effect;
};
static_assert(sizeof(Push) == 64);
uint64_t align4(uint64_t n) { return (n + 3) & ~uint64_t(3); }
} // namespace
GtaoCapabilities gtaoCapabilities(VkPhysicalDevice p, bool unavailable, bool wide) {
    const auto need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(p, &features);
    bool supported =
        !unavailable && formatSupports(p, VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        formatSupports(p, VK_FORMAT_R32_SFLOAT, need) && formatSupports(p, VK_FORMAT_R32_UINT, need) &&
        formatSupports(p, VK_FORMAT_R8G8B8A8_UNORM, need);
    bool compact = !wide && features.shaderStorageImageExtendedFormats &&
                   formatSupports(p, VK_FORMAT_R8_UNORM, need) &&
                   formatSupports(p, VK_FORMAT_R8_UINT, need);
    return {supported, compact};
}
struct VulkanGtao::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    GtaoCapabilities caps;
    uint64_t budget;
    uint32_t width = 0, height = 0, workWidth = 0, workHeight = 0, levels = 1, current = 0;
    GtaoMode configured = GtaoMode::Off;
    GtaoFallback allocationFallback = GtaoFallback::None;
    GtaoStats stats;
    GtaoSettings settings;
    Image white;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool pool{};
    std::array<VkPipeline, 5> pipelines{};
    VkQueryPool queries{};
    double period = 0;
    uint32_t timestampBits = 0;
    struct Frame {
        Image depth, raw, edges, filtered, offsets, result;
        std::array<VkDescriptorSet, 5> sets{};
        bool issued = false;
        GtaoMode mode = GtaoMode::Off;
        GtaoDiagnostic diagnostic = GtaoDiagnostic::Shaded;
        std::array<Image *, 6> images() { return {&depth, &raw, &edges, &filtered, &offsets, &result}; }
        std::array<const Image *, 6> snapshotImages() const {
            return {&result, &depth, &raw, &filtered, &edges, &offsets};
        }
    };
    std::array<Frame, 2> frames;
    Impl(VkPhysicalDevice p, VkDevice d, GtaoCapabilities c, uint64_t b)
        : physical(p), device(d), caps(c), budget(b) {}
    void destroyWorking() {
        for (auto &f : frames) {
            for (auto i : f.images())
                i->reset();
            f.issued = false;
            f.mode = GtaoMode::Off;
        }
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        pool = {};
        stats.allocatedBytes = white.requirements.size;
    }
    ~Impl() {
        destroyWorking();
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
    void collect(Frame &f) {
        stats.timingsAvailable = false;
        if (!queries || !f.issued)
            return;
        uint64_t t[8]{};
        if (vkGetQueryPoolResults(device, queries, current * 4, 4, sizeof(t), t, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                VK_SUCCESS &&
            t[1] && t[3] && t[5] && t[7]) {
            const uint64_t mask =
                timestampBits == 64 ? ~uint64_t(0) : (uint64_t(1) << timestampBits) - 1;
            stats.prepareMilliseconds = double((t[2] - t[0]) & mask) * period / 1e6;
            stats.evaluateMilliseconds = double((t[4] - t[2]) & mask) * period / 1e6;
            stats.filterMilliseconds = double((t[6] - t[4]) & mask) * period / 1e6;
            stats.completedMode = f.mode;
            stats.timingsAvailable = true;
        }
        f.issued = false;
    }
};
VulkanGtao::VulkanGtao(VkPhysicalDevice p, VkDevice d, uint32_t family, GtaoCapabilities caps,
                       uint64_t budget, const std::filesystem::path &shaders)
    : impl_(std::make_unique<Impl>(p, d, caps, budget)) {
    auto &s = *impl_;
    s.white.create(d, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    s.white.allocate(p);
    s.stats.allocatedBytes = s.white.requirements.size;
    s.stats.compact = caps.compact;
    if (!caps.supported)
        return;
    std::array<VkDescriptorSetLayoutBinding, 12> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, i < 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = bindings.size();
    li.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.setLayout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &s.pipelineLayout));
    const char *names[] = {"prepare", "reduce", "evaluate", "denoise", "resolve"};
    const char *entries[] = {"prepare", "reduceDepth", "evaluate", "denoise", "resolve"};
    for (uint32_t i = 0; i < 5; ++i) {
        ShaderModule module(d, shaders / (std::string("gtao_") + names[i] +
                                          (caps.compact ? "" : "_wide") + ".comp.spv"));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = s.pipelineLayout;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module.handle;
        ci.stage.pName = entries[i];
        check(vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &s.pipelines[i]));
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    uint32_t count;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, families.data());
    s.timestampBits = families.at(family).timestampValidBits;
    if (s.timestampBits && props.limits.timestampComputeAndGraphics) {
        s.period = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 8;
        check(vkCreateQueryPool(d, &qi, nullptr, &s.queries));
    }
}
VulkanGtao::~VulkanGtao() = default;
bool VulkanGtao::needsResize(uint32_t w, uint32_t h, GtaoMode mode) const {
    auto &s = *impl_;
    return s.width != w || s.height != h || s.configured != mode;
}
void VulkanGtao::resize(uint32_t w, uint32_t h, GtaoMode mode) {
    auto &s = *impl_;
    auto size = gtaoExtent(w, h, mode);
    if (!needsResize(w, h, mode))
        return;
    s.destroyWorking();
    s.width = w;
    s.height = h;
    s.configured = mode;
    s.workWidth = size.x;
    s.workHeight = size.y;
    s.stats.width = size.x;
    s.stats.height = size.y;
    s.stats.timingsAvailable = false;
    s.allocationFallback = GtaoFallback::None;
    if (!s.caps.supported) {
        s.allocationFallback = GtaoFallback::Unsupported;
        return;
    }
    if (mode == GtaoMode::Off)
        return;
    s.levels = 1;
    for (uint32_t n = std::max(size.x, size.y); n > 1 && s.levels < 5; n /= 2)
        ++s.levels;
    const VkFormat scalar = s.caps.compact ? VK_FORMAT_R8_UNORM : VK_FORMAT_R32_SFLOAT;
    const VkFormat integer = s.caps.compact ? VK_FORMAT_R8_UINT : VK_FORMAT_R32_UINT;
    uint64_t total = s.white.requirements.size;
    for (auto &f : s.frames) {
        f.depth.create(s.device, size.x, size.y, VK_FORMAT_R32_SFLOAT, s.levels);
        f.raw.create(s.device, size.x, size.y, scalar);
        f.edges.create(s.device, size.x, size.y, scalar);
        f.filtered.create(s.device, size.x, size.y, scalar);
        f.offsets.create(s.device, size.x, size.y, integer);
        f.result.create(s.device, w, h, VK_FORMAT_R8G8B8A8_UNORM);
        for (auto i : f.images())
            total += i->requirements.size;
    }
    if (total > s.budget) {
        s.destroyWorking();
        s.allocationFallback = GtaoFallback::Budget;
        return;
    }
    for (auto &f : s.frames)
        for (auto i : f.images())
            i->allocate(s.physical);
    s.stats.allocatedBytes = total;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 60},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 60}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 10;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(s.device, &pi, nullptr, &s.pool));
    for (auto &f : s.frames)
        for (uint32_t l = 0; l < s.levels; ++l) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s.pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &s.setLayout;
            check(vkAllocateDescriptorSets(s.device, &ai, &f.sets[l]));
            VkImageView views[] = {s.white.view,    f.depth.view,    f.raw.view,     f.edges.view,
                                   f.filtered.view, f.offsets.view,  f.depth.mip(l), f.raw.view,
                                   f.edges.view,    f.filtered.view, f.offsets.view, f.result.view};
            for (uint32_t b = 0; b < 12; ++b)
                imageDescriptor(s.device, f.sets[l], b, views[b],
                                b < 6 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        }
}
VkImageView VulkanGtao::begin(uint32_t frame, const GtaoSettings &settings, bool wireframe) {
    validateGtao(settings);
    auto &s = *impl_;
    s.current = frame;
    s.settings = settings;
    auto &f = s.frames.at(frame);
    s.collect(f);
    s.stats.requested = settings.mode;
    s.stats.effective = settings.mode;
    s.stats.fallback = GtaoFallback::None;
    if (settings.mode != GtaoMode::Off) {
        s.stats.fallback = wireframe ? GtaoFallback::Wireframe : s.allocationFallback;
        if (s.stats.fallback != GtaoFallback::None)
            s.stats.effective = GtaoMode::Off;
    }
    f.mode = s.stats.effective;
    f.diagnostic = settings.diagnostic;
    return active() ? f.result.view : s.white.view;
}
bool VulkanGtao::active() const { return impl_->stats.effective != GtaoMode::Off; }
void VulkanGtao::initialize(VkCommandBuffer cmd) {
    auto &s = *impl_;
    transition(cmd, s.white.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue colour{{1, 1, 1, 1}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, s.white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
    transition(cmd, s.white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
void VulkanGtao::record(VkCommandBuffer cmd, VkImageView source, glm::vec2 jitter,
                        uint32_t temporalPhase) {
    auto &s = *impl_;
    if (!active())
        return;
    auto &f = s.frames[s.current];
    for (uint32_t l = 0; l < s.levels; ++l)
        imageDescriptor(s.device, f.sets[l], 0, source, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    for (auto i : f.images())
        transition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, i->levels);
    const float ty = std::tan(glm::radians(Camera::verticalFovDegrees) * .5f);
    Push push{{s.width, s.height, s.workWidth, s.workHeight},
              {s.configured == GtaoMode::Half ? 2u : 1u, 0, s.levels, uint32_t(s.settings.diagnostic)},
              {ty * float(s.width) / s.height, ty,
               Camera::nearPlane * Camera::farPlane / (Camera::farPlane - Camera::nearPlane),
               Camera::nearPlane / (Camera::farPlane - Camera::nearPlane)},
              {s.settings.radius, jitter.x, jitter.y, float(temporalPhase)}};
    auto stamp = [&](uint32_t index) {
        if (s.queries)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                 s.current * 4 + index);
    };
    auto dispatch = [&](uint32_t pipeline, uint32_t level, uint32_t w, uint32_t h) {
        push.pass.y = level;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelines[pipeline]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout, 0, 1,
                                &f.sets[level], 0, nullptr);
        vkCmdPushConstants(cmd, s.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    };
    auto computeBarrier = [&]() {
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    };
    if (s.queries)
        vkCmdResetQueryPool(cmd, s.queries, s.current * 4, 4);
    stamp(0);
    dispatch(0, 0, s.workWidth, s.workHeight);
    computeBarrier();
    for (uint32_t l = 1; l < s.levels; ++l) {
        dispatch(1, l, std::max(1u, s.workWidth >> l), std::max(1u, s.workHeight >> l));
        computeBarrier();
    }
    stamp(1);
    dispatch(2, 0, s.workWidth, s.workHeight);
    computeBarrier();
    stamp(2);
    dispatch(3, 0, s.workWidth, s.workHeight);
    computeBarrier();
    dispatch(4, 0, s.width, s.height);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    stamp(3);
    f.issued = true;
}
const GtaoStats &VulkanGtao::stats() const { return impl_->stats; }
VkDeviceSize VulkanGtao::snapshotBytes(uint32_t frame) const {
    const auto &f = impl_->frames.at(frame);
    if (f.mode == GtaoMode::Off)
        throw std::runtime_error("GTAO snapshot requires an active GTAO frame");
    uint64_t bytes = 0;
    for (auto i : f.snapshotImages())
        bytes += align4(i->payload());
    return bytes;
}
void VulkanGtao::copySnapshot(VkCommandBuffer cmd, uint32_t frame, VkBuffer dst) const {
    const auto &f = impl_->frames.at(frame);
    VkDeviceSize offset = 0;
    for (auto i : f.snapshotImages()) {
        transition(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {i->width, i->height, 1};
        vkCmdCopyImageToBuffer(cmd, i->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &copy);
        offset += align4(i->payload());
        transition(cmd, i->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
}
GtaoSnapshot VulkanGtao::decodeSnapshot(uint32_t frame, const void *data) const {
    const auto &s = *impl_;
    const auto &f = s.frames.at(frame);
    GtaoSnapshot out;
    out.width = s.width;
    out.height = s.height;
    out.workWidth = s.workWidth;
    out.workHeight = s.workHeight;
    out.mode = f.mode;
    out.diagnostic = f.diagnostic;
    const auto *bytes = static_cast<const uint8_t *>(data);
    out.pixels.resize(size_t(s.width) * s.height);
    for (size_t i = 0; i < out.pixels.size(); ++i)
        out.pixels[i] =
            glm::vec4(bytes[4 * i], bytes[4 * i + 1], bytes[4 * i + 2], bytes[4 * i + 3]) / 255.f;
    bytes += align4(f.result.payload());
    std::vector<float> *arrays[] = {&out.depth, &out.raw, &out.filtered, &out.edges};
    const Image *images[] = {&f.depth, &f.raw, &f.filtered, &f.edges};
    for (uint32_t a = 0; a < 4; ++a) {
        auto &v = *arrays[a];
        v.resize(size_t(s.workWidth) * s.workHeight);
        if (images[a]->texelBytes == 1)
            for (size_t i = 0; i < v.size(); ++i)
                v[i] = bytes[i] / 255.f;
        else
            std::memcpy(v.data(), bytes, v.size() * 4);
        bytes += align4(images[a]->payload());
    }
    out.offsets.resize(size_t(s.workWidth) * s.workHeight);
    if (s.caps.compact)
        for (size_t i = 0; i < out.offsets.size(); ++i)
            out.offsets[i] = bytes[i];
    else
        std::memcpy(out.offsets.data(), bytes, out.offsets.size() * 4);
    return out;
}
} // namespace ow

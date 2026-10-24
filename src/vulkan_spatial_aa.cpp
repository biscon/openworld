#include "vulkan_spatial_aa.hpp"
#include "../third_party/smaa/AreaTex.h"
#include "../third_party/smaa/SearchTex.h"
#include "render_resources.hpp"
#include <array>
#include <cstring>
namespace ow {
using namespace render_detail;
namespace {
constexpr VkFormat rgba = VK_FORMAT_R8G8B8A8_UNORM, srgb = VK_FORMAT_R8G8B8A8_SRGB;
bool spatial(AntialiasingMode m) { return m == AntialiasingMode::Fxaa || m == AntialiasingMode::Smaa1x; }
struct Image {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{}, linearView{};
    VkMemoryRequirements req{};
    VkFormat format{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D extent{};
    Image() = default;
    Image(const Image &) = delete;
    ~Image() { reset(); }
    void reset() {
        if (linearView)
            vkDestroyImageView(device, linearView, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        image = {};
        memory = {};
        view = {};
        linearView = {};
        req = {};
        layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    void create(VkDevice d, uint32_t w, uint32_t h, VkFormat f, bool lookup = false) {
        device = d;
        extent = {w, h};
        format = f;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = f == rgba ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = f;
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                   (lookup ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        check(vkCreateImage(d, &ci, nullptr, &image));
        vkGetImageMemoryRequirements(d, image, &req);
    }
    void allocate(VkPhysicalDevice p) {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(p, &props);
        uint32_t type = ~0u;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        if (type == ~0u)
            throw std::runtime_error("Spatial AA device memory unavailable");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &ai, nullptr, &memory));
        check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &view));
        if (format == rgba) {
            vi.format = srgb;
            check(vkCreateImageView(device, &vi, nullptr, &linearView));
        }
    }
    void transition(VkCommandBuffer cmd, VkImageLayout next) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = layout;
        b.newLayout = next;
        b.image = image;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        layout = next;
    }
};
struct Push {
    glm::vec4 metrics;
    uint32_t mapper;
    float bloom;
    uint32_t unused[2]{};
};
} // namespace
struct VulkanSpatialAA::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    uint64_t budget;
    std::filesystem::path shaders;
    uint32_t width = 0, height = 0, current = 0;
    AntialiasingMode requested = AntialiasingMode::Off;
    PostFallback allocation = PostFallback::Disabled;
    SpatialAaStats stats;
    bool fxaaSupported = false, smaaSupported = false;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool pool{};
    VkSampler linear{}, point{};
    std::array<VkPipeline, 5> pipelines{};
    VkQueryPool queries{};
    uint32_t timestampBits = 0;
    double period = 0;
    struct Frame {
        Image source, result, edges, weights;
        std::array<VkDescriptorSet, 4> sets{};
        bool timed = false;
    };
    std::array<Frame, 2> frames;
    Image area, search;
    Buffer upload;
    Impl(VkPhysicalDevice p, VkDevice d, uint64_t b, const std::filesystem::path &s)
        : physical(p), device(d), budget(b), shaders(s) {}
    ~Impl() {
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (linear)
            vkDestroySampler(device, linear, nullptr);
        if (point)
            vkDestroySampler(device, point, nullptr);
    }
    void clear() {
        for (auto &f : frames) {
            f.source.reset();
            f.result.reset();
            f.edges.reset();
            f.weights.reset();
            f.timed = false;
        }
        area.reset();
        search.reset();
        upload.reset();
        stats.bytes = 0;
    }
    bool supports(VkFormat f, bool target, bool mutableFormat = false) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical, f, &props);
        VkFormatFeatureFlags flags =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            (target ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
        if ((props.optimalTilingFeatures & flags) != flags)
            return false;
        VkImageFormatProperties ip;
        return vkGetPhysicalDeviceImageFormatProperties(
                   physical, f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_SAMPLED_BIT |
                       (target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                   mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, &ip) == VK_SUCCESS;
    }
    void pipeline(uint32_t index, const char *file, VkFormat format) {
        if (pipelines[index])
            return;
        ShaderModule vs(device, shaders / "fullscreen.vert.spv"), ps(device, shaders / file);
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &s : stages) {
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs.handle;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps.handle;
        VkPipelineVertexInputStateCreateInfo vertex{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multi{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamics;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vertex;
        ci.pInputAssemblyState = &assembly;
        ci.pViewportState = &viewport;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &multi;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dynamic;
        ci.layout = pipelineLayout;
        check(vkCreateGraphicsPipelines(device, {}, 1, &ci, nullptr, &pipelines[index]));
    }
    void descriptors(VkDescriptorSet set, VkImageView input, VkImageLayout inputLayout, VkImageView aux,
                     VkImageLayout auxLayout) {
        VkDescriptorImageInfo infos[] = {
            {{}, input, inputLayout},
            {{}, aux, auxLayout},
            {{}, area.view ? area.view : input, area.view ? area.layout : inputLayout},
            {{}, search.view ? search.view : input, search.view ? search.layout : inputLayout},
            {linear, {}, {}},
            {point, {}, {}}};
        VkWriteDescriptorSet writes[6]{};
        for (uint32_t i = 0; i < 6; ++i) {
            auto &w = writes[i];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            w.pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
    }
    void draw(VkCommandBuffer cmd, Image &target, bool encodedTarget, uint32_t pipelineIndex,
              VkDescriptorSet set, const Push &push) {
        target.transition(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = encodedTarget ? target.linearView : target.view;
        color.imageLayout = target.layout;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        VkViewport viewport{0, 0, float(width), float(height), 0, 1};
        VkRect2D scissor{{0, 0}, {width, height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[pipelineIndex]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        target.transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
};
VulkanSpatialAA::VulkanSpatialAA(VkPhysicalDevice p, VkDevice d, uint32_t family, uint64_t budget,
                                 const std::filesystem::path &shaders)
    : impl_(std::make_unique<Impl>(p, d, budget, shaders)) {
    auto &s = *impl_;
    s.fxaaSupported = s.supports(rgba, true, true) && s.supports(srgb, true, true);
    s.smaaSupported = s.fxaaSupported && s.supports(VK_FORMAT_R8G8_UNORM, true) &&
                      s.supports(VK_FORMAT_R8G8_UNORM, false) && s.supports(VK_FORMAT_R8_UNORM, false);
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (uint32_t i = 0; i < 6; ++i)
        bindings[i] = {i, i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.setLayout));
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &s.setLayout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &pi, nullptr, &s.pipelineLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 8;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &pool, nullptr, &s.pool));
    std::array<VkDescriptorSetLayout, 4> layouts;
    layouts.fill(s.setLayout);
    for (auto &f : s.frames) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.pool;
        ai.descriptorSetCount = 4;
        ai.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(d, &ai, f.sets.data()));
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(d, &sampler, nullptr, &s.linear));
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    check(vkCreateSampler(d, &sampler, nullptr, &s.point));
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, families.data());
    s.timestampBits = families.at(family).timestampValidBits;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    s.period = props.limits.timestampPeriod;
    if (s.timestampBits) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;
        check(vkCreateQueryPool(d, &qi, nullptr, &s.queries));
    }
}
VulkanSpatialAA::~VulkanSpatialAA() = default;
bool VulkanSpatialAA::needsResize(uint32_t w, uint32_t h, AntialiasingMode mode) const {
    auto &s = *impl_;
    return s.width != w || s.height != h || s.requested != mode;
}
void VulkanSpatialAA::resize(uint32_t w, uint32_t h, AntialiasingMode mode) {
    auto &s = *impl_;
    s.clear();
    s.width = w;
    s.height = h;
    s.requested = mode;
    s.stats = {};
    s.stats.requested = mode;
    s.allocation = PostFallback::Disabled;
    if (!spatial(mode))
        return;
    if (!(mode == AntialiasingMode::Fxaa ? s.fxaaSupported : s.smaaSupported)) {
        s.allocation = PostFallback::Unsupported;
        return;
    }
    uint64_t bytes = 0;
    for (auto &f : s.frames) {
        f.source.create(s.device, w, h, rgba);
        f.result.create(s.device, w, h, rgba);
        bytes += f.source.req.size + f.result.req.size;
        if (mode == AntialiasingMode::Smaa1x) {
            f.edges.create(s.device, w, h, VK_FORMAT_R8G8_UNORM);
            f.weights.create(s.device, w, h, rgba);
            bytes += f.edges.req.size + f.weights.req.size;
        }
    }
    if (mode == AntialiasingMode::Smaa1x) {
        s.area.create(s.device, AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM, true);
        s.search.create(s.device, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM, true);
        bytes += s.area.req.size + s.search.req.size;
        // Upload buffer remains alive until the next idle reconfiguration.
        if (bytes + AREATEX_SIZE + SEARCHTEX_SIZE <= s.budget) {
            s.upload.create(s.physical, s.device, AREATEX_SIZE + SEARCHTEX_SIZE,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            bytes += s.upload.bytes;
        } else
            bytes = s.budget + 1;
    }
    if (bytes > s.budget) {
        s.clear();
        s.allocation = PostFallback::Budget;
        return;
    }
    for (auto &f : s.frames) {
        f.source.allocate(s.physical);
        f.result.allocate(s.physical);
        if (mode == AntialiasingMode::Smaa1x) {
            f.edges.allocate(s.physical);
            f.weights.allocate(s.physical);
        }
    }
    s.pipeline(0, "spatial_prepare.frag.spv", srgb);
    if (mode == AntialiasingMode::Fxaa)
        s.pipeline(1, "fxaa.frag.spv", rgba);
    else {
        s.area.allocate(s.physical);
        s.search.allocate(s.physical);
        std::memcpy(s.upload.mapped, areaTexBytes, AREATEX_SIZE);
        std::memcpy(static_cast<char *>(s.upload.mapped) + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
        s.pipeline(2, "smaa_edges.frag.spv", VK_FORMAT_R8G8_UNORM);
        s.pipeline(3, "smaa_weights.frag.spv", rgba);
        s.pipeline(4, "smaa_blend.frag.spv", srgb);
    }
    s.stats.bytes = bytes;
    s.allocation = PostFallback::None;
}
void VulkanSpatialAA::begin(uint32_t frame, bool diagnostic, const PostStats &temporal) {
    auto &s = *impl_;
    s.current = frame;
    s.stats.requested = s.requested;
    s.stats.fallback = diagnostic ? PostFallback::Diagnostic : s.allocation;
    s.stats.effective = spatial(s.requested) && s.stats.fallback == PostFallback::None ? s.requested
                        : s.requested == AntialiasingMode::Taa && temporal.taa ? AntialiasingMode::Taa
                                                                               : AntialiasingMode::Off;
    if (s.requested == AntialiasingMode::Taa && !diagnostic)
        s.stats.fallback = temporal.taa ? PostFallback::None : temporal.taaFallback;
    s.stats.timingsAvailable = false;
    s.stats.milliseconds = 0;
    auto &f = s.frames[frame];
    if (s.queries && f.timed) {
        uint64_t t[4]{};
        if (vkGetQueryPoolResults(s.device, s.queries, frame * 2, 2, sizeof(t), t, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                VK_SUCCESS &&
            t[1] && t[3]) {
            uint64_t mask = s.timestampBits == 64 ? ~uint64_t(0) : (uint64_t(1) << s.timestampBits) - 1;
            s.stats.milliseconds = double((t[2] - t[0]) & mask) * s.period / 1e6;
            s.stats.timingsAvailable = true;
        }
    }
    f.timed = false;
}
void VulkanSpatialAA::record(VkCommandBuffer cmd, VkImageView hdr, VkImageView bloom, bool bloomActive,
                             float bloomIntensity, uint32_t mapper) {
    auto &s = *impl_;
    if (!spatial(s.stats.effective))
        return;
    auto &f = s.frames[s.current];
    if (s.area.image && s.area.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        uint64_t offset = 0;
        for (auto *image : {&s.area, &s.search}) {
            image->transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {image->extent.width, image->extent.height, 1};
            vkCmdCopyBufferToImage(cmd, s.upload.buffer, image->image, image->layout, 1, &copy);
            image->transition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            offset = AREATEX_SIZE;
        }
    }
    if (s.queries) {
        vkCmdResetQueryPool(cmd, s.queries, s.current * 2, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, s.current * 2);
        f.timed = true;
    }
    Push push{{1.f / s.width, 1.f / s.height, float(s.width), float(s.height)},
              mapper,
              bloomActive ? bloomIntensity : 0,
              {}};
    s.descriptors(f.sets[0], hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, bloom,
                  bloomActive ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s.draw(cmd, f.source, true, 0, f.sets[0], push);
    s.descriptors(f.sets[1], f.source.view, f.source.layout, f.source.view, f.source.layout);
    if (s.stats.effective == AntialiasingMode::Fxaa)
        s.draw(cmd, f.result, false, 1, f.sets[1], push);
    else {
        s.draw(cmd, f.edges, false, 2, f.sets[1], push);
        s.descriptors(f.sets[2], f.edges.view, f.edges.layout, f.source.view, f.source.layout);
        s.draw(cmd, f.weights, false, 3, f.sets[2], push);
        s.descriptors(f.sets[3], f.source.linearView, f.source.layout, f.weights.view, f.weights.layout);
        s.draw(cmd, f.result, true, 4, f.sets[3], push);
    }
    if (s.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, s.current * 2 + 1);
}
VkImageView VulkanSpatialAA::result() const {
    auto &s = *impl_;
    return spatial(s.stats.effective) ? s.frames[s.current].result.linearView : VK_NULL_HANDLE;
}
const SpatialAaStats &VulkanSpatialAA::stats() const { return impl_->stats; }
} // namespace ow

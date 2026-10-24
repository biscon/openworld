#include "vulkan_post.hpp"
#include "render_resources.hpp"
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
namespace ow {
using namespace render_detail;
namespace {
constexpr auto hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
struct Image {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkMemoryRequirements req{};
    VkFormat format{};
    uint32_t width = 0, height = 0, texel = 0;
    bool initialized = false;
    ~Image() { reset(); }
    void reset() {
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        view = {};
        image = {};
        memory = {};
        req = {};
        initialized = false;
    }
    void create(VkDevice d, uint32_t w, uint32_t h, VkFormat f, bool attachment = false) {
        reset();
        device = d;
        width = w;
        height = h;
        format = f;
        texel = f == hdrFormat ? 8 : f == VK_FORMAT_R8_UNORM ? 1 : 4;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = f;
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   (attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
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
            throw std::runtime_error("Post-process memory unavailable");
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
    }
    // Keep every packed snapshot region aligned for the largest texel (RGBA16F).
    uint64_t payload() const { return (uint64_t(width) * height * texel + 7) & ~uint64_t(7); }
};
void transition(VkCommandBuffer cmd, Image &i, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = i.initialized ? from : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = to;
    b.image = i.image;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    i.initialized = true;
}
void imageDescriptor(VkDevice d, VkDescriptorSet set, uint32_t binding, VkImageView view,
                     VkImageLayout layout, VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
    VkDescriptorImageInfo info{{}, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
}
struct Push {
    glm::uvec4 dims, pass;
    glm::vec4 effect, extra;
};
} // namespace
PostCapabilities postCapabilities(VkPhysicalDevice p) {
    auto has = [&](VkFormat f, VkFormatFeatureFlags flags) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(p, f, &fp);
        return (fp.optimalTilingFeatures & flags) == flags;
    };
    auto sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    bool bloom = has(hdrFormat, sampled | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(p, &features);
    bool taa = bloom && features.independentBlend &&
               has(hdrFormat, sampled | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
               has(VK_FORMAT_R8_UNORM, sampled | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) &&
               has(VK_FORMAT_R32_SFLOAT, sampled | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
               has(VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    return {taa, bloom};
}
struct VulkanPost::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    PostCapabilities caps;
    uint64_t taaBudget, bloomBudget;
    uint32_t capacity;
    PostFallback taaAllocation = PostFallback::Disabled, bloomAllocation = PostFallback::Disabled;
    uint32_t width = 0, height = 0, current = 0, last = 0;
    bool requestedTaa = false, requestedBloom = false, haveHistory = false;
    uint64_t sequence = 0;
    float previousExposure = 1, currentExposure = 1;
    Camera previousCamera{}, pendingCamera{};
    TerrainGrid previousGrid{};
    TemporalView view;
    PostStats stats;
    TemporalReset pendingReset = TemporalReset::Initial;
    TaaSettings taa;
    BloomSettings bloom;
    WorldPosition previousOrigin;
    VkDescriptorSetLayout layout{};
    VkPipelineLayout pipelineLayout{};
    VkDescriptorPool pool{};
    VkSampler sampler{};
    VkPipeline taaPipeline{}, downPipeline{}, upPipeline{};
    VkQueryPool queries{};
    double period = 0;
    uint32_t timestampBits = 0;
    struct TaaFrame {
        Image motion, reactive, history, depth;
    };
    struct BloomFrame {
        std::array<Image, 6> down, up;
    };
    struct Frame {
        TaaFrame taa;
        BloomFrame bloom;
        Buffer view, terrain;
        VkDescriptorSet resolve{};
        std::array<VkDescriptorSet, 6> down{}, up{};
        bool timed = false, completedTaa = false, completedBloom = false;
        float exposure = 1;
    };
    std::array<Frame, 2> frames;
    uint32_t levels = 0;
    Impl(VkPhysicalDevice p, VkDevice d, PostCapabilities c, uint64_t t, uint64_t b, uint32_t n)
        : physical(p), device(d), caps(c), taaBudget(t), bloomBudget(b), capacity(n) {}
    ~Impl() {
        for (auto p : {taaPipeline, downPipeline, upPipeline})
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (layout)
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
    void dispatch(VkCommandBuffer cmd, VkPipeline p, VkDescriptorSet set, const Push &push) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (push.dims.x + 7) / 8, (push.dims.y + 7) / 8, 1);
    }
    void common(VkDescriptorSet set, Frame &f) {
        descriptor(device, set, 9, f.view, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        VkDescriptorImageInfo si{sampler, {}, {}};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 8;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &si;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
};
VulkanPost::VulkanPost(VkPhysicalDevice p, VkDevice d, uint32_t family, PostCapabilities caps,
                       uint64_t tb, uint64_t bb, uint32_t capacity, const std::filesystem::path &shaders)
    : impl_(std::make_unique<Impl>(p, d, caps, tb, bb, capacity)) {
    auto &s = *impl_;
    std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
    for (uint32_t i = 0; i < 10; ++i)
        bindings[i] = {i,
                       i < 6    ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                       : i < 8  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                       : i == 8 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = bindings.size();
    li.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.layout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &s.layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(d, &pi, nullptr, &s.pipelineLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 156},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 52},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 26},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 26}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 26;
    pool.poolSizeCount = 4;
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &pool, nullptr, &s.pool));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(d, &si, nullptr, &s.sampler));
    for (auto &f : s.frames) {
        f.view.create(p, d, sizeof(TemporalView), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f.terrain.create(p, d, sizeof(TemporalTerrainRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        auto alloc = [&](VkDescriptorSet &set) {
            VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool = s.pool;
            a.descriptorSetCount = 1;
            a.pSetLayouts = &s.layout;
            check(vkAllocateDescriptorSets(d, &a, &set));
            s.common(set, f);
        };
        alloc(f.resolve);
        for (uint32_t l = 0; l < 6; ++l) {
            alloc(f.down[l]);
            alloc(f.up[l]);
        }
    }
    auto pipeline = [&](const char *name, VkPipeline &out) {
        ShaderModule shader(d, shaders / name);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = shader.handle;
        ci.stage.pName = "main";
        ci.layout = s.pipelineLayout;
        check(vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &out));
    };
    if (caps.taa)
        pipeline("taa.comp.spv", s.taaPipeline);
    if (caps.bloom) {
        pipeline("bloom_down.comp.spv", s.downPipeline);
        pipeline("bloom_up.comp.spv", s.upPipeline);
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    if (uint64_t(capacity) * sizeof(TemporalTerrainRecord) > props.limits.maxStorageBufferRange)
        s.caps.taa = false;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &n, families.data());
    s.timestampBits = families.at(family).timestampValidBits;
    if (s.timestampBits && props.limits.timestampComputeAndGraphics) {
        s.period = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 28;
        check(vkCreateQueryPool(d, &qi, nullptr, &s.queries));
    }
}
VulkanPost::~VulkanPost() = default;
bool VulkanPost::needsResize(uint32_t w, uint32_t h, bool t, bool b) const {
    auto &s = *impl_;
    return w != s.width || h != s.height || t != s.requestedTaa || b != s.requestedBloom;
}
void VulkanPost::resize(uint32_t w, uint32_t h, bool t, bool b) {
    auto &s = *impl_;
    s.width = w;
    s.height = h;
    s.requestedTaa = t;
    s.requestedBloom = b;
    s.haveHistory = false;
    s.pendingReset = TemporalReset::Extent;
    s.sequence = 0;
    s.stats.taaBytes = s.stats.bloomBytes = 0;
    s.taaAllocation = !t            ? PostFallback::Disabled
                      : !s.caps.taa ? PostFallback::Unsupported
                                    : PostFallback::None;
    s.bloomAllocation = !b              ? PostFallback::Disabled
                        : !s.caps.bloom ? PostFallback::Unsupported
                                        : PostFallback::None;
    for (auto &f : s.frames) {
        f.timed = false;
        for (auto *i : {&f.taa.motion, &f.taa.reactive, &f.taa.history, &f.taa.depth})
            i->reset();
        for (auto &i : f.bloom.down)
            i.reset();
        for (auto &i : f.bloom.up)
            i.reset();
    }
    // Descriptor-valid minimal records remain when disabled. Measure the full
    // buffer before allocating it, just like the images.
    uint64_t recordBytes = 0;
    for (auto &f : s.frames)
        f.terrain.create(s.physical, s.device, sizeof(TemporalTerrainRecord),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (t && s.caps.taa) {
        VkBuffer sizing{};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = uint64_t(s.capacity) * sizeof(TemporalTerrainRecord);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(s.device, &bi, nullptr, &sizing));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(s.device, sizing, &req);
        recordBytes = req.size;
        vkDestroyBuffer(s.device, sizing, nullptr);
        uint64_t bytes = 0;
        for (auto &f : s.frames) {
            f.taa.motion.create(s.device, w, h, hdrFormat, true);
            f.taa.reactive.create(s.device, w, h, VK_FORMAT_R8_UNORM, true);
            f.taa.history.create(s.device, w, h, hdrFormat);
            f.taa.depth.create(s.device, w, h, VK_FORMAT_R32_SFLOAT);
            bytes += f.view.bytes + recordBytes;
            for (auto *i : {&f.taa.motion, &f.taa.reactive, &f.taa.history, &f.taa.depth})
                bytes += i->req.size;
        }
        if (bytes > s.taaBudget) {
            s.taaAllocation = PostFallback::Budget;
            for (auto &f : s.frames)
                for (auto *i : {&f.taa.motion, &f.taa.reactive, &f.taa.history, &f.taa.depth})
                    i->reset();
        } else {
            for (auto &f : s.frames) {
                f.terrain.create(s.physical, s.device,
                                 uint64_t(s.capacity) * sizeof(TemporalTerrainRecord),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
                for (auto *i : {&f.taa.motion, &f.taa.reactive, &f.taa.history, &f.taa.depth})
                    i->allocate(s.physical);
            }
            s.stats.taaBytes = bytes;
        }
    }
    if (!s.stats.taaBytes)
        for (auto &f : s.frames)
            s.stats.taaBytes += f.view.bytes + f.terrain.bytes;
    auto sizes = bloomExtents(w, h);
    s.levels = uint32_t(sizes.size());
    s.stats.bloomLevels = s.levels;
    if (b && s.caps.bloom) {
        uint64_t bytes = 0;
        for (auto &f : s.frames)
            for (uint32_t l = 0; l < s.levels; ++l)
                for (auto *i : {&f.bloom.down[l], &f.bloom.up[l]}) {
                    i->create(s.device, sizes[l].x, sizes[l].y, hdrFormat);
                    bytes += i->req.size;
                }
        if (bytes > s.bloomBudget) {
            s.bloomAllocation = PostFallback::Budget;
            for (auto &f : s.frames) {
                for (auto &i : f.bloom.down)
                    i.reset();
                for (auto &i : f.bloom.up)
                    i.reset();
            }
        } else {
            for (auto &f : s.frames)
                for (uint32_t l = 0; l < s.levels; ++l)
                    for (auto *i : {&f.bloom.down[l], &f.bloom.up[l]})
                        i->allocate(s.physical);
            s.stats.bloomBytes = bytes;
        }
    }
}
void VulkanPost::reset(TemporalReset r) {
    impl_->pendingReset = r;
    impl_->haveHistory = false;
    impl_->sequence = 0;
}
void VulkanPost::begin(uint32_t frame, const Camera &camera, TerrainGrid grid, const TaaSettings &t,
                       const BloomSettings &b, float exposure, bool diagnostic) {
    auto &s = *impl_;
    s.stats.taaFallback = diagnostic ? PostFallback::Diagnostic : s.taaAllocation;
    s.stats.bloomFallback = diagnostic ? PostFallback::Diagnostic : s.bloomAllocation;
    s.current = frame;
    s.taa = t;
    s.bloom = b;
    s.currentExposure = exposure;
    s.pendingCamera = camera;
    s.stats.taa = t.enabled && s.frames[frame].taa.history.view && !diagnostic;
    s.stats.bloom = b.intensity > 0 && s.frames[frame].bloom.down[0].view && !diagnostic;
    if (s.stats.taa != bool(s.view.state.x))
        reset(TemporalReset::Settings);
    const auto origin = worldPosition(camera, grid);
    auto delta = relativePosition(origin, s.previousOrigin);
    if (s.haveHistory &&
        (glm::length(delta) > 256 ||
         glm::dot(camera.forward(), s.previousCamera.forward()) < std::cos(glm::radians(45.f))))
        reset(TemporalReset::CameraCut);
    if (s.haveHistory && (camera.curvature.x != s.previousCamera.curvature.x ||
                          camera.curvature.z != s.previousCamera.curvature.z ||
                          camera.renderFar != s.previousCamera.renderFar))
        reset(TemporalReset::Settings);
    s.view = {};
    s.view.curvature = camera.curvature;
    s.view.previousCurvature = s.haveHistory ? s.previousCamera.curvature : camera.curvature;
    s.view.current = camera.viewProjection(float(s.width) / s.height);
    s.view.previous =
        s.haveHistory ? s.previousCamera.viewProjection(float(s.width) / s.height) : s.view.current;
    s.view.originDelta = glm::vec4(glm::vec3(delta), camera.renderFar);
    auto jitter = s.stats.taa ? temporalJitter(s.sequence) : glm::vec2(0);
    s.view.raster = jitterProjection(s.view.current, jitter, {s.width, s.height});
    s.view.pixels = {s.width, s.height, jitter.x, jitter.y};
    s.view.state = {s.stats.taa ? 1.f : 0.f, s.haveHistory ? 1.f : 0.f, exposure / s.previousExposure,
                    float(s.capacity)};
    s.stats.historyValid = s.stats.taa && s.haveHistory;
    s.stats.sequence = uint32_t(s.sequence % 8);
    s.stats.reset = s.pendingReset;
    s.stats.rejectedSurfaces = s.stats.trackingOverflow = 0;
    auto &f = s.frames[frame];
    f.exposure = exposure;
    std::memcpy(f.view.mapped, &s.view, sizeof(s.view));
    s.stats.timingsAvailable = false;
    if (s.queries && f.timed) {
        uint64_t values[28]{};
        if (vkGetQueryPoolResults(s.device, s.queries, frame * 14, 14, sizeof(values), values, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
            VK_SUCCESS) {
            bool available = true;
            for (uint32_t i = 0; i < 14; ++i)
                available &= values[i * 2 + 1] != 0;
            if (available) {
                uint64_t mask =
                    s.timestampBits == 64 ? ~uint64_t(0) : (uint64_t(1) << s.timestampBits) - 1;
                auto elapsed = [&](uint32_t a, uint32_t b) {
                    return ((values[b * 2] - values[a * 2]) & mask) * s.period / 1e6;
                };
                s.stats.taaMilliseconds = f.completedTaa ? elapsed(0, 1) : 0;
                s.stats.bloomMilliseconds = f.completedBloom ? elapsed(1, 13) : 0;
                s.stats.bloomDownMilliseconds = {};
                s.stats.bloomUpMilliseconds = {};
                if (f.completedBloom)
                    for (uint32_t l = 0; l < s.levels; ++l) {
                        s.stats.bloomDownMilliseconds[l] = elapsed(1 + l, 2 + l);
                        uint32_t k = s.levels - 1 - l;
                        s.stats.bloomUpMilliseconds[l] = elapsed(7 + k, 8 + k);
                    }
                s.stats.timingsAvailable = true;
            }
        }
    }
    s.previousGrid = grid;
}
void VulkanPost::submitted() {
    auto &s = *impl_;
    s.last = s.current;
    s.previousCamera = s.pendingCamera;
    s.previousOrigin = worldPosition(s.pendingCamera, s.previousGrid);
    s.previousExposure = s.currentExposure;
    s.haveHistory = s.stats.taa;
    s.pendingReset = TemporalReset::None;
    if (s.stats.taa)
        ++s.sequence;
}
const TemporalView &VulkanPost::view() const { return impl_->view; }
const Camera &VulkanPost::previousCamera() const { return impl_->previousCamera; }
void VulkanPost::bindLighting(VkDescriptorSet set) {
    auto &s = *impl_;
    auto &f = s.frames[s.current];
    descriptor(s.device, set, 14, f.view, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    descriptor(s.device, set, 15, f.terrain);
}
void VulkanPost::terrainRecord(uint32_t index, const TemporalTerrainRecord &record) {
    auto &s = *impl_;
    if (index >= s.capacity) {
        ++s.stats.trackingOverflow;
        return;
    }
    static_cast<TemporalTerrainRecord *>(s.frames[s.current].terrain.mapped)[index] = record;
    if (record.valid.x == 0)
        ++s.stats.rejectedSurfaces;
}
void VulkanPost::beginAttachments(VkCommandBuffer cmd) {
    auto &s = *impl_;
    if (s.stats.taa)
        for (auto *i : {&s.frames[s.current].taa.motion, &s.frames[s.current].taa.reactive})
            transition(cmd, *i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
std::array<VkRenderingAttachmentInfo, 2> VulkanPost::attachments() const {
    auto &s = *impl_;
    auto &f = s.frames[s.current];
    std::array<VkRenderingAttachmentInfo, 2> a{};
    for (auto &v : a) {
        v.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        v.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        v.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        v.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (s.stats.taa) {
        a[0].imageView = f.taa.motion.view;
        a[1].imageView = f.taa.reactive.view;
    }
    return a;
}
void VulkanPost::record(VkCommandBuffer cmd, VkImageView hdr, VkImageView depth) {
    auto &s = *impl_;
    auto &f = s.frames[s.current];
    auto &previous = s.frames[1 - s.current];
    f.completedTaa = s.stats.taa;
    f.completedBloom = s.stats.bloom;
    if (s.queries) {
        vkCmdResetQueryPool(cmd, s.queries, s.current * 14, 14);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, s.current * 14);
        f.timed = true;
    }
    if (s.stats.taa) {
        for (auto *i : {&f.taa.motion, &f.taa.reactive})
            transition(cmd, *i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        for (auto *i : {&f.taa.history, &f.taa.depth, &previous.taa.history, &previous.taa.depth})
            transition(cmd, *i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        imageDescriptor(s.device, f.resolve, 0, hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        imageDescriptor(s.device, f.resolve, 1, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        imageDescriptor(s.device, f.resolve, 2, f.taa.motion.view, VK_IMAGE_LAYOUT_GENERAL);
        imageDescriptor(s.device, f.resolve, 3, f.taa.reactive.view, VK_IMAGE_LAYOUT_GENERAL);
        imageDescriptor(s.device, f.resolve, 4, previous.taa.history.view, VK_IMAGE_LAYOUT_GENERAL);
        imageDescriptor(s.device, f.resolve, 5, previous.taa.depth.view, VK_IMAGE_LAYOUT_GENERAL);
        imageDescriptor(s.device, f.resolve, 6, f.taa.history.view, VK_IMAGE_LAYOUT_GENERAL,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        imageDescriptor(s.device, f.resolve, 7, f.taa.depth.view, VK_IMAGE_LAYOUT_GENERAL,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        Push push{};
        push.dims = {s.width, s.height, 0, 0};
        push.effect.x = s.taa.historyWeight;
        s.dispatch(cmd, s.taaPipeline, f.resolve, push);
        transition(cmd, f.taa.history, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        transition(cmd, f.taa.depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (s.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, s.current * 14 + 1);
    if (s.stats.bloom) {
        for (uint32_t l = 0; l < s.levels; ++l) {
            auto &out = f.bloom.down[l];
            transition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            auto input = l ? f.bloom.down[l - 1].view : s.stats.taa ? f.taa.history.view : hdr;
            auto layout =
                l || s.stats.taa ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageDescriptor(s.device, f.down[l], 0, input, layout);
            imageDescriptor(s.device, f.down[l], 6, out.view, VK_IMAGE_LAYOUT_GENERAL,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            Push push{};
            push.dims = {out.width, out.height, l ? f.bloom.down[l - 1].width : s.width,
                         l ? f.bloom.down[l - 1].height : s.height};
            push.pass.x = l;
            push.effect = {s.bloom.threshold * s.currentExposure, s.bloom.knee, s.currentExposure,
                           s.bloom.radius};
            s.dispatch(cmd, s.downPipeline, f.down[l], push);
            transition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            if (s.queries)
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                     s.current * 14 + 2 + l);
        }
        if (s.queries)
            for (uint32_t l = s.levels; l < 6; ++l)
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                     s.current * 14 + 2 + l);
        for (int l = int(s.levels) - 1; l >= 0; --l) {
            auto &out = f.bloom.up[l];
            transition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            imageDescriptor(s.device, f.up[l], 0, f.bloom.down[l].view, VK_IMAGE_LAYOUT_GENERAL);
            imageDescriptor(s.device, f.up[l], 1,
                            l + 1 < int(s.levels) ? f.bloom.up[l + 1].view : f.bloom.down[l].view,
                            VK_IMAGE_LAYOUT_GENERAL);
            imageDescriptor(s.device, f.up[l], 6, out.view, VK_IMAGE_LAYOUT_GENERAL,
                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            Push push{};
            push.dims = {out.width, out.height, 0, 0};
            push.pass.x = l + 1 == int(s.levels) ? 1 : 0;
            push.effect.w = s.bloom.radius;
            s.dispatch(cmd, s.upPipeline, f.up[l], push);
            transition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            if (s.queries)
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                     s.current * 14 + 8 + s.levels - 1 - l);
        }
        if (s.queries)
            for (uint32_t k = s.levels; k < 6; ++k)
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                     s.current * 14 + 8 + k);
    } else if (s.queries) {
        for (uint32_t q = 2; q < 14; ++q)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                 s.current * 14 + q);
    }
}
VkImageView VulkanPost::resolved(VkImageView raw) const {
    auto &s = *impl_;
    return s.stats.taa ? s.frames[s.current].taa.history.view : raw;
}
VkImageView VulkanPost::bloomView(VkImageView fallback) const {
    auto &s = *impl_;
    return s.stats.bloom ? s.frames[s.current].bloom.up[0].view : fallback;
}
const PostStats &VulkanPost::stats() const { return impl_->stats; }
uint64_t VulkanPost::snapshotBytes() const {
    auto &s = *impl_;
    auto &f = s.frames[s.last];
    uint64_t n = 0;
    if (f.completedTaa)
        for (auto *i : {&f.taa.history, &f.taa.motion, &f.taa.reactive, &f.taa.depth})
            n += i->payload();
    if (f.completedBloom)
        for (uint32_t l = 0; l < s.levels; ++l)
            n += f.bloom.up[l].payload();
    return n;
}
void VulkanPost::copySnapshot(VkCommandBuffer cmd, VkBuffer target) const {
    auto &s = *impl_;
    auto &f = s.frames[s.last];
    uint64_t offset = 0;
    auto copy = [&](Image &i) {
        transition(cmd, i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {i.width, i.height, 1};
        vkCmdCopyImageToBuffer(cmd, i.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &c);
        transition(cmd, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        offset += i.payload();
    };
    if (f.completedTaa)
        for (auto *i : {&f.taa.history, &f.taa.motion, &f.taa.reactive, &f.taa.depth})
            copy(*i);
    if (f.completedBloom)
        for (uint32_t l = 0; l < s.levels; ++l)
            copy(f.bloom.up[l]);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
PostSnapshot VulkanPost::decodeSnapshot(const void *data) const {
    auto &s = *impl_;
    auto &f = s.frames[s.last];
    PostSnapshot out;
    out.width = s.width;
    out.height = s.height;
    out.preExposure = f.exposure;
    auto *p = static_cast<const uint8_t *>(data);
    auto rgba = [&](const Image &i) {
        std::vector<glm::vec4> pixels(size_t(i.width) * i.height);
        for (size_t n = 0; n < pixels.size(); ++n) {
            uint32_t xy, zw;
            std::memcpy(&xy, p + n * 8, 4);
            std::memcpy(&zw, p + n * 8 + 4, 4);
            pixels[n] = glm::vec4(glm::unpackHalf2x16(xy), glm::unpackHalf2x16(zw));
        }
        p += i.payload();
        return pixels;
    };
    if (f.completedTaa) {
        out.resolved = rgba(f.taa.history);
        out.motion = rgba(f.taa.motion);
        if (static_cast<const TemporalView *>(f.view.mapped)->curvature.z != 0)
            for (auto &motion : out.motion)
                motion.z *= 1000; // Public snapshots remain metres.
        out.reactivity.resize(size_t(s.width) * s.height);
        for (size_t i = 0; i < out.reactivity.size(); ++i)
            out.reactivity[i] = p[i] / 255.f;
        p += f.taa.reactive.payload();
        out.depth.resize(out.reactivity.size());
        std::memcpy(out.depth.data(), p, out.depth.size() * 4);
        p += f.taa.depth.payload();
    }
    if (f.completedBloom)
        for (uint32_t l = 0; l < s.levels; ++l) {
            out.bloom.push_back(rgba(f.bloom.up[l]));
            out.bloomSizes.push_back({f.bloom.up[l].width, f.bloom.up[l].height});
        }
    return out;
}
} // namespace ow

#include "vulkan_atmosphere.hpp"
#include "render_resources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
namespace ow {
using namespace render_detail;
namespace {
struct Image {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{}, cubeBase{};
    std::array<VkImageView, 7> mips{};
    VkMemoryRequirements req{};
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1;
    ~Image() { reset(); }
    void reset() {
        for (auto v : mips)
            if (v)
                vkDestroyImageView(device, v, nullptr);
        mips = {};
        if (cubeBase)
            vkDestroyImageView(device, cubeBase, nullptr);
        cubeBase = {};
        if (view)
            vkDestroyImageView(device, view, nullptr);
        view = {};
        if (image)
            vkDestroyImage(device, image, nullptr);
        image = {};
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        memory = {};
        req = {};
    }
    void create(VkDevice d, uint32_t w, uint32_t h, uint32_t z, bool cube, bool storage,
                bool volume = false) {
        reset();
        device = d;
        width = w;
        height = h;
        depth = z;
        layers = cube ? 6 : 1;
        levels = cube && w > 1 ? 7 : 1;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {w, h, z};
        ci.mipLevels = levels;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (storage)
            ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        check(vkCreateImage(d, &ci, nullptr, &image));
        vkGetImageMemoryRequirements(d, image, &req);
    }
    void allocate(VkPhysicalDevice p, bool volume = false) {
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
            throw std::runtime_error("Atmosphere device memory unavailable");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &a, nullptr, &memory));
        check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        v.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE
                     : volume    ? VK_IMAGE_VIEW_TYPE_3D
                                 : VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        check(vkCreateImageView(device, &v, nullptr, &view));
        if (layers == 6) {
            v.subresourceRange.levelCount = 1;
            check(vkCreateImageView(device, &v, nullptr, &cubeBase));
            v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            for (uint32_t i = 0; i < levels; ++i) {
                v.subresourceRange.baseMipLevel = i;
                check(vkCreateImageView(device, &v, nullptr, &mips[i]));
            }
        }
    }
    uint64_t payload() const { return uint64_t(width) * height * depth * 8; }
};
void transition(VkCommandBuffer cmd, const Image &i, VkImageLayout before, VkImageLayout after) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i.levels, 0, i.layers};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
void imageDescriptor(VkDevice d, VkDescriptorSet set, uint32_t binding, VkImageView view,
                     VkDescriptorType type, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) {
    VkDescriptorImageInfo i{{}, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &i;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
}
uint64_t opticalKey(AtmosphereSettings s, bool ground) {
    // Extinction is independent of datum, phase function and the MS comparison toggle.
    s.datum = 0;
    s.mieG = 0;
    s.multipleScattering = false;
    if (!ground)
        s.groundAlbedo = 0;
    return atmosphereProfileKey(s);
}
void dependency(VkCommandBuffer cmd) {
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
} // namespace
struct VulkanAtmosphere::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    uint64_t budget;
    bool caps = false, storage = false, allocated = false;
    AtmosphereQuality quality = AtmosphereQuality::Normal;
    AtmosphereMode requested = AtmosphereMode::Off;
    AtmosphereFallback allocationFallback = AtmosphereFallback::None;
    AtmosphereStats stats;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkDescriptorPool pool{};
    std::array<VkPipeline, 7> pipelines{};
    VkSampler sampler{};
    VkQueryPool queries{};
    double period = 0;
    uint32_t current = 0, last = 0;
    bool recorded = false;
    uint64_t lastProfile = 0;
    AtmosphereMode lastMode = AtmosphereMode::Off;
    bool lastAerial = true;
    glm::vec3 lastSun{0, 1, 0};
    float lastAltitude = 0, lastIntensity = 0, lastHaze = 0;
    AtmosphereSettings settings;
    AtmosphereUniform uniform;
    struct Frame {
        Image trans, multi, sky, t, s, env;
        Buffer constants, sh;
        std::array<VkDescriptorSet, 13> sets{};
        uint64_t profile = 0, optical = 0, multiple = 0;
        float altitude = -1e9f, envAltitude = -1e9f, haze = -1, outer = 0;
        glm::vec3 sun{0}, envSun{0};
        bool initialized = false, timed = false, descriptorsValid = false;
        bool dirtyProfile = false, dirtyOptical = false, dirtyMultiple = false, dirtySky = false,
             dirtyEnv = false;
        std::array<Image *, 6> images() { return {&trans, &multi, &sky, &t, &s, &env}; }
    };
    std::array<Frame, 2> frames;
    bool extended = false;
    Impl(VkPhysicalDevice p, VkDevice d, uint64_t b) : physical(p), device(d), budget(b) {}
    ~Impl() {
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        for (auto pipeline : pipelines)
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
    }
    void init(uint32_t family, bool force, const std::filesystem::path &shaders) {
        VkFormatProperties f;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &f);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        storage = (f.optimalTilingFeatures & need) == need;
        VkFormatProperties depth;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT, &depth);
        caps = storage && (depth.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
               !force;
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        s.magFilter = s.minFilter = VK_FILTER_LINEAR;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.maxLod = 6;
        check(vkCreateSampler(device, &s, nullptr, &sampler));
        for (auto &frame : frames) {
            frame.constants.create(physical, device, sizeof(AtmosphereUniform),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
            frame.sh.create(physical, device, 9 * sizeof(glm::vec4),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        if (!caps)
            return;
        std::array<VkDescriptorSetLayoutBinding, 15> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            auto &v = bindings[i];
            v.binding = i + 16;
            v.descriptorCount = 1;
            v.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            v.descriptorType = i == 0              ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                               : i == 6            ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                               : i == 7            ? VK_DESCRIPTOR_TYPE_SAMPLER
                               : i >= 9 && i <= 13 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                   : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = bindings.size();
        sl.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &setLayout));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
        const char *names[] = {"transmittance", "multiple",   "sky",      "aerial",
                               "environment",   "irradiance", "composite"};
        for (uint32_t j = 0; j < 7; ++j) {
            ShaderModule module(device,
                                shaders / (std::string("atmosphere_") + names[j] + ".comp.spv"));
            VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cp.layout = layout;
            cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cp.stage.module = module.handle;
            cp.stage.pName = "main";
            check(vkCreateComputePipelines(device, {}, 1, &cp, nullptr, &pipelines[j]));
        }
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 26},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 26},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 26},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 26 * 7},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 26 * 5}};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pc.maxSets = 26;
        pc.poolSizeCount = 5;
        pc.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pc, nullptr, &pool));
        for (auto &frame : frames)
            for (auto &set : frame.sets) {
                VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                a.descriptorPool = pool;
                a.descriptorSetCount = 1;
                a.pSetLayouts = &setLayout;
                check(vkAllocateDescriptorSets(device, &a, &set));
            }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        if (families[family].timestampValidBits && props.limits.timestampComputeAndGraphics) {
            period = props.limits.timestampPeriod;
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            q.queryType = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = 12;
            check(vkCreateQueryPool(device, &q, nullptr, &queries));
        }
    }
    void bind(Frame &f, VkDescriptorSet set, bool compute) {
        descriptor(device, set, 16, f.constants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        descriptor(device, set, 22, f.sh);
        Image *images[] = {&f.trans, &f.multi, &f.sky, &f.t, &f.s};
        for (uint32_t i = 0; i < 5; ++i)
            imageDescriptor(device, set, 17 + i, images[i]->view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        imageDescriptor(device, set, 24, compute ? f.env.cubeBase : f.env.view,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        VkDescriptorImageInfo info{sampler, {}, VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 23;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }
    void makeImages(bool full) {
        uint32_t n = full ? (quality == AtmosphereQuality::High ? 64 : 32) : 1,
                 w = full ? (quality == AtmosphereQuality::High ? 384 : 192) : 1,
                 h = full ? (quality == AtmosphereQuality::High ? 216 : 108) : 1;
        for (auto &f : frames) {
            f.trans.create(device, full ? 256 : 1, full ? 64 : 1, 1, false, storage);
            f.multi.create(device, full ? 32 : 1, full ? 32 : 1, 1, false, storage);
            f.sky.create(device, w, h, 1, false, storage);
            f.t.create(device, n, n, n + (full && extended ? 32 : 0), false, storage, true);
            f.s.create(device, n, n, n + (full && extended ? 32 : 0), false, storage, true);
            f.env.create(device, full ? 64 : 1, full ? 64 : 1, 1, true, storage);
            f.initialized = false;
            f.descriptorsValid = false;
            f.profile = f.optical = f.multiple = 0;
            f.altitude = f.envAltitude = -1e9f;
            f.timed = false;
        }
    }
    void resize(const AtmosphereSettings &s) {
        quality = s.quality;
        requested = s.mode;
        allocated = caps && requested == AtmosphereMode::Physical;
        allocationFallback = !caps ? AtmosphereFallback::Unsupported : AtmosphereFallback::None;
        makeImages(allocated);
        uint64_t bytes = 0;
        for (auto &f : frames) {
            bytes += f.constants.bytes + f.sh.bytes;
            for (auto i : f.images())
                bytes += i->req.size;
        }
        if (allocated && bytes > budget) {
            allocated = false;
            allocationFallback = AtmosphereFallback::Budget;
            makeImages(false);
        }
        stats.bytes = 0;
        for (auto &f : frames) {
            for (auto i : f.images()) {
                i->allocate(physical, i == &f.t || i == &f.s);
                stats.bytes += i->req.size;
            }
            stats.bytes += f.constants.bytes + f.sh.bytes;
        }
        // Descriptor-valid fallback metadata/images are retained even below the
        // minimum budget.
    }
    void dispatch(VkCommandBuffer cmd, uint32_t set, uint32_t pass, uint32_t w, uint32_t h,
                  uint32_t z = 1, uint32_t mip = 0) {
        auto &f = frames[current];
        dependency(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &f.sets[set], 0,
                                nullptr);
        uint32_t push[] = {w, h, pass, mip};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, z);
    }
};
VulkanAtmosphere::VulkanAtmosphere(VkPhysicalDevice p, VkDevice d, uint32_t family, uint64_t budget,
                                   bool force, const std::filesystem::path &shaders, bool extended)
    : impl_(std::make_unique<Impl>(p, d, budget)) {
    impl_->extended = extended;
    impl_->init(family, force, shaders);
    impl_->resize({});
}
VulkanAtmosphere::~VulkanAtmosphere() = default;
void VulkanAtmosphere::invalidateSceneViews() {
    for (auto &f : impl_->frames)
        f.descriptorsValid = false;
}
bool VulkanAtmosphere::supported() const { return impl_->caps; }
bool VulkanAtmosphere::needsResize(const AtmosphereSettings &s) const {
    return impl_->quality != s.quality || impl_->requested != s.mode;
}
void VulkanAtmosphere::resize(const AtmosphereSettings &s) {
    validateAtmosphere(s);
    impl_->resize(s);
}
bool VulkanAtmosphere::historyChanged(const AtmosphereSettings &s, bool diagnostic) const {
    auto &i = *impl_;
    return i.lastProfile != atmosphereProfileKey(s) ||
           i.lastMode != (diagnostic ? AtmosphereMode::Off : s.mode) ||
           i.lastAerial != s.aerialPerspective || i.lastHaze != s.boundaryHaze;
}
void VulkanAtmosphere::begin(uint32_t frame, const Camera &camera, const AtmosphereSettings &s,
                             const LightingSettings &l, const TemporalView &v, float exposure,
                             bool diagnostic, float outer, VkDescriptorSet lighting, VkImageView hdr,
                             VkImageView depth, float localDistance) {
    auto &i = *impl_;
    i.current = frame;
    i.settings = s;
    i.recorded = false;
    auto &f = i.frames[frame];
    i.stats.effective = diagnostic ? AtmosphereMode::Off : s.mode;
    i.stats.fallback = diagnostic                      ? AtmosphereFallback::Diagnostic
                       : s.mode == AtmosphereMode::Off ? AtmosphereFallback::Disabled
                                                       : AtmosphereFallback::None;
    if (i.stats.effective == AtmosphereMode::Physical && !i.allocated) {
        i.stats.effective = AtmosphereMode::Analytic;
        i.stats.fallback = i.allocationFallback;
    }
    float altitude = float((camera.local.y - double(s.datum)) * .001);
    glm::vec3 sun =
        glm::length(l.sunDirection) > 1e-5f ? glm::normalize(l.sunDirection) : glm::vec3(0, 1, 0);
    f.dirtyProfile = f.profile != atmosphereProfileKey(s);
    f.dirtyOptical = f.optical != opticalKey(s, false);
    f.dirtyMultiple = f.multiple != opticalKey(s, true);
    f.dirtySky = f.dirtyProfile || altitude != f.altitude || sun != f.sun ||
                 s.boundaryHaze != f.haze || outer * .001f != f.outer;
    f.dirtyEnv = f.dirtyProfile || s.boundaryHaze != f.haze ||
                 (s.boundaryHaze > 0 && outer * .001f != f.outer) ||
                 std::abs(altitude - f.envAltitude) >= .025f ||
                 glm::dot(sun, f.envSun) < std::cos(glm::radians(.25f));
    AtmosphereUniform u;
    u.inverseRaster = glm::inverse(v.raster);
    u.current = v.current;
    u.previous = v.previous;
    u.planet = {s.planetRadius * .001f, s.atmosphereHeight * .001f, s.groundAlbedo,
                s.multipleScattering ? 1.f : 0.f};
    u.rayleigh = {s.rayleigh * 1000.f, s.rayleighHeight * .001f};
    u.mie = {s.mieScattering * s.aerosolDensity * 1000, s.mieAbsorption * s.aerosolDensity * 1000,
             s.mieHeight * .001f, s.mieG};
    u.ozone = {s.ozone * 1000.f, 0};
    u.profile = {s.ozoneCentre * .001f, s.ozoneWidth * .001f, localDistance, 0};
    u.sun = {sun, s.sunRadius};
    const float intensity = l.sunEnabled ? std::clamp(l.sunLux, 0.f, 200000.f) : 0;
    u.illumination = {glm::clamp(l.sunColour, glm::vec3(0), glm::vec3(1)) * intensity, exposure};
    float reactive =
        std::max({std::min(1.f, glm::length(sun - i.lastSun) * 20),
                  std::min(1.f, std::abs(altitude - i.lastAltitude)),
                  std::min(1.f, std::abs(intensity - i.lastIntensity) / std::max(1.f, intensity))});
    u.view = {altitude, camera.renderFar, reactive, i.extended ? float(f.t.width) : 0.f};
    u.options = {float(i.stats.effective), s.aerialPerspective ? 1.f : 0.f, float(s.diagnostic),
                 l.environmentEnabled ? s.environmentMultiplier : 0};
    u.extent = {intensity, float(f.sky.width), float(f.sky.height), float(f.t.depth)};
    u.haze = {s.boundaryHaze, outer * .001f, v.pixels.z / v.pixels.x, v.pixels.w / v.pixels.y};
    i.uniform = u;
    std::memcpy(f.constants.mapped, &u, sizeof(u));
    if (!f.descriptorsValid) {
        i.bind(f, lighting, false);
        if (i.caps)
            for (uint32_t at = 0; at < f.sets.size(); ++at) {
                auto set = f.sets[at];
                i.bind(f, set, true);
                imageDescriptor(i.device, set, 25,
                                at == 0   ? f.trans.view
                                : at == 1 ? f.multi.view
                                          : f.sky.view,
                                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                imageDescriptor(i.device, set, 26, f.t.view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                imageDescriptor(i.device, set, 27, f.s.view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                imageDescriptor(
                    i.device, set, 28,
                    f.env.mips[std::min(at >= 4 && at < 11 ? at - 4 : 0, f.env.levels - 1)],
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                imageDescriptor(i.device, set, 29, hdr, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
                imageDescriptor(i.device, set, 30, depth, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
            }
        f.descriptorsValid = true;
    }
    i.stats.timingsAvailable = false;
    if (i.queries && f.timed) {
        uint64_t q[12]{};
        if (vkGetQueryPoolResults(i.device, i.queries, frame * 6, 6, sizeof(q), q, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
            VK_SUCCESS) {
            i.stats.timingsAvailable = q[1] && q[3] && q[5] && q[7] && q[9] && q[11];
            if (i.stats.timingsAvailable) {
                i.stats.lutMilliseconds = (q[2] - q[0]) * i.period / 1e6;
                i.stats.environmentMilliseconds = (q[6] - q[4]) * i.period / 1e6;
                i.stats.compositeMilliseconds = (q[10] - q[8]) * i.period / 1e6;
            }
        }
        f.timed = false;
    }
}
void VulkanAtmosphere::record(VkCommandBuffer cmd) {
    auto &i = *impl_;
    auto &f = i.frames[i.current];
    i.recorded = true;
    if (!f.initialized) {
        for (auto image : f.images()) {
            transition(cmd, *image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue colour{};
            if (image == &f.trans || image == &f.t)
                colour.float32[0] = colour.float32[1] = colour.float32[2] = 1;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, image->levels, 0,
                                          image->layers};
            vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &colour, 1, &range);
        }
        vkCmdFillBuffer(cmd, f.sh.buffer, 0, VK_WHOLE_SIZE, 0);
        dependency(cmd);
    }
    if (!physical())
        return;
    if (i.queries) {
        vkCmdResetQueryPool(cmd, i.queries, i.current * 6, 6);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, i.queries, i.current * 6);
    }
    if (f.dirtyOptical)
        i.dispatch(cmd, 0, 0, 256, 64);
    if (f.dirtyMultiple)
        i.dispatch(cmd, 1, 1, 32, 32);
    if (f.dirtySky)
        i.dispatch(cmd, 2, 2, f.sky.width, f.sky.height);
    if (aerial())
        i.dispatch(cmd, 3, 3, f.t.width, f.t.height, f.t.depth);
    if (i.queries) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, i.queries,
                             i.current * 6 + 1);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, i.queries,
                             i.current * 6 + 2);
    }
    if (f.dirtyEnv) {
        for (uint32_t mip = 0; mip < 7; ++mip)
            i.dispatch(cmd, 4 + mip, 4, 64 >> mip, 64 >> mip, 6, mip);
        i.dispatch(cmd, 11, 5, 9, 1);
    }
    if (i.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, i.queries,
                             i.current * 6 + 3);
    dependency(cmd);
}
void VulkanAtmosphere::composite(VkCommandBuffer cmd, uint32_t w, uint32_t h, bool apply) {
    auto &i = *impl_;
    if (!physical())
        return;
    if (i.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, i.queries,
                             i.current * 6 + 4);
    if (apply && aerial())
        i.dispatch(cmd, 12, 6, w, h);
    if (i.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, i.queries,
                             i.current * 6 + 5);
}
void VulkanAtmosphere::submitted() {
    auto &i = *impl_;
    if (!i.recorded)
        return;
    auto &f = i.frames[i.current];
    f.initialized = true;
    i.last = i.current;
    if (physical()) {
        f.profile = atmosphereProfileKey(i.settings);
        f.optical = opticalKey(i.settings, false);
        f.multiple = opticalKey(i.settings, true);
        if (f.dirtyOptical)
            ++i.stats.transmittanceUpdates;
        if (f.dirtyMultiple)
            ++i.stats.multipleScatteringUpdates;
        f.altitude = i.uniform.view.x;
        f.sun = glm::vec3(i.uniform.sun);
        f.haze = i.settings.boundaryHaze;
        f.outer = i.uniform.haze.y;
        if (f.dirtyProfile)
            ++i.stats.profileUpdates;
        if (f.dirtySky)
            ++i.stats.skyUpdates;
        if (f.dirtyEnv) {
            ++i.stats.environmentUpdates;
            f.envAltitude = f.altitude;
            f.envSun = f.sun;
        }
        f.timed = i.queries != VK_NULL_HANDLE;
    }
    i.lastProfile = atmosphereProfileKey(i.settings);
    i.lastMode =
        i.stats.fallback == AtmosphereFallback::Diagnostic ? AtmosphereMode::Off : i.settings.mode;
    i.lastAerial = i.settings.aerialPerspective;
    i.lastHaze = i.settings.boundaryHaze;
    i.lastSun = glm::vec3(i.uniform.sun);
    i.lastAltitude = i.uniform.view.x;
    i.lastIntensity = i.uniform.extent.x;
}
bool VulkanAtmosphere::active() const { return impl_->stats.effective != AtmosphereMode::Off; }
bool VulkanAtmosphere::physical() const {
    return impl_->stats.effective == AtmosphereMode::Physical;
}
bool VulkanAtmosphere::aerial() const { return physical() && impl_->settings.aerialPerspective; }
const AtmosphereStats &VulkanAtmosphere::stats() const { return impl_->stats; }
uint64_t VulkanAtmosphere::snapshotBytes() const {
    auto &f = impl_->frames[impl_->last];
    uint64_t n = f.sh.size;
    for (auto p : {&f.trans, &f.multi, &f.sky, &f.t, &f.s})
        n += p->payload();
    return n;
}
void VulkanAtmosphere::copySnapshot(VkCommandBuffer cmd, VkBuffer target) {
    auto &f = impl_->frames[impl_->last];
    dependency(cmd);
    uint64_t offset = 0;
    for (auto p : {&f.trans, &f.multi, &f.sky, &f.t, &f.s}) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {p->width, p->height, p->depth};
        vkCmdCopyImageToBuffer(cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, target, 1, &copy);
        offset += p->payload();
    }
    VkBufferCopy copy{0, offset, f.sh.size};
    vkCmdCopyBuffer(cmd, f.sh.buffer, target, 1, &copy);
    dependency(cmd);
}
AtmosphereSnapshot VulkanAtmosphere::decodeSnapshot(const void *data) const {
    auto &f = impl_->frames[impl_->last];
    AtmosphereSnapshot out;
    out.skyExtent = {f.sky.width, f.sky.height};
    out.volumeSize = f.t.width;
    out.volumeDepth = f.t.depth;
    out.aerialStartDistance = static_cast<const AtmosphereUniform *>(f.constants.mapped)->profile.z;
    auto bytes = static_cast<const uint8_t *>(data);
    uint64_t offset = 0;
    std::vector<glm::vec4> *arrays[] = {&out.transmittance, &out.multipleScattering, &out.sky,
                                        &out.aerialTransmittance, &out.aerialScattering};
    uint32_t index = 0;
    for (auto p : {&f.trans, &f.multi, &f.sky, &f.t, &f.s}) {
        auto &v = *arrays[index++];
        v.resize(p->payload() / 8);
        for (size_t j = 0; j < v.size(); ++j)
            for (int k = 0; k < 4; ++k) {
                uint16_t h;
                std::memcpy(&h, bytes + offset + j * 8 + k * 2, 2);
                v[j][k] = glm::unpackHalf1x16(h);
            }
        offset += p->payload();
    }
    std::memcpy(out.irradiance.data(), bytes + offset, f.sh.size);
    return out;
}
} // namespace ow

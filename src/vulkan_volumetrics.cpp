#include "vulkan_volumetrics.hpp"
#include "horizon.hpp"
#include "render_resources.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>
namespace ow {
using namespace render_detail;
namespace {
uint32_t checkedVolumeCapacity(const VolumetricCapacity &c, uint32_t sceneLights) {
    // Validate before constructing any pools or reserving candidate storage.
    if (!c.volumes || c.volumes > 256 || !c.lights || c.lights > 128 || !c.clusterIndices ||
        c.clusterIndices > 128 || !sceneLights || sceneLights > 65535)
        throw std::invalid_argument("Invalid volumetric/scene-light capacities");
    return c.volumes;
}
struct Image {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkMemoryRequirements req{};
    uint32_t width = 1, height = 1, depth = 1, levels = 1, layers = 1;
    ~Image() { reset(); }
    void reset() {
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
    void create(VkDevice d, uint32_t w, uint32_t h, uint32_t z, bool storage, bool volume = false) {
        reset();
        device = d;
        width = w;
        height = h;
        depth = z;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {w, h, z};
        ci.mipLevels = levels;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
            throw std::runtime_error("Volumetric device memory unavailable");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &a, nullptr, &memory));
        check(vkBindImageMemory(device, image, memory, 0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        v.viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        check(vkCreateImageView(device, &v, nullptr, &view));
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

void dependency(VkCommandBuffer cmd) {
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
uint64_t lightKey(const LocalLight &l) {
    uint64_t h = 1469598103934665603ull;
    auto add = [&](auto v) {
        unsigned char b[sizeof(v)];
        std::memcpy(b, &v, sizeof(v));
        for (auto x : b)
            h = (h ^ x) * 1099511628211ull;
    };
    add(l.position.x);
    add(l.position.z);
    for (int k = 0; k < 3; ++k) {
        add(l.position.local[k]);
        add(l.colour[k]);
        add(l.direction[k]);
    }
    add(l.intensity);
    add(l.range);
    add(l.sourceRadius);
    add(l.innerAngle);
    add(l.outerAngle);
    add(l.type);
    add(l.volumetricScattering);
    add(l.volumetricHistory);
    return h;
}
bool sameSettings(const VolumetricSettings &a, const VolumetricSettings &b) {
    return a.enabled == b.enabled && a.temporal == b.temporal && a.quality == b.quality &&
           a.distance == b.distance && a.extinction == b.extinction && a.height == b.height &&
           a.scaleHeight == b.scaleHeight && a.localDensity == b.localDensity && a.albedo == b.albedo &&
           a.anisotropy == b.anisotropy && a.samples == b.samples && a.diagnostic == b.diagnostic;
}
} // namespace
struct VulkanVolumetrics::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    VolumetricCapacity capacity;
    bool caps = false, storage = false, allocated = false, requested = false, haveHistory = false,
         recorded = false;
    uint32_t width = 0, height = 0, current = 0, last = 0;
    uint64_t revision = 0, lastRevision = 0;
    VolumetricQuality quality = VolumetricQuality::Normal;
    VolumetricFallback allocationFallback = VolumetricFallback::Disabled;
    VolumetricStats stats;
    VolumetricSettings settings, previousSettings;
    Camera camera, previousCamera;
    WorldPosition origin, previousOrigin;
    float exposure = 1, previousExposure = 1;
    glm::vec3 previousSun{0}, previousColour{0};
    float previousLux = 0;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkDescriptorPool pool{};
    std::array<VkPipeline, 6> pipelines{};
    VkDescriptorSet lighting{};
    VkQueryPool queries{};
    double period = 0;
    uint32_t timestampBits = 0;
    struct Selected {
        LightHandle handle;
        uint32_t dense;
        uint64_t key;
        WorldPosition position;
        float range;
        bool history;
        double distance;
    };
    std::vector<Selected> candidates, selected, previousSelected;
    struct Frame {
        Image ext, raw, source, t, s, sky, skyT;
        Buffer constants, volumes, selected, rejections, counts, indices, counters, readback;
        VkDescriptorSet set{};
        bool initialized = false, timed = false, descriptorsValid = false;
        std::array<Image *, 7> images() { return {&ext, &raw, &source, &t, &s, &sky, &skyT}; }
    };
    std::array<Frame, 2> frames;
    Impl(VkPhysicalDevice p, VkDevice d, const VolumetricCapacity &c) : physical(p), device(d), capacity(c) {}
    ~Impl() {
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
    void init(uint32_t family, uint32_t sceneLights, bool force, VkDescriptorSetLayout shared,
              const std::filesystem::path &shaders) {
        candidates.reserve(sceneLights);
        selected.reserve(capacity.lights);
        previousSelected.reserve(capacity.lights);
        VkFormatProperties f{}, d{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &f);
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D32_SFLOAT, &d);
        const auto need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        storage = (f.optimalTilingFeatures & need) == need;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical, &props);
        caps = storage && (d.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !force &&
               props.limits.maxImageDimension3D >= 256 &&
               props.limits.maxPerStageDescriptorStorageImages >= 8;
        for (auto &frame : frames) {
            frame.constants.create(physical, device, sizeof(VolumetricUniform),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
            frame.volumes.create(physical, device, capacity.volumes * sizeof(FogVolumeRecord),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            frame.selected.create(physical, device, capacity.lights * sizeof(glm::uvec4),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            frame.rejections.create(physical, device, 2 * capacity.lights * sizeof(glm::vec4),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            frame.counters.create(physical, device, 16,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            frame.readback.create(physical, device, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        if (!caps)
            return;
        // Set 0 is private compute work; set 1 is the existing shared lighting set.
        std::array<VkDescriptorSetLayoutBinding, 17> bindings{};
        for (uint32_t k = 0; k < bindings.size(); ++k) {
            auto &b = bindings[k];
            b.binding = k;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b.descriptorType = k <= 5                            ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                               : (k == 13 || k == 14 || k == 16) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                                 : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = bindings.size();
        sl.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &setLayout));
        VkDescriptorSetLayout layouts[] = {setLayout, shared};
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2;
        pl.pSetLayouts = layouts;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
        const char *names[] = {"assign", "inject", "temporal", "integrate", "sky", "composite"};
        for (uint32_t k = 0; k < 6; ++k) {
            ShaderModule module(device, shaders / (std::string("volumetric_") + names[k] + ".comp.spv"));
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.layout = layout;
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module.handle;
            ci.stage.pName = "main";
            check(vkCreateComputePipelines(device, {}, 1, &ci, nullptr, &pipelines[k]));
        }
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6}};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pc.maxSets = 2;
        pc.poolSizeCount = 3;
        pc.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pc, nullptr, &pool));
        for (auto &f : frames) {
            VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool = pool;
            a.descriptorSetCount = 1;
            a.pSetLayouts = &setLayout;
            check(vkAllocateDescriptorSets(device, &a, &f.set));
        }
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        timestampBits = families.at(family).timestampValidBits;
        if (timestampBits && props.limits.timestampComputeAndGraphics) {
            period = props.limits.timestampPeriod;
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            q.queryType = VK_QUERY_TYPE_TIMESTAMP;
            q.queryCount = 24;
            check(vkCreateQueryPool(device, &q, nullptr, &queries));
        }
    }
    void makeImages(glm::uvec3 g) {
        for (auto &f : frames) {
            for (auto p : {&f.ext, &f.raw, &f.source})
                p->create(device, g.x, g.y, g.z, storage, true);
            for (auto p : {&f.t, &f.s})
                p->create(device, g.x, g.y, g.z + 1, storage, true);
            for (auto p : {&f.sky, &f.skyT})
                p->create(device, g.x, g.y, 1, storage);
            f.initialized = f.timed = f.descriptorsValid = false;
        }
    }
    uint64_t bytes() {
        uint64_t n = 0;
        for (auto &f : frames) {
            for (auto p : f.images())
                n += p->req.size;
            for (auto p : {&f.constants, &f.volumes, &f.selected, &f.rejections, &f.counts, &f.indices,
                           &f.counters, &f.readback})
                n += p->bytes;
        }
        return n;
    }
    void resize(uint32_t w, uint32_t h, const VolumetricSettings &s) {
        width = w;
        height = h;
        quality = s.quality;
        requested = s.enabled;
        allocated = caps && requested;
        allocationFallback = !caps ? VolumetricFallback::Unsupported : VolumetricFallback::Disabled;
        auto g = allocated ? volumetricGrid(w, h, s.quality) : glm::uvec3(1);
        for (auto &f : frames) {
            f.counts.reset();
            f.indices.reset();
        }
        makeImages(g);
        uint64_t clusters = uint64_t((g.x + 3) / 4) * ((g.y + 3) / 4) * ((g.z + 3) / 4);
        const uint64_t buffers = 2 * clusters * (4 + 4 * capacity.clusterIndices);
        if (allocated && bytes() + buffers > capacity.bytes) {
            allocated = false;
            allocationFallback = VolumetricFallback::Budget;
            g = glm::uvec3(1);
            makeImages(g);
            clusters = 1;
        }
        for (auto &f : frames) {
            for (auto p : f.images())
                p->allocate(physical, p != &f.sky && p != &f.skyT);
            f.counts.create(physical, device, clusters * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            f.indices.create(physical, device, clusters * capacity.clusterIndices * 4,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        stats.bytes = bytes();
        stats.grid = g;
        haveHistory = false;
    }
    void dispatch(VkCommandBuffer cmd, uint32_t pass, uint32_t w, uint32_t h, uint32_t z = 1,
                  uint32_t flag = 0) {
        dependency(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
        VkDescriptorSet sets[] = {frames[current].set, lighting};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0, nullptr);
        uint32_t push[] = {w, h, z, flag};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, z);
    }
    void stamp(VkCommandBuffer cmd, uint32_t p) {
        if (queries)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, current * 12 + p);
    }
};
VulkanVolumetrics::VulkanVolumetrics(VkPhysicalDevice p, VkDevice d, uint32_t family,
                                     const VolumetricCapacity &c, uint32_t sceneLights, bool force,
                                     VkDescriptorSetLayout lighting, const std::filesystem::path &shaders)
    : volumes(checkedVolumeCapacity(c, sceneLights)), impl_(std::make_unique<Impl>(p, d, c)) {
    impl_->init(family, sceneLights, force, lighting, shaders);
    impl_->resize(1, 1, {});
}
VulkanVolumetrics::~VulkanVolumetrics() = default;
void VulkanVolumetrics::edited() { ++impl_->revision; }
void VulkanVolumetrics::reset() { impl_->haveHistory = false; }
bool VulkanVolumetrics::historyChanged(const VolumetricSettings &s) const {
    return !sameSettings(s, impl_->previousSettings) || impl_->revision != impl_->lastRevision;
}
bool VulkanVolumetrics::supported() const { return impl_->caps; }
bool VulkanVolumetrics::needsResize(uint32_t w, uint32_t h, const VolumetricSettings &s) const {
    auto &i = *impl_;
    return i.width != w || i.height != h || i.requested != s.enabled || i.quality != s.quality;
}
void VulkanVolumetrics::resize(uint32_t w, uint32_t h, const VolumetricSettings &s) {
    validateVolumetrics(s);
    impl_->resize(w, h, s);
}
void VulkanVolumetrics::invalidateSceneViews() {
    for (auto &f : impl_->frames)
        f.descriptorsValid = false;
}
void VulkanVolumetrics::begin(uint32_t frame, const Camera &camera, TerrainGrid terrain,
                              const VolumetricSettings &s, const LightingSettings &light,
                              const ScenePool<LocalLight, LightHandle> &lights, LocalLightMode mode,
                              const TemporalView &v, float exposure, bool diagnostic, bool atmosphereChanged,
                              VkDescriptorSet lighting, VkImageView hdr, VkImageView depth,
                              glm::vec3 background) {
    auto &i = *impl_;
    auto &f = i.frames[frame];
    i.current = frame;
    i.lighting = lighting;
    i.settings = s;
    i.camera = camera;
    i.origin = worldPosition(camera, terrain);
    i.exposure = exposure;
    i.recorded = false;
    i.stats.timingsAvailable = false;
    if (f.timed) {
        const auto *c = static_cast<const uint32_t *>(f.readback.mapped);
        i.stats.overflowClusters = c[0];
        i.stats.rejectedCells = c[1];
        if (i.queries) {
            uint64_t q[24]{};
            if (vkGetQueryPoolResults(i.device, i.queries, frame * 12, 12, sizeof(q), q, 16,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                VK_SUCCESS) {
                i.stats.timingsAvailable = true;
                for (int k = 0; k < 12; ++k)
                    i.stats.timingsAvailable &= q[k * 2 + 1] != 0;
                double *times[] = {&i.stats.assignmentMilliseconds, &i.stats.injectionMilliseconds,
                                   &i.stats.temporalMilliseconds,   &i.stats.integrationMilliseconds,
                                   &i.stats.skyMilliseconds,        &i.stats.compositeMilliseconds};
                const uint64_t mask = i.timestampBits == 64 ? ~0ull : (1ull << i.timestampBits) - 1;
                if (i.stats.timingsAvailable)
                    for (int k = 0; k < 6; ++k)
                        *times[k] = double((q[k * 4 + 2] - q[k * 4]) & mask) * i.period / 1e6;
            }
        }
        f.timed = false;
    }
    bool density = s.extinction > 0;
    for (auto &volume : volumes.values())
        density |= volume.extinction > 0 && s.localDensity > 0;
    i.stats.active = s.enabled && i.allocated && !diagnostic && density;
    i.stats.fallback = diagnostic     ? VolumetricFallback::Diagnostic
                       : !s.enabled   ? VolumetricFallback::Disabled
                       : !i.allocated ? i.allocationFallback
                       : !density     ? VolumetricFallback::Empty
                                      : VolumetricFallback::None;
    auto delta = relativePosition(i.origin, i.previousOrigin);
    auto sun =
        glm::length(light.sunDirection) > 1e-5f ? glm::normalize(light.sunDirection) : glm::vec3(0, 1, 0);
    const float lux = light.sunEnabled ? std::clamp(light.sunLux, 0.f, 200000.f) : 0;
    if (!i.stats.active || !sameSettings(s, i.previousSettings) || atmosphereChanged ||
        i.revision != i.lastRevision || glm::length(delta) > 256 ||
        glm::dot(camera.forward(), i.previousCamera.forward()) < std::cos(glm::radians(45.f)) ||
        camera.curvature.x != i.previousCamera.curvature.x ||
        camera.curvature.z != i.previousCamera.curvature.z || glm::length(sun - i.previousSun) > .001f ||
        lux != i.previousLux || light.sunColour != i.previousColour)
        i.haveHistory = false;
    i.stats.historyValid = i.haveHistory && s.temporal;
    i.stats.volumes = uint32_t(volumes.values().size());
    i.stats.selectedLights = i.stats.excludedLights = 0;
    i.candidates.clear();
    i.selected.clear();
    if (i.stats.active && mode != LocalLightMode::Disabled) {
        const float aspect = float(i.width) / i.height,
                    ty = std::tan(glm::radians(Camera::verticalFovDegrees) * .5f), tx = ty * aspect;
        auto right = glm::normalize(glm::cross(camera.forward(), glm::vec3(0, 1, 0))),
             up = glm::cross(right, camera.forward());
        for (uint32_t n = 0; n < lights.values().size(); ++n) {
            const auto &l = lights.values()[n];
            if (l.volumetricScattering == 0 || l.intensity == 0)
                continue;
            auto flat = relativePosition(l.position, i.origin);
            if (glm::length(flat) >
                double(s.distance) * std::sqrt(1. + double(tx) * tx + double(ty) * ty) + l.range)
                continue;
            auto p = glm::vec3(curvePosition(flat, camera.curvature));
            float z = glm::dot(p, camera.forward());
            if (z + l.range < 0 || z - l.range > s.distance ||
                std::abs(glm::dot(p, right)) > z * tx + l.range * std::sqrt(1 + tx * tx) ||
                std::abs(glm::dot(p, up)) > z * ty + l.range * std::sqrt(1 + ty * ty))
                continue;
            i.candidates.push_back({lights.handle(n), n, lightKey(l), l.position, l.range,
                                    l.volumetricHistory, std::max(0., glm::length(flat) - l.range)});
        }
        std::sort(i.candidates.begin(), i.candidates.end(), [](const auto &a, const auto &b) {
            return a.distance != b.distance ? a.distance < b.distance : a.handle.slot < b.handle.slot;
        });
        const auto count = std::min(size_t(i.capacity.lights), i.candidates.size());
        i.selected.assign(i.candidates.begin(), i.candidates.begin() + count);
        i.stats.selectedLights = uint32_t(count);
        i.stats.excludedLights = uint32_t(i.candidates.size() - count);
    }
    auto *records = static_cast<glm::uvec4 *>(f.selected.mapped);
    auto *reject = static_cast<glm::vec4 *>(f.rejections.mapped);
    uint32_t rejects = 0;
    auto rejectLight = [&](const Impl::Selected &l) {
        auto p = curvePosition(relativePosition(l.position, i.origin), camera.curvature);
        reject[rejects++] = {glm::vec3(p), l.range};
    };
    for (uint32_t n = 0; n < i.selected.size(); ++n) {
        auto &l = i.selected[n];
        records[n] = {l.dense, 0, 0, 0};
        auto old = std::find_if(i.previousSelected.begin(), i.previousSelected.end(), [&](auto &p) {
            return p.handle.slot == l.handle.slot && p.handle.generation == l.handle.generation;
        });
        if (!l.history || old == i.previousSelected.end() || old->key != l.key)
            rejectLight(l);
    }
    for (auto &old : i.previousSelected) {
        auto now = std::find_if(i.selected.begin(), i.selected.end(), [&](auto &l) {
            return l.handle.slot == old.handle.slot && l.handle.generation == old.handle.generation;
        });
        if (now == i.selected.end() || now->key != old.key)
            rejectLight(old);
    }
    auto *vr = static_cast<FogVolumeRecord *>(f.volumes.mapped);
    uint32_t vn = 0;
    for (auto &volume : volumes.values()) {
        auto p = relativePosition(volume.position, i.origin);
        auto rot = glm::mat4_cast(glm::conjugate(glm::normalize(volume.rotation)));
        rot[3] = rot * glm::vec4(-glm::vec3(p), 1);
        vr[vn++] = {rot,
                    glm::vec4(volume.extent, 0),
                    glm::vec4(volume.albedo, volume.extinction * s.localDensity),
                    {float(volume.shape), volume.falloff, 0, 0}};
    }
    VolumetricUniform u;
    u.inverseViewProjection = glm::inverse(v.current);
    u.current = v.current;
    u.previous = i.haveHistory ? i.previousCamera.viewProjection(float(i.width) / i.height) : v.current;
    u.curvature = camera.curvature;
    u.previousCurvature = i.previousCamera.curvature;
    u.originDelta = glm::vec4(glm::vec3(delta), 0);
    u.grid = glm::vec4(glm::vec3(i.stats.grid), i.stats.active ? 1 : 0);
    u.depth = {s.distance, float(std::log1p(s.distance / volumetricDepthOffset)), float(s.samples),
               float(s.diagnostic)};
    u.height = {s.extinction, float(double(s.height) - camera.local.y), s.scaleHeight, s.anisotropy};
    u.albedo = glm::vec4(s.albedo, 0);
    u.temporal = {i.stats.historyValid ? 1 : 0, s.historyWeight, exposure / i.previousExposure, 0};
    u.counts = {float(vn), float(i.selected.size()), float(i.capacity.clusterIndices), float(rejects)};
    u.background = glm::vec4(background, 0);
    u.sun = glm::vec4(sun, lux);
    u.illumination = glm::vec4(glm::clamp(light.sunColour, glm::vec3(0), glm::vec3(1)), exposure);
    u.jitter = {v.pixels.z / v.pixels.x, v.pixels.w / v.pixels.y, s.temporal ? 1.f : 0.f, 0};
    std::memcpy(f.constants.mapped, &u, sizeof(u));
    if (!f.descriptorsValid) {
        descriptor(i.device, lighting, 25, f.constants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        Image *samples[] = {&f.t, &f.s, &f.ext, &f.source, &f.sky, &f.skyT};
        for (uint32_t k = 0; k < 6; ++k)
            imageDescriptor(i.device, lighting, 26 + k, samples[k]->view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        if (i.caps) {
            Buffer *buffers[] = {&f.volumes, &f.selected, &f.rejections, &f.counts, &f.indices, &f.counters};
            for (uint32_t k = 0; k < 6; ++k)
                descriptor(i.device, f.set, k, *buffers[k]);
            uint32_t k = 6;
            for (auto p : f.images())
                imageDescriptor(i.device, f.set, k++, p->view, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            auto &previous = i.frames[1 - frame];
            imageDescriptor(i.device, f.set, 13, previous.source.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            imageDescriptor(i.device, f.set, 14, previous.ext.view, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            // Scene attachment descriptors are used only by the composite pass.
            imageDescriptor(i.device, f.set, 15, hdr, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            imageDescriptor(i.device, f.set, 16, depth, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        }
        f.descriptorsValid = true;
    }
}
void VulkanVolumetrics::record(VkCommandBuffer cmd) {
    auto &i = *impl_;
    auto &f = i.frames[i.current];
    i.recorded = true;
    // Initialize both slots before any dynamically guarded previous-history read.
    for (auto &frame : i.frames)
        if (!frame.initialized) {
            for (auto p : frame.images()) {
                transition(cmd, *p, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                VkClearColorValue c{};
                if (p == &frame.t || p == &frame.skyT)
                    c.float32[0] = c.float32[1] = c.float32[2] = 1;
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
            }
        }
    dependency(cmd);
    if (!active())
        return;
    vkCmdFillBuffer(cmd, f.counters.buffer, 0, VK_WHOLE_SIZE, 0);
    if (i.queries)
        vkCmdResetQueryPool(cmd, i.queries, i.current * 12, 12);
    auto g = i.stats.grid;
    i.stamp(cmd, 0);
    i.dispatch(cmd, 0, (g.x + 3) / 4, (g.y + 3) / 4, (g.z + 3) / 4);
    i.stamp(cmd, 1);
    i.stamp(cmd, 2);
    i.dispatch(cmd, 1, g.x, g.y, g.z);
    i.stamp(cmd, 3);
    i.stamp(cmd, 4);
    i.dispatch(cmd, 2, g.x, g.y, g.z);
    i.stamp(cmd, 5);
    i.stamp(cmd, 6);
    i.dispatch(cmd, 3, g.x, g.y);
    i.stamp(cmd, 7);
    i.stamp(cmd, 8);
    i.dispatch(cmd, 4, g.x, g.y);
    i.stamp(cmd, 9);
    dependency(cmd);
}
void VulkanVolumetrics::composite(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    auto &i = *impl_;
    if (!active())
        return;
    i.stamp(cmd, 10);
    i.dispatch(cmd, 5, w, h);
    i.stamp(cmd, 11);
    dependency(cmd);
    auto &f = i.frames[i.current];
    VkBufferCopy copy{0, 0, 16};
    vkCmdCopyBuffer(cmd, f.counters.buffer, f.readback.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
void VulkanVolumetrics::submitted() {
    auto &i = *impl_;
    if (!i.recorded)
        return;
    for (auto &f : i.frames)
        f.initialized = true;
    i.last = i.current;
    i.frames[i.current].timed = active();
    i.haveHistory = active();
    i.previousCamera = i.camera;
    i.previousOrigin = i.origin;
    i.previousExposure = i.exposure;
    i.previousSettings = i.settings;
    auto u = static_cast<VolumetricUniform *>(i.frames[i.current].constants.mapped);
    i.previousSun = glm::vec3(u->sun);
    i.previousLux = u->sun.w;
    i.previousColour = glm::vec3(u->illumination);
    i.lastRevision = i.revision;
    i.previousSelected.assign(i.selected.begin(), i.selected.end());
}
bool VulkanVolumetrics::active() const { return impl_->stats.active; }
const VolumetricStats &VulkanVolumetrics::stats() const { return impl_->stats; }
uint64_t VulkanVolumetrics::snapshotBytes() const {
    auto &i = *impl_;
    if (!i.haveHistory)
        return 0;
    auto &f = i.frames[i.last];
    uint64_t n = 0;
    for (auto p : {&f.ext, &f.source, &f.t, &f.s, &f.sky, &f.skyT})
        n += p->payload();
    return n;
}
void VulkanVolumetrics::copySnapshot(VkCommandBuffer cmd, VkBuffer target) {
    auto &f = impl_->frames[impl_->last];
    dependency(cmd);
    uint64_t offset = 0;
    for (auto p : {&f.ext, &f.source, &f.t, &f.s, &f.sky, &f.skyT}) {
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {p->width, p->height, p->depth};
        vkCmdCopyImageToBuffer(cmd, p->image, VK_IMAGE_LAYOUT_GENERAL, target, 1, &copy);
        offset += p->payload();
    }
    dependency(cmd);
}
VolumetricSnapshot VulkanVolumetrics::decodeSnapshot(const void *data) const {
    auto &i = *impl_;
    auto &f = i.frames[i.last];
    VolumetricSnapshot out;
    out.grid = i.stats.grid;
    out.preExposure = i.previousExposure;
    auto bytes = static_cast<const uint8_t *>(data);
    uint64_t offset = 0;
    uint32_t index = 0;
    std::vector<glm::vec4> *arrays[] = {&out.extinction, &out.source, &out.transmittance,
                                        &out.scattering, &out.sky,    &out.skyTransmittance};
    for (auto p : {&f.ext, &f.source, &f.t, &f.s, &f.sky, &f.skyT}) {
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
    return out;
}
} // namespace ow

#include "vulkan_shadows.hpp"
#include "render_resources.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
namespace ow {
using namespace render_detail;
namespace {
constexpr uint32_t coverageCells = shadowCoverageSize * shadowCoverageSize * shadowCascades;
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                  VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src;
    b.srcAccessMask = sa;
    b.dstStageMask = dst;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, shadowCascades};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}
struct Image {
    VkDevice device{};
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    uint64_t bytes = 0;
    std::array<VkImageView, shadowCascades> layerViews{};
    ~Image() {
        for (auto v : layerViews)
            if (v)
                vkDestroyImageView(device, v, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
    void create(VkPhysicalDevice p, VkDevice d, uint32_t n, uint32_t count, VkFormat format) {
        device = d;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {n, n, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = count;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(d, &ci, nullptr, &image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, image, &req);
        bytes = req.size;
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
            throw std::runtime_error("No shadow depth memory");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = type;
        check(vkAllocateMemory(d, &a, nullptr, &memory));
        check(vkBindImageMemory(d, image, memory, 0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        v.format = ci.format;
        v.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, count};
        check(vkCreateImageView(d, &v, nullptr, &view));
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange.layerCount = 1;
        if (count == shadowCascades)
            for (uint32_t i = 0; i < shadowCascades; ++i) {
                v.subresourceRange.baseArrayLayer = i;
                check(vkCreateImageView(d, &v, nullptr, &layerViews[i]));
            }
    }
};
bool intersects(const VisibilityBounds &b, const ShadowCascade &c) {
    return c.farDistance > 0 && b.hi.x >= c.centre.x - c.radius && b.lo.x <= c.centre.x + c.radius &&
           b.hi.y >= c.centre.y - c.radius && b.lo.y <= c.centre.y + c.radius;
}
} // namespace
ShadowCapabilities shadowCapabilities(VkPhysicalDevice p) {
    ShadowCapabilities c;
    VkFormatProperties f;
    vkGetPhysicalDeviceFormatProperties(p, VK_FORMAT_D32_SFLOAT, &f);
    const auto required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    c.depth = (f.optimalTilingFeatures & required) == required;
    c.filtered = (f.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(p, &features);
    c.indirect = features.drawIndirectFirstInstance;
    c.multi = features.multiDrawIndirect;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    c.maxDraws = std::max(1u, props.limits.maxDrawIndirectCount);
    c.maxResolution = std::min({props.limits.maxImageDimension2D, props.limits.maxFramebufferWidth,
                                props.limits.maxFramebufferHeight});
    return c;
}
struct VulkanShadows::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    ShadowCapabilities caps;
    ShadowCapacity capacity;
    Image depth;
    VkDescriptorSetLayout layout{};
    VkDescriptorPool pool{};
    VkSampler sampler{};
    VkQueryPool queries{};
    double timestampPeriod = 0;
    uint32_t timestampBits = 0;
    struct Frame {
        Buffer uniform, records, sources, commands, coverage;
        VkDescriptorSet set{};
        ShadowStats stats;
        bool submitted = false;
    };
    std::array<Frame, 2> frames;
    struct Caster {
        VisibilityBounds bounds;
        uint32_t count, first;
    };
    std::vector<Caster> casters;
    std::vector<uint32_t> offsets;
    std::array<ShadowCascade, shadowCascades> cascades;
    std::array<uint32_t, coverageCells> coverage{};
    ShadowUniform uniform;
    ShadowStats current, last;
    uint32_t frame = 0, raster = 0, invalidMask = 0;
    VkPipelineLayout boundLayout{};
    std::chrono::steady_clock::time_point started;
    Impl(VkPhysicalDevice p, VkDevice d, ShadowCapabilities c, ShadowCapacity cap)
        : physical(p), device(d), caps(c), capacity(cap) {
        casters.reserve(cap.casters);
        offsets.resize(size_t(cap.casters + 1) * shadowCascades);
    }
    ~Impl() {
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (layout)
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
};
VulkanShadows::VulkanShadows(VkPhysicalDevice p, VkDevice d, uint32_t family, ShadowCapabilities caps,
                             ShadowCapacity cap) {
    if (!cap.casters || cap.casters > 65536 || cap.resolution < 16 || cap.resolution > caps.maxResolution)
        throw std::invalid_argument("Invalid CSM capacity");
    impl_ = std::make_unique<Impl>(p, d, caps, cap);
    auto &s = *impl_;
    // Query exact allocation requirements before allocating memory.
    const uint32_t storageResolution = caps.depth ? cap.resolution : 16;
    const VkFormat storageFormat = caps.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
    uint64_t required = 0;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = storageFormat;
    ici.extent = {storageResolution, storageResolution, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = shadowCascades;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage tmp;
    check(vkCreateImage(d, &ici, nullptr, &tmp));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, tmp, &req);
    required = req.size;
    vkDestroyImage(d, tmp, nullptr);
    const uint64_t sizes[] = {sizeof(ShadowUniform), uint64_t(cap.casters) * 128, uint64_t(cap.casters) * 4,
                              uint64_t(cap.casters) * shadowCascades * sizeof(VkDrawIndexedIndirectCommand),
                              coverageCells * 4};
    for (uint64_t bytes : sizes) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = bytes;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        VkBuffer b;
        check(vkCreateBuffer(d, &ci, nullptr, &b));
        vkGetBufferMemoryRequirements(d, b, &req);
        required += 2 * req.size;
        vkDestroyBuffer(d, b, nullptr);
    }
    if (required > cap.bytes)
        throw std::invalid_argument("CSM allocation exceeds shadow memory budget");
    s.depth.create(p, d, storageResolution, shadowCascades, storageFormat);
    s.current.allocatedBytes = required;
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.layout));
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                 {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    check(vkCreateDescriptorPool(d, &pi, nullptr, &s.pool));
    for (auto &f : s.frames) {
        f.uniform.create(p, d, sizes[0], VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f.records.create(p, d, sizes[1], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.sources.create(p, d, sizes[2], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.commands.create(p, d, sizes[3], VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        f.coverage.create(p, d, sizes[4], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        a.descriptorPool = s.pool;
        a.descriptorSetCount = 1;
        a.pSetLayouts = &s.layout;
        check(vkAllocateDescriptorSets(d, &a, &f.set));
        descriptor(d, f.set, 0, f.records);
        descriptor(d, f.set, 1, f.uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        descriptor(d, f.set, 2, f.sources);
    }
    s.current.allocatedBytes = s.depth.bytes;
    for (const auto &f : s.frames)
        s.current.allocatedBytes +=
            f.uniform.bytes + f.records.bytes + f.sources.bytes + f.commands.bytes + f.coverage.bytes;
    if (s.current.allocatedBytes > cap.bytes)
        throw std::runtime_error("CSM allocation exceeds validated memory budget");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = caps.depth && caps.filtered ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    check(vkCreateSampler(d, &si, nullptr, &s.sampler));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    s.timestampPeriod = props.limits.timestampPeriod;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, queues.data());
    s.timestampBits = queues.at(family).timestampValidBits;
    if (s.timestampBits) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * shadowCascades * 2;
        check(vkCreateQueryPool(d, &qi, nullptr, &s.queries));
    }
}
VulkanShadows::~VulkanShadows() = default;
VkDescriptorSetLayout VulkanShadows::layout() const { return impl_->layout; }
void VulkanShadows::initialize(VkCommandBuffer cmd) {
    auto &s = *impl_;
    imageBarrier(cmd, s.depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearDepthStencilValue clear{0, 0};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, shadowCascades};
    vkCmdClearDepthStencilImage(cmd, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    imageBarrier(cmd, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
void VulkanShadows::begin(uint32_t frame, const Camera &camera, TerrainGrid grid, float aspect, glm::vec3 sun,
                          const ShadowSettings &settings, bool disabled, float farDistance) {
    auto &s = *impl_;
    s.started = std::chrono::steady_clock::now();
    s.frame = frame;
    auto &f = s.frames[frame];
    if (f.submitted) {
        s.last = f.stats;
        s.last.completed = true;
        s.last.totalMilliseconds = 0;
        if (s.queries) {
            std::array<uint64_t, shadowCascades * 4> values{};
            vkGetQueryPoolResults(s.device, s.queries, frame * shadowCascades * 2, shadowCascades * 2,
                                  sizeof(values), values.data(), 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            s.last.timingsAvailable = true;
            const uint64_t mask = s.timestampBits == 64 ? ~0ull : (1ull << s.timestampBits) - 1;
            for (uint32_t i = 0; i < shadowCascades; ++i) {
                if (!values[i * 4 + 1] || !values[i * 4 + 3]) {
                    s.last.timingsAvailable = false;
                    continue;
                }
                s.last.rasterMilliseconds[i] =
                    double((values[i * 4 + 2] - values[i * 4]) & mask) * s.timestampPeriod / 1e6;
                s.last.totalMilliseconds += s.last.rasterMilliseconds[i];
            }
        }
        f.submitted = false;
    }
    const auto bytes = s.current.allocatedBytes;
    s.current = {};
    s.current.allocatedBytes = bytes;
    s.current.requested = settings.mode;
    s.current.effective = settings.mode;
    if (!s.caps.depth) {
        s.current.effective = ShadowMode::Off;
        s.current.fallback = ShadowFallback::UnsupportedDepth;
    } else if (disabled) {
        s.current.effective = ShadowMode::Off;
        s.current.fallback = ShadowFallback::Wireframe;
    }
    s.casters.clear();
    s.coverage.fill(1);
    s.invalidMask = 0;
    s.uniform = {};
    s.cascades = shadowCascadeLayout(camera, grid, aspect, glm::dvec3(sun), settings, farDistance,
                                     s.capacity.resolution);
    const auto normalized = glm::normalize(sun);
    s.uniform.forward = glm::vec4(camera.forward(), 0);
    s.uniform.sun = glm::vec4(normalized, glm::smoothstep(0.f, .1f, normalized.y));
    s.uniform.sampling = {settings.biasTexels, settings.normalBiasTexels, float(s.capacity.resolution),
                          float(settings.diagnostic)};
    s.uniform.options = {active() ? 1u : 0u, s.cascades[4].farDistance > 0 ? 5u : 4u, 31u,
                         s.caps.filtered ? 1u : 0u};
    s.current.nearDistance = s.cascades[3].farDistance;
    s.current.farDistance = s.cascades[s.uniform.options.y - 1].farDistance;
    for (uint32_t i = 0; i < shadowCascades; ++i)
        s.uniform.ranges[i] = {s.cascades[i].nearDistance, s.cascades[i].farDistance,
                               float(2 * s.cascades[i].radius / s.capacity.resolution), 0};
    s.current.incomplete += normalized.y < .1f;
    s.current.preparationMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.started).count();
}
uint32_t VulkanShadows::append(VisibilityBounds bounds, uint32_t count, uint32_t first, uint32_t source,
                               const void *record, uint32_t bytes) {
    auto &s = *impl_;
    if (!active() || !count)
        return ~0u;
    if (bytes > 128 || (bytes && !record))
        throw std::invalid_argument("CSM draw record");
    const auto light = shadowLightBounds(bounds, s.cascades[0].basis);
    uint32_t mask = 0;
    for (uint32_t i = 0; i < cascadeCount(); ++i)
        if (intersects(light, s.cascades[i]))
            mask |= 1u << i;
    if (!mask)
        return ~0u;
    if (s.casters.size() == s.capacity.casters) {
        ++s.current.overflow;
        s.invalidMask |= mask;
        s.current.fallback = ShadowFallback::Capacity;
        return ~0u;
    }
    uint32_t id = uint32_t(s.casters.size());
    s.casters.push_back({light, count, first});
    if (bytes)
        std::memcpy(static_cast<char *>(s.frames[s.frame].records.mapped) + uint64_t(id) * 128, record,
                    bytes);
    static_cast<uint32_t *>(s.frames[s.frame].sources.mapped)[id] = source;
    return id;
}
void VulkanShadows::missing(VisibilityBounds bounds) {
    auto &s = *impl_;
    if (!active())
        return;
    ++s.current.incomplete;
    const auto b = shadowLightBounds(bounds, s.cascades[0].basis);
    for (uint32_t i = 0; i < cascadeCount(); ++i) {
        const auto &c = s.cascades[i];
        if (!intersects(b, c))
            continue;
        s.uniform.options.z |= 1u << (i + 8);
        const glm::dvec2 minimum = c.centre - glm::dvec2(c.radius);
        const auto lo = (glm::dvec2(b.lo) - minimum) / (2 * c.radius) * double(shadowCoverageSize);
        const auto hi = (glm::dvec2(b.hi) - minimum) / (2 * c.radius) * double(shadowCoverageSize);
        int x0 = int(std::clamp(std::floor(lo.x) - 1., 0., 31.)),
            x1 = int(std::clamp(std::ceil(hi.x) + 1., 0., 31.));
        int y0 = int(std::clamp(std::floor(lo.y) - 1., 0., 31.)),
            y1 = int(std::clamp(std::ceil(hi.y) + 1., 0., 31.));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                s.coverage[i * 1024 + (31 - y) * 32 + x] = 0;
    }
}
void VulkanShadows::prepare(double collectionMilliseconds) {
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    s.started = std::chrono::steady_clock::now();
    auto *commands = static_cast<VkDrawIndexedIndirectCommand *>(f.commands.mapped);
    const auto forward = glm::dvec3(s.uniform.forward);
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < cascadeCount(); ++i) {
        const auto &c = s.cascades[i];
        const double centreZ = (c.basis * (forward * double((c.nearDistance + c.farDistance) * .5f))).z;
        double z0 = centreZ - c.radius - 16, z1 = centreZ + c.radius + 16;
        const double maximum = z1 + 8192;
        for (uint32_t n = 0; n < s.casters.size(); ++n) {
            s.offsets[size_t(i) * (s.capacity.casters + 1) + n] = cursor;
            const auto &caster = s.casters[n];
            if (!intersects(caster.bounds, c) || caster.bounds.hi.z < z0 || caster.bounds.lo.z > maximum ||
                (s.invalidMask & (1u << i)))
                continue;
            z1 = std::max(z1, std::min(maximum, double(caster.bounds.hi.z) + 16));
            commands[cursor++] = {caster.count, 1, caster.first, 0, n};
            ++s.current.cascadeCasters[i];
            s.current.triangles[i] += caster.count / 3;
        }
        s.offsets[size_t(i) * (s.capacity.casters + 1) + s.casters.size()] = cursor;
        glm::dmat4 matrix(1);
        for (int axis = 0; axis < 3; ++axis) {
            matrix[axis][0] = c.basis[axis][0] / c.radius;
            matrix[axis][1] = -c.basis[axis][1] / c.radius;
            matrix[axis][2] = c.basis[axis][2] / (z1 - z0);
        }
        matrix[3][0] = -c.centre.x / c.radius;
        matrix[3][1] = c.centre.y / c.radius;
        matrix[3][2] = -z0 / (z1 - z0);
        s.uniform.matrices[i] = glm::mat4(matrix);
        s.uniform.ranges[i].w = float(1 / (z1 - z0));
    }
    s.uniform.options.z &= ~s.invalidMask;
    s.current.casters = uint32_t(s.casters.size());
    std::memcpy(f.uniform.mapped, &s.uniform, sizeof(s.uniform));
    std::memcpy(f.coverage.mapped, s.coverage.data(), sizeof(s.coverage));
    s.current.preparationMilliseconds +=
        collectionMilliseconds +
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.started).count();
}
bool VulkanShadows::active() const { return impl_->current.effective == ShadowMode::CSM; }
uint32_t VulkanShadows::cascadeCount() const { return impl_->uniform.options.y; }
void VulkanShadows::writeLighting(VkDescriptorSet set) {
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    descriptor(s.device, set, 10, f.uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    descriptor(s.device, set, 11, f.coverage);
    VkDescriptorImageInfo image{VK_NULL_HANDLE, s.depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sampler{s.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w[2]{};
    for (auto &v : w) {
        v.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        v.dstSet = set;
        v.descriptorCount = 1;
    }
    w[0].dstBinding = 12;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &image;
    w[1].dstBinding = 13;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &sampler;
    vkUpdateDescriptorSets(s.device, 2, w, 0, nullptr);
}
void VulkanShadows::beginRaster(VkCommandBuffer cmd, uint32_t cascade) {
    auto &s = *impl_;
    s.raster = cascade;
    if (cascade == 0) {
        if (s.queries)
            vkCmdResetQueryPool(cmd, s.queries, s.frame * 10, 10);
        imageBarrier(cmd, s.depth.image, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    if (s.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                             s.frame * 10 + cascade * 2);
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = s.depth.layerViews[cascade];
    a.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue.depthStencil = {0, 0};
    VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
    r.renderArea = {{0, 0}, {s.capacity.resolution, s.capacity.resolution}};
    r.layerCount = 1;
    r.pDepthAttachment = &a;
    vkCmdBeginRendering(cmd, &r);
    VkViewport viewport{0, 0, float(s.capacity.resolution), float(s.capacity.resolution), 0, 1};
    VkRect2D scissor{{0, 0}, {s.capacity.resolution, s.capacity.resolution}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void VulkanShadows::endRaster(VkCommandBuffer cmd) {
    auto &s = *impl_;
    vkCmdEndRendering(cmd);
    if (s.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                             s.frame * 10 + s.raster * 2 + 1);
}
void VulkanShadows::bind(VkCommandBuffer cmd, VkPipelineLayout layout) {
    auto &s = *impl_;
    s.boundLayout = layout;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 1, &s.frames[s.frame].set, 0,
                            nullptr);
}
void VulkanShadows::drawRange(VkCommandBuffer cmd, uint32_t first, uint32_t count) {
    auto &s = *impl_;
    if (first >= s.casters.size())
        return;
    const auto base = size_t(s.raster) * (s.capacity.casters + 1);
    uint32_t start = s.offsets[base + first],
             end = s.offsets[base + std::min<uint32_t>(first + count, s.casters.size())];
    uint32_t push[2] = {s.raster, ~0u};
    if (s.caps.indirect) {
        vkCmdPushConstants(cmd, s.boundLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, push);
        while (start < end) {
            uint32_t n = std::min(end - start, s.caps.multi ? s.caps.maxDraws : 1u);
            vkCmdDrawIndexedIndirect(cmd, s.frames[s.frame].commands.buffer, uint64_t(start) * 20, n, 20);
            start += n;
        }
    } else {
        auto *commands = static_cast<const VkDrawIndexedIndirectCommand *>(s.frames[s.frame].commands.mapped);
        for (; start < end; ++start) {
            const auto &c = commands[start];
            push[1] = c.firstInstance;
            vkCmdPushConstants(cmd, s.boundLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, push);
            vkCmdDrawIndexed(cmd, c.indexCount, 1, c.firstIndex, 0, 0);
        }
    }
}
void VulkanShadows::finish(VkCommandBuffer cmd) {
    auto &s = *impl_;
    if (s.queries)
        for (uint32_t i = cascadeCount(); i < shadowCascades; ++i) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, s.frame * 10 + i * 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries,
                                 s.frame * 10 + i * 2 + 1);
        }
    imageBarrier(cmd, s.depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    s.frames[s.frame].submitted = true;
    s.frames[s.frame].stats = s.current;
}
uint64_t VulkanShadows::snapshotBytes() const {
    return uint64_t(impl_->capacity.resolution) * impl_->capacity.resolution * shadowCascades * 4;
}
void VulkanShadows::copySnapshot(VkCommandBuffer cmd, VkBuffer destination) const {
    auto &s = *impl_;
    imageBarrier(cmd, s.depth.image, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, shadowCascades};
    region.imageExtent = {s.capacity.resolution, s.capacity.resolution, 1};
    vkCmdCopyImageToBuffer(cmd, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, 1, &region);
    imageBarrier(cmd, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
ShadowSnapshot VulkanShadows::decodeSnapshot(const void *data) const {
    auto &s = *impl_;
    ShadowSnapshot out;
    out.resolution = s.capacity.resolution;
    out.uniform = s.uniform;
    out.depth.resize(snapshotBytes() / 4);
    std::memcpy(out.depth.data(), data, snapshotBytes());
    out.coverage.assign(s.coverage.begin(), s.coverage.end());
    return out;
}
const ShadowStats &VulkanShadows::stats() const {
    return active() && impl_->last.completed ? impl_->last : impl_->current;
}
} // namespace ow

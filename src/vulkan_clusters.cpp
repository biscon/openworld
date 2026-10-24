#include "vulkan_clusters.hpp"
#include "render_resources.hpp"
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
namespace ow {
using namespace render_detail;
namespace {
struct LightRecord {
    glm::vec4 positionRange, colourIntensity, directionOuter, parameters;
};
struct ClusterUniform {
    glm::mat4 view;
    glm::uvec4 grid, options;
    glm::vec4 depth, projection;
};
struct Bounds {
    glm::vec4 low, high;
};
static_assert(sizeof(LightRecord) == 64 && sizeof(ClusterUniform) == 128 && sizeof(Bounds) == 32);
} // namespace
struct VulkanClusters::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    SceneCapacity capacity;
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    Buffer bounds;
    struct Frame {
        Buffer uniform, lights, counts, indices, readback;
        bool submitted = false, timed = false;
        uint32_t mode = 0;
    };
    std::array<Frame, 2> frames;
    SceneStats stats;
    uint32_t width = 0, height = 0;
    VkQueryPool queries{};
    double period = 0;
    uint32_t timestampBits = 0;
    Impl(VkPhysicalDevice p, VkDevice d, const SceneCapacity &c) : physical(p), device(d), capacity(c) {}
    ~Impl() {
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
    }
    void collect(uint32_t i) {
        auto &f = frames[i];
        if (!f.submitted)
            return;
        stats.occupiedClusters = stats.maximumOccupancy = stats.overflowClusters = 0;
        uint64_t sum = 0;
        if (f.mode == uint32_t(LocalLightMode::Clustered))
            for (uint32_t j = 0; j < stats.grid.count(); ++j) {
                auto n = static_cast<uint32_t *>(f.readback.mapped)[j];
                sum += n;
                stats.occupiedClusters += n > 0;
                stats.maximumOccupancy = std::max(n, stats.maximumOccupancy);
                stats.overflowClusters += n > stats.grid.indices;
            }
        stats.meanOccupancy = stats.occupiedClusters ? double(sum) / stats.occupiedClusters : 0;
        if (queries && f.timed) {
            std::array<uint64_t, 12> q{};
            auto r = vkGetQueryPoolResults(device, queries, i * 6, 6, sizeof(q), q.data(), 16,
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            stats.timingsAvailable = r == VK_SUCCESS;
            for (int j = 0; j < 6; ++j)
                stats.timingsAvailable &= q[j * 2 + 1] != 0;
            if (stats.timingsAvailable) {
                const uint64_t mask = timestampBits == 64 ? ~0ull : (1ull << timestampBits) - 1;
                stats.assignmentMilliseconds = double((q[2] - q[0]) & mask) * period / 1e6;
                stats.opaqueMilliseconds = double((q[6] - q[4]) & mask) * period / 1e6;
                stats.transparentMilliseconds = double((q[10] - q[8]) & mask) * period / 1e6;
            }
        }
        f.submitted = false;
        f.timed = false;
    }
};
VulkanClusters::VulkanClusters(VkPhysicalDevice p, VkDevice d, const SceneCapacity &c, uint32_t queueFamily,
                               VkDescriptorSetLayout lighting, VkDescriptorSetLayout empty,
                               const std::filesystem::path &shaders)
    : lights(c.lights), impl_(std::make_unique<Impl>(p, d, c)) {
    if (!c.lights || c.lights > 65535)
        throw std::invalid_argument("Light capacity must be 1..65535");
    auto &s = *impl_;
    VkDescriptorSetLayout layouts[] = {empty, lighting};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = layouts;
    check(vkCreatePipelineLayout(d, &li, nullptr, &s.layout));
    auto module = shader(d, shaders / "cluster_assign.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = s.layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    auto result = vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &s.pipeline);
    vkDestroyShaderModule(d, module, nullptr);
    check(result);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    s.period = props.limits.timestampPeriod;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &count, families.data());
    s.timestampBits = families.at(queueFamily).timestampValidBits;
    if (s.timestampBits && props.limits.timestampComputeAndGraphics) {
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 12;
        check(vkCreateQueryPool(d, &q, nullptr, &s.queries));
    }
    for (auto &f : s.frames) {
        f.uniform.create(p, d, sizeof(ClusterUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f.lights.create(p, d, uint64_t(c.lights) * sizeof(LightRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        true);
    }
}
VulkanClusters::~VulkanClusters() = default;
void VulkanClusters::resize(uint32_t w, uint32_t h) {
    auto &s = *impl_;
    if (w == s.width && h == s.height)
        return;
    s.width = w;
    s.height = h;
    s.stats.grid = clusterGrid(w, h, s.capacity);
    auto g = s.stats.grid;
    s.bounds.create(s.physical, s.device, uint64_t(g.count()) * sizeof(Bounds),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    const float ty = std::tan(glm::radians(Camera::verticalFovDegrees) * .5f), tx = ty * float(w) / h;
    auto *b = static_cast<Bounds *>(s.bounds.mapped);
    for (uint32_t z = 0; z < g.z; ++z)
        for (uint32_t y = 0; y < g.y; ++y)
            for (uint32_t x = 0; x < g.x; ++x) {
                const float zn = Camera::nearPlane *
                                 std::pow(Camera::farPlane / Camera::nearPlane, float(z) / g.z),
                            zf = Camera::nearPlane *
                                 std::pow(Camera::farPlane / Camera::nearPlane, float(z + 1) / g.z);
                const float xmin = (float(x * g.tile) / w * 2 - 1) * tx,
                            xmax = (float(std::min(w, (x + 1) * g.tile)) / w * 2 - 1) * tx;
                const float ymin = (1 - float(std::min(h, (y + 1) * g.tile)) / h * 2) * ty,
                            ymax = (1 - float(y * g.tile) / h * 2) * ty;
                b[(z * g.y + y) * g.x + x] = {
                    glm::vec4(std::min(xmin * zn, xmin * zf), std::min(ymin * zn, ymin * zf), zn, 0),
                    glm::vec4(std::max(xmax * zn, xmax * zf), std::max(ymax * zn, ymax * zf), zf, 0)};
            }
    s.stats.clusterBytes = s.bounds.bytes;
    for (auto &f : s.frames) {
        f.submitted = f.timed = false;
        f.counts.create(s.physical, s.device, uint64_t(g.count()) * 4,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        f.indices.create(s.physical, s.device, uint64_t(g.count()) * g.indices * 4,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        f.readback.create(s.physical, s.device, uint64_t(g.count()) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          true);
        s.stats.clusterBytes +=
            f.counts.bytes + f.indices.bytes + f.readback.bytes + f.lights.bytes + f.uniform.bytes;
    }
    if (s.stats.clusterBytes > 2 * s.capacity.clusterBytesPerFrame)
        throw std::runtime_error("Actual cluster allocations exceed budget");
}
void VulkanClusters::prepare(uint32_t frame, const Camera &camera, TerrainGrid terrain, VkDescriptorSet set,
                             LocalLightMode mode, uint32_t diagnostic) {
    auto &s = *impl_;
    s.collect(frame);
    auto &f = s.frames.at(frame);
    auto g = s.stats.grid;
    ClusterUniform u{};
    u.view = glm::lookAt(glm::vec3(0), camera.forward(), glm::vec3(0, 1, 0));
    u.view[0][2] *= -1;
    u.view[1][2] *= -1;
    u.view[2][2] *= -1;
    u.view[3][2] *= -1;
    u.grid = {g.x, g.y, g.z, g.tile};
    u.options = {uint32_t(lights.values().size()), g.indices, uint32_t(mode), std::min(diagnostic, 2u)};
    u.depth = {Camera::nearPlane, Camera::farPlane,
               float(g.z) / std::log(Camera::farPlane / Camera::nearPlane), 0};
    u.projection = {float(s.width), float(s.height),
                    std::tan(glm::radians(Camera::verticalFovDegrees) * .5f) * float(s.width) / s.height,
                    std::tan(glm::radians(Camera::verticalFovDegrees) * .5f)};
    auto origin = worldPosition(camera, terrain);
    auto *records = static_cast<LightRecord *>(f.lights.mapped);
    uint32_t n = 0;
    for (const auto &l : lights.values()) {
        const auto flat = relativePosition(l.position, origin);
        const bool local = glm::length(flat) <= camera.renderFar * 2.0 + l.range;
        const auto curved = local ? curvePosition(flat, camera.curvature) : flat;
        const auto direction =
            local ? glm::normalize(glm::vec3(
                        curvePosition(flat + glm::dvec3(l.direction), camera.curvature) - curved))
                  : glm::normalize(l.direction);
        records[n++] = {glm::vec4(glm::vec3(curved), l.range),
                        glm::vec4(l.colour, l.intensity),
                        glm::vec4(direction, std::cos(l.outerAngle)),
                        {l.sourceRadius, std::cos(l.innerAngle), float(l.type), 0}};
    }
    std::memcpy(f.uniform.mapped, &u, sizeof(u));
    f.mode = uint32_t(mode);
    s.stats.activeLights = n;
    descriptor(s.device, set, 4, f.lights);
    descriptor(s.device, set, 5, f.counts);
    descriptor(s.device, set, 6, f.indices);
    descriptor(s.device, set, 7, f.uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    descriptor(s.device, set, 8, s.bounds);
}
void VulkanClusters::record(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet set) {
    auto &s = *impl_;
    auto &f = s.frames[frame];
    if (s.queries)
        vkCmdResetQueryPool(cmd, s.queries, frame * 6, 6);
    timestamp(cmd, frame, 0);
    if (f.mode == uint32_t(LocalLightMode::Clustered)) {
        vkCmdFillBuffer(cmd, f.counts.buffer, 0, VK_WHOLE_SIZE, 0);
        barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (!lights.values().empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 1, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, uint32_t(lights.values().size()), 1, 1);
        }
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy copy{0, 0, f.counts.size};
        vkCmdCopyBuffer(cmd, f.counts.buffer, f.readback.buffer, 1, &copy);
        barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
    timestamp(cmd, frame, 1);
    f.submitted = true;
}
void VulkanClusters::timestamp(VkCommandBuffer cmd, uint32_t frame, uint32_t point) {
    auto &s = *impl_;
    if (s.queries) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s.queries, frame * 6 + point);
        if (point == 5)
            s.frames[frame].timed = true;
    }
}
const SceneStats &VulkanClusters::stats() const { return impl_->stats; }
} // namespace ow

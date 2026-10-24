#include "vulkan_vegetation_interaction.hpp"
#include <cstring>
namespace ow {
using namespace render_detail;
VulkanVegetationInteraction::VulkanVegetationInteraction(VkPhysicalDevice physical, VkDevice d,
                                                         const std::filesystem::path &shaders,
                                                         uint64_t budget, bool diagnostics)
    : device_(d) {
    try {
        auto allocate = [&](Buffer &b, uint64_t size, VkBufferUsageFlags usage, bool host = false) {
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            ci.size = size;
            ci.usage = usage;
            VkBuffer temp{};
            check(vkCreateBuffer(d, &ci, nullptr, &temp));
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(d, temp, &req);
            vkDestroyBuffer(d, temp, nullptr);
            if (req.size > budget - bytes)
                throw std::runtime_error("Interaction allocation budget exhausted");
            b.create(physical, d, size, usage, host);
            bytes += b.bytes;
        };
        for (auto &f : fields_)
            allocate(f, 128 * 128 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for (auto &u : uniforms_)
            allocate(u, sizeof(Uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        if (diagnostics)
            allocate(readback_, 128 * 128 * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3;
        li.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(d, &li, nullptr, &setLayout_));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout_;
        check(vkCreatePipelineLayout(d, &pl, nullptr, &layout_));
        ShaderModule shader(d, shaders / "vegetation_interaction.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = layout_;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    nullptr,
                    0,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    shader.handle,
                    "main",
                    nullptr};
        check(vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &pipeline_));
        for (uint32_t i = 0; i < 2; ++i) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &setLayout_;
            check(vkAllocateDescriptorSets(d, &ai, &sets_[i]));
            descriptor(d, sets_[i], 2, uniforms_[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        }
    } catch (...) {
        if (pipeline_)
            vkDestroyPipeline(d, pipeline_, nullptr);
        if (layout_)
            vkDestroyPipelineLayout(d, layout_, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(d, pool_, nullptr);
        if (setLayout_)
            vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
        throw;
    }
}
VulkanVegetationInteraction::~VulkanVegetationInteraction() {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
}
void VulkanVegetationInteraction::prepare(uint32_t frame, WorldPosition camera, WorldPosition oldCamera,
                                          const VegetationSettings &s) {
    frame_ = frame;
    current_ = (previous_ + 1) % 3;
    origin_ = vegetationFieldOrigin(s.contacts.anchor);
    time_ = s.time;
    generation_ = s.contacts.reset;
    active_ = s.enabled && s.interaction.enabled && s.contacts.active;
    auto delta = relativePosition(origin_, previousOrigin_);
    float dt = time_ - previousTime_;
    reset = !previousActive_ || generation_ != previousGeneration_ || dt < 0 || dt > .25f ||
            std::abs(delta.x) >= 32 || std::abs(delta.z) >= 32;
    auto cur = relativePosition(origin_, camera), old = relativePosition(previousOrigin_, oldCamera);
    currentMapping = {float(cur.x), float(cur.z), active_ ? 1.f : 0.f, reset ? 1.f : 0.f};
    previousMapping = {float(old.x), float(old.z), previousActive_ ? 1.f : 0.f, 0};
    Uniform u{};
    if (!reset)
        u.scroll = {int(std::lround(delta.x * 4)), int(std::lround(delta.z * 4)), 0, 0};
    dt = std::clamp(dt, 0.f, .25f);
    u.response = {std::exp2(-dt / s.interaction.recoveryHalfLife), 1 - std::exp(-dt / .05f), 0, 0};
    u.control = {s.contacts.count, reset ? 1u : 0u, active_ ? 1u : 0u, 0};
    for (uint32_t i = 0; i < s.contacts.count; ++i) {
        auto a = relativePosition(s.contacts.shapes[i].a, origin_),
             b = relativePosition(s.contacts.shapes[i].b, origin_);
        // Inputs far outside the field need no float precision or shader work.
        if (std::max({std::abs(a.x), std::abs(a.z), std::abs(b.x), std::abs(b.z)}) > 1e6)
            continue;
        u.endpoints[i] = {a.x, a.z, b.x, b.z};
        u.properties[i] = {s.contacts.shapes[i].radius, s.contacts.shapes[i].strength, 0, 0};
    }
    std::memcpy(uniforms_[frame].mapped, &u, sizeof(u));
    descriptor(device_, sets_[frame], 0, fields_[previous_]);
    descriptor(device_, sets_[frame], 1, fields_[current_]);
}
void VulkanVegetationInteraction::record(VkCommandBuffer cmd) {
    // Includes readers from earlier queue submissions before a ring slot is reused.
    barrier(cmd,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[frame_], 0, nullptr);
    vkCmdDispatch(cmd, 16, 16, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}
void VulkanVegetationInteraction::submitted() {
    previous_ = current_;
    previousOrigin_ = origin_;
    previousTime_ = time_;
    previousGeneration_ = generation_;
    previousActive_ = active_;
}
void VulkanVegetationInteraction::copySnapshot(VkCommandBuffer cmd) {
    if (!readback_.buffer)
        return;
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy c{0, 0, 128 * 128 * 8};
    vkCmdCopyBuffer(cmd, fields_[previous_].buffer, readback_.buffer, 1, &c);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
}
std::vector<glm::vec2> VulkanVegetationInteraction::snapshot() const {
    if (!readback_.mapped)
        return {};
    auto *p = static_cast<const glm::vec2 *>(readback_.mapped);
    return {p, p + 128 * 128};
}
} // namespace ow

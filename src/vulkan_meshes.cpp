#include "vulkan_meshes.hpp"
#include "render_resources.hpp"
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
namespace ow {
using namespace render_detail;
namespace {
struct Geometry {
    Buffer data, staging;
    uint32_t count = 0;
    uint64_t indexOffset = 0;
    glm::vec3 centre{0}, lo{0}, hi{0};
    bool uploaded = false;
};
struct InstanceRecord {
    glm::mat4 transform{1}, normalTransform{1};
    glm::uvec4 options{0};
    glm::mat4 previousTransform{1};
};
static_assert(sizeof(InstanceRecord) == 208);
struct Draw {
    uint32_t record = 0, variant = 0;
    float depth = 0;
    uint32_t identity = 0;
    std::shared_ptr<Geometry> geometry;
    VisibilityBounds bounds;
    uint32_t mesh = 0, batch = ~0u, shadow = ~0u;
};
} // namespace
struct VulkanMeshes::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    SceneCapacity capacity;
    VulkanMaterials &materials;
    ScenePool<std::shared_ptr<Geometry>, MeshHandle> meshes;
    std::vector<std::weak_ptr<Geometry>> allocations;
    struct Frame {
        bool temporal = false;
        Buffer records;
        VkDescriptorSet set{};
        std::vector<Draw> opaque, blend;
    };
    std::array<Frame, 2> frames;
    VkDescriptorSetLayout setLayout{};
    VkDescriptorPool pool{};
    VkPipelineLayout layout{};
    std::array<VkPipeline, 40> pipelines{};
    uint32_t skipped = 0;
    bool temporalAttachments = true;
    struct History {
        MeshInstance instance;
        uint32_t generation = 0;
        bool valid = false;
    };
    std::vector<History> history, pending;
    Impl(VkPhysicalDevice p, VkDevice d, const SceneCapacity &c, VulkanMaterials &m)
        : physical(p), device(d), capacity(c), materials(m), meshes(c.meshes), allocations(c.meshes),
          history(c.instances), pending(c.instances) {}
    ~Impl() {
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
    uint64_t bytes() const {
        uint64_t n = 0;
        for (const auto &w : allocations)
            if (auto p = w.lock())
                n += p->data.bytes;
        return n;
    }
    void pipeline(uint32_t pass, uint32_t variant, VkShaderModule vs, VkShaderModule fs,
                  bool temporal = false) {
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto &s : stages) {
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        VkVertexInputBindingDescription bind{0, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, tangent)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)}};
        VkPipelineVertexInputStateCreateInfo input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &bind;
        input.vertexAttributeDescriptionCount = 4;
        VkVertexInputAttributeDescription shadowAttrs[] = {attrs[0], attrs[3]};
        input.pVertexAttributeDescriptions = pass == 4 ? shadowAttrs : attrs;
        if (pass == 4)
            input.vertexAttributeDescriptionCount = 2;
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.lineWidth = 1;
        raster.cullMode = variant & 1 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        raster.frontFace = variant & 2 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = true;
        depth.depthWriteEnable = pass == 0 || pass == 2 || pass == 4;
        depth.depthCompareOp = pass == 1 ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 15;
        attachment.blendEnable = pass == 3;
        attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        std::array<VkPipelineColorBlendAttachmentState, 3> attachments{attachment, attachment,
                                                                       attachment};
        if (temporal && pass == 3) {
            attachments[1].blendEnable = false;
            attachments[1].colorWriteMask = 0;
            attachments[2].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachments[2].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachments[2].colorBlendOp = VK_BLEND_OP_MAX;
            attachments[2].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachments[2].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachments[2].alphaBlendOp = VK_BLEND_OP_MAX;
        }
        blend.attachmentCount = (pass == 2 || pass == 4) ? 0 : temporal ? 3 : 1;
        blend.pAttachments = attachments.data();
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;
        VkFormat colours[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                              temporalAttachments ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = blend.attachmentCount;
        rendering.pColorAttachmentFormats = colours;
        rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &input;
        ci.pInputAssemblyState = &assembly;
        ci.pViewportState = &viewport;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &depth;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dynamic;
        ci.layout = layout;
        check(vkCreateGraphicsPipelines(device, {}, 1, &ci, nullptr,
                                        &pipelines[pass * 4 + variant + (temporal ? 20 : 0)]));
    }
};
VulkanMeshes::VulkanMeshes(VkPhysicalDevice p, VkDevice d, const SceneCapacity &c,
                           VkDescriptorSetLayout lighting, VulkanMaterials &m,
                           const std::filesystem::path &shaders, bool bindless,
                           VkDescriptorSetLayout shadowLayout, bool temporalAttachments)
    : instances(c.instances), impl_(std::make_unique<Impl>(p, d, c, m)) {
    if (!c.instances || c.instances > 65535 || !c.meshes || c.meshes > 4096)
        throw std::invalid_argument("Invalid mesh capacities");
    auto &s = *impl_;
    s.temporalAttachments = temporalAttachments;
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.setLayout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    check(vkCreateDescriptorPool(d, &pi, nullptr, &s.pool));
    for (auto &f : s.frames) {
        f.records.create(p, d, uint64_t(c.instances) * sizeof(InstanceRecord),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.opaque.reserve(c.instances);
        f.blend.reserve(c.instances);
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        a.descriptorPool = s.pool;
        a.descriptorSetCount = 1;
        a.pSetLayouts = &s.setLayout;
        check(vkAllocateDescriptorSets(d, &a, &f.set));
        descriptor(d, f.set, 0, f.records);
    }
    VkDescriptorSetLayout layouts[] = {s.setLayout, lighting, m.layout(), shadowLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 4;
    pl.pSetLayouts = layouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &s.layout));
    ShaderModule vs(d, shaders / "mesh.vert.spv");
    ShaderModule fs(d, shaders / (bindless ? "mesh_bindless.frag.spv" : "mesh.frag.spv"));
    ShaderModule depth(d, shaders / (bindless ? "mesh_depth_bindless.frag.spv" : "mesh_depth.frag.spv"));
    ShaderModule sv(d, shaders / "shadow_mesh.vert.spv");
    ShaderModule sf(d, shaders / (bindless ? "shadow_mesh_bindless.frag.spv" : "shadow_mesh.frag.spv"));
    if (temporalAttachments) {
        ShaderModule temporalFs(
            d, shaders / (bindless ? "mesh_bindless_temporal.frag.spv" : "mesh_temporal.frag.spv"));
        for (uint32_t pass : {0u, 1u, 3u})
            for (uint32_t v = 0; v < 4; ++v)
                s.pipeline(pass, v, vs.handle, temporalFs.handle, true);
    }
    for (uint32_t v = 0; v < 4; ++v)
        s.pipeline(4, v, sv.handle, sf.handle);
    for (uint32_t pass = 0; pass < 4; ++pass)
        for (uint32_t v = 0; v < 4; ++v)
            s.pipeline(pass, v, vs.handle, pass == 2 ? depth.handle : fs.handle);
}
VulkanMeshes::~VulkanMeshes() = default;
MeshHandle VulkanMeshes::upload(const MeshData &data) {
    auto &s = *impl_;
    if (data.vertices.empty() || data.indices.empty() || data.indices.size() % 3 ||
        data.vertices.size() > UINT32_MAX || data.indices.size() > UINT32_MAX)
        throw std::invalid_argument("Invalid triangle mesh");
    glm::vec3 lo(INFINITY), hi(-INFINITY);
    for (const auto &v : data.vertices) {
        for (float x : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                        v.tangent.x, v.tangent.y, v.tangent.z, v.tangent.w, v.uv.x, v.uv.y})
            if (!std::isfinite(x))
                throw std::invalid_argument("Nonfinite mesh vertex");
        if (glm::any(glm::greaterThan(glm::abs(v.position), glm::vec3(1e6))) ||
            glm::any(glm::greaterThan(glm::abs(v.uv), glm::vec2(1e6))) ||
            !std::isfinite(glm::dot(v.normal, v.normal)) || glm::dot(v.normal, v.normal) > 1e6f ||
            !std::isfinite(glm::dot(glm::vec3(v.tangent), glm::vec3(v.tangent))) ||
            glm::dot(glm::vec3(v.tangent), glm::vec3(v.tangent)) > 1e6f ||
            glm::dot(glm::cross(v.normal, glm::vec3(v.tangent)),
                     glm::cross(v.normal, glm::vec3(v.tangent))) < 1e-12f ||
            glm::dot(v.normal, v.normal) < 1e-12f ||
            glm::dot(glm::vec3(v.tangent), glm::vec3(v.tangent)) < 1e-12f || std::abs(v.tangent.w) != 1)
            throw std::invalid_argument("Invalid mesh normal/tangent");
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    for (auto i : data.indices)
        if (i >= data.vertices.size())
            throw std::invalid_argument("Mesh index out of range");
    auto free = std::find_if(s.allocations.begin(), s.allocations.end(),
                             [](const auto &p) { return p.expired(); });
    if (free == s.allocations.end())
        return {};
    uint64_t vertexBytes = data.vertices.size() * sizeof(MeshVertex),
             bytes = vertexBytes + data.indices.size() * 4, used = s.bytes();
    if (bytes > s.capacity.geometryBytes || used > s.capacity.geometryBytes - bytes)
        return {};
    auto g = std::make_shared<Geometry>();
    g->data.create(s.physical, s.device, bytes,
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (g->data.bytes > s.capacity.geometryBytes - used)
        return {};
    g->staging.create(s.physical, s.device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(g->staging.mapped, data.vertices.data(), vertexBytes);
    std::memcpy(static_cast<char *>(g->staging.mapped) + vertexBytes, data.indices.data(),
                data.indices.size() * 4);
    g->centre = (lo + hi) * .5f;
    g->lo = lo;
    g->hi = hi;
    g->indexOffset = vertexBytes;
    g->count = uint32_t(data.indices.size());
    auto h = s.meshes.create(g);
    if (h.slot != ~0u)
        *free = g;
    return h;
}
bool VulkanMeshes::release(MeshHandle h) { return impl_->meshes.remove(h); }
void VulkanMeshes::prepare(uint32_t frame, const Camera &camera, TerrainGrid terrain,
                           const Camera &previous, bool historyValid, bool temporal) {
    auto &s = *impl_;
    auto &f = s.frames.at(frame);
    for (auto &d : f.opaque)
        if (d.geometry->uploaded)
            d.geometry->staging.reset();
    for (auto &d : f.blend)
        if (d.geometry->uploaded)
            d.geometry->staging.reset();
    f.opaque.clear();
    f.blend.clear();
    s.skipped = 0;
    auto *out = static_cast<InstanceRecord *>(f.records.mapped);
    auto origin = worldPosition(camera, terrain);
    f.temporal = temporal;
    auto previousOrigin = worldPosition(previous, terrain);
    for (auto &h : s.pending)
        h.valid = false;
    uint32_t n = 0, dense = 0;
    for (const auto &i : instances.values()) {
        auto handle = instances.handle(dense++);
        auto identity = handle.slot;
        auto *geometry = s.meshes.get(i.mesh);
        if (!geometry) {
            ++s.skipped;
            continue;
        }
        auto rotation = glm::mat4_cast(glm::normalize(i.rotation));
        auto transform = glm::translate(glm::mat4(1), glm::vec3(relativePosition(i.position, origin))) *
                         rotation * glm::scale(glm::mat4(1), i.scale);
        bool mirror = i.scale.x * i.scale.y * i.scale.z < 0;
        auto &m = s.materials.meshMaterial(i.material);
        const auto &old = s.history[identity];
        bool valid = historyValid && old.valid && old.generation == handle.generation &&
                     old.instance.mesh.slot == i.mesh.slot &&
                     old.instance.mesh.generation == i.mesh.generation &&
                     old.instance.material.slot == i.material.slot &&
                     old.instance.material.generation == i.material.generation;
        auto previousTransform =
            valid ? glm::translate(glm::mat4(1),
                                   glm::vec3(relativePosition(old.instance.position, previousOrigin))) *
                        glm::mat4_cast(glm::normalize(old.instance.rotation)) *
                        glm::scale(glm::mat4(1), old.instance.scale)
                  : transform;
        s.pending[identity] = {i, handle.generation, true};
        out[n] = {transform,
                  glm::transpose(glm::inverse(transform)),
                  {s.materials.resolve(i.material), mirror ? 1u : 0u, valid ? 1u : 0u, 0},
                  previousTransform};
        const auto centre = glm::vec3(transform * glm::vec4((*geometry)->centre, 1));
        Draw draw{n++, uint32_t(m.doubleSided) | (mirror ? 2u : 0u), glm::dot(centre, camera.forward()),
                  identity, *geometry};
        draw.bounds = transformBounds((*geometry)->lo, (*geometry)->hi, transform);
        draw.mesh = i.mesh.slot;
        (m.alpha == AlphaMode::Blend ? f.blend : f.opaque).push_back(std::move(draw));
    }
    std::sort(f.blend.begin(), f.blend.end(), [](const auto &a, const auto &b) {
        return a.depth != b.depth ? a.depth > b.depth : a.identity < b.identity;
    });
}
void VulkanMeshes::prepareVisibility(uint32_t frame, VulkanVisibility &visibility) {
    auto &draws = impl_->frames[frame].opaque;
    std::sort(draws.begin(), draws.end(), [](const Draw &a, const Draw &b) {
        if (a.mesh != b.mesh)
            return a.mesh < b.mesh;
        if (a.variant != b.variant)
            return a.variant < b.variant;
        return a.record < b.record;
    });
    uint32_t mesh = ~0u, variant = ~0u;
    for (auto &d : draws) {
        d.batch = visibility.append(d.bounds, d.geometry->count, 0, d.record, nullptr, 0,
                                    d.mesh != mesh || d.variant != variant);
        mesh = d.mesh;
        variant = d.variant;
    }
}
void VulkanMeshes::uploads(VkCommandBuffer cmd, uint32_t frame) {
    auto &f = impl_->frames[frame];
    bool wrote = false;
    const auto upload = [&](const Draw &d) {
        auto &g = *d.geometry;
        if (g.uploaded)
            return;
        VkBufferCopy copy{0, 0, g.data.size};
        vkCmdCopyBuffer(cmd, g.staging.buffer, g.data.buffer, 1, &copy);
        g.uploaded = true;
        wrote = true;
    };
    for (const auto &d : f.opaque)
        upload(d);
    for (const auto &d : f.blend)
        upload(d);
    if (wrote)
        barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
}
void VulkanMeshes::draw(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet lighting, uint32_t pass,
                        VulkanVisibility *visibility) {
    auto &s = *impl_;
    auto &f = s.frames[frame];
    VkDescriptorSet sets[] = {f.set, lighting, s.materials.set(frame)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 3, sets, 0, nullptr);
    uint32_t previous = ~0u;
    const bool indirect = pass != 3 && visibility && visibility->indirect();
    for (const auto &d : pass == 3 ? f.blend : f.opaque) {
        if (indirect && previous == d.batch)
            continue;
        previous = d.batch;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          s.pipelines[pass * 4 + d.variant + (f.temporal && pass != 2 ? 20 : 0)]);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &d.geometry->data.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, d.geometry->data.buffer, d.geometry->indexOffset,
                             VK_INDEX_TYPE_UINT32);
        const uint32_t record = indirect ? 0x80000000u : d.record;
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &record);
        if (indirect)
            visibility->draw(cmd, d.batch);
        else
            vkCmdDrawIndexed(cmd, d.geometry->count, 1, 0, 0, 0);
    }
}
void VulkanMeshes::prepareShadows(uint32_t frame, VulkanShadows &shadows) {
    for (auto &d : impl_->frames[frame].opaque)
        d.shadow = shadows.append(d.bounds, d.geometry->count, 0, d.record);
}
void VulkanMeshes::drawShadows(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet lighting,
                               VulkanShadows &shadows) {
    auto &s = *impl_;
    auto &f = s.frames[frame];
    VkDescriptorSet sets[] = {f.set, lighting, s.materials.set(frame)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 3, sets, 0, nullptr);
    shadows.bind(cmd, s.layout);
    for (size_t i = 0; i < f.opaque.size();) {
        const auto &d = f.opaque[i];
        if (d.shadow == ~0u) {
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < f.opaque.size() && f.opaque[end].geometry == d.geometry &&
               f.opaque[end].variant == d.variant && f.opaque[end].shadow == d.shadow + end - i)
            ++end;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipelines[16 + d.variant]);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &d.geometry->data.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, d.geometry->data.buffer, d.geometry->indexOffset,
                             VK_INDEX_TYPE_UINT32);
        shadows.drawRange(cmd, d.shadow, uint32_t(end - i));
        i = end;
    }
}
uint64_t VulkanMeshes::bytes() const { return impl_->bytes(); }
uint32_t VulkanMeshes::skipped() const { return impl_->skipped; }
} // namespace ow

namespace ow {
void VulkanMeshes::submitted() { impl_->history.swap(impl_->pending); }
bool VulkanMeshes::invalidateHistory(InstanceHandle h) {
    if (!instances.valid(h))
        return false;
    impl_->history[h.slot].valid = false;
    return true;
}
} // namespace ow

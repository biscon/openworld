#include "vulkan_vegetation.hpp"
#include "render_resources.hpp"
#include <chrono>
#include <cmath>
namespace ow {
using namespace render_detail;
namespace {
constexpr uint32_t workWords = 97 * vegetationViews, queryCount = 16;
struct Uniform {
    std::array<glm::mat4, 6> views{};
    glm::vec4 cameraTime{}, previousCameraTime{}, settings{}, previousSettings{}, cards{}, previousCards{};
    glm::uvec4 modes{}, limits{};
    std::array<glm::uvec4, 12> geometry{};
};
static_assert(sizeof(Uniform) == 704);
struct Cell {
    glm::vec4 lo{0}, hi{0};
};
} // namespace
struct VulkanVegetation::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    VulkanMaterials &materials;
    VkDescriptorSetLayout setLayout{};
    VkDescriptorPool pool{};
    VkPipelineLayout layout{};
    VkPipeline compute{};
    std::array<VkPipeline, 6> pipelines{};
    VkQueryPool queries{};
    bool temporalAttachments = false, uploaded = false, history = false;
    Buffer instances, geometry, cells, staging, diagnostic;
    uint64_t indexOffset = 0, bytes = 0, budget = 0, serial = 0;
    uint32_t count = 0, cellCount = 0, listCapacity = 0, frame = 0, lastFrame = 0, timestampBits = 0;
    double timestampPeriod = 0;
    std::array<glm::uvec4, 12> geometryInfo{};
    std::vector<Cell> cellBounds;
    struct Frame {
        Buffer uniform, lists, work, readback;
        VkDescriptorSet set{};
        VegetationStats stats;
        uint32_t stamped = 0;
        bool submitted = false, temporal = false;
    };
    std::array<Frame, 2> frames;
    Uniform current{}, previous{};
    VegetationStats last;
    TextureHandle texture;
    MaterialHandle bladeMaterial, cardMaterial;
    Impl(VkPhysicalDevice p, VkDevice d, VulkanMaterials &m) : physical(p), device(d), materials(m) {}
    ~Impl() {
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (compute)
            vkDestroyPipeline(device, compute, nullptr);
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        materials.releaseMaterial(bladeMaterial);
        materials.releaseMaterial(cardMaterial);
        materials.releaseTexture(texture);
    }
    void allocate(Buffer &b, uint64_t size, VkBufferUsageFlags usage, bool host = false) {
        // Query driver padding before committing each allocation.
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = std::max<uint64_t>(size, 16);
        ci.usage = usage;
        VkBuffer temporary{};
        check(vkCreateBuffer(device, &ci, nullptr, &temporary));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, temporary, &req);
        vkDestroyBuffer(device, temporary, nullptr);
        if (req.size > budget - bytes)
            throw std::runtime_error("Vegetation allocation budget exhausted");
        b.create(physical, device, ci.size, usage, host);
        bytes += b.bytes;
    }
    void stamp(VkCommandBuffer cmd, uint32_t index) {
        if (queries) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries,
                                 frame * queryCount + index);
            frames[frame].stamped |= 1u << index;
        }
    }
    VegetationStats decode(uint32_t slot) const {
        const auto &f = frames[slot];
        auto result = f.stats;
        result.completed = f.submitted;
        if (!result.enabled)
            return result;
        auto *w = static_cast<const uint32_t *>(f.readback.mapped);
        for (uint32_t view = 0; view < 6; ++view) {
            result.overflow += w[view * 97 + 96];
            for (uint32_t b = 0; b < 12; ++b) {
                auto n = w[view * 97 + b * 8 + 1];
                if (view == 0) {
                    result.visible += n;
                    result.triangles += uint64_t(n) * geometryInfo[b].x / 3;
                } else
                    result.casters += n;
            }
        }
        if (queries) {
            std::array<uint64_t, queryCount * 2> t{};
            vkGetQueryPoolResults(device, queries, slot * queryCount, queryCount, sizeof(t), t.data(), 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            result.timingsAvailable = true;
            uint64_t mask = timestampBits == 64 ? ~0ull : (1ull << timestampBits) - 1;
            for (uint32_t q = 0; q < queryCount; q += 2) {
                if (!(f.stamped & (1u << q)))
                    continue;
                if (!t[q * 2 + 1] || !t[(q + 1) * 2 + 1]) {
                    result.timingsAvailable = false;
                    continue;
                }
                double ms = double((t[(q + 1) * 2] - t[q * 2]) & mask) * timestampPeriod / 1e6;
                if (q == 0)
                    result.selectionMilliseconds = ms;
                else if (q == 2)
                    result.depthMilliseconds = ms;
                else if (q == 4)
                    result.colourMilliseconds = ms;
                else
                    result.shadowMilliseconds += ms;
            }
        }
        return result;
    }
    void pipeline(uint32_t pass, VkShaderModule vs, VkShaderModule fs, bool temporal = false) {
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
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &bind;
        input.vertexAttributeDescriptionCount = 4;
        input.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.lineWidth = 1;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = true;
        depth.depthWriteEnable = pass == 0 || pass == 2 || pass == 4;
        depth.depthCompareOp = pass == 1 ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_GREATER;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 15;
        attachment.blendEnable = false;
        attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        std::array<VkPipelineColorBlendAttachmentState, 3> attachments{attachment, attachment, attachment};

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
                                        &pipelines[pass == 4 ? 3 : pass + (temporal ? 4 : 0)]));
    }
};
VulkanVegetation::VulkanVegetation(VkPhysicalDevice p, VkDevice d, uint32_t family,
                                   VkDescriptorSetLayout lighting, VulkanMaterials &m,
                                   const std::filesystem::path &shaders, bool bindless,
                                   bool temporalAttachments, const VegetationOptions &options)
    : impl_(std::make_unique<Impl>(p, d, m)) {
    auto &s = *impl_;
    const auto &fixture = *options.fixture;
    if (fixture.instances.size() > vegetationMaximum || fixture.instances.size() % 128 ||
        fixture.side > 256 || !fixture.side || fixture.side % 4)
        throw std::invalid_argument("Invalid vegetation population");
    s.budget = options.budgetBytes;
    s.count = uint32_t(fixture.instances.size());
    s.cellCount = (s.count + 127) / 128;
    s.listCapacity = options.listCapacity ? options.listCapacity : std::max(1u, s.count);
    if (s.listCapacity > vegetationMaximum)
        throw std::invalid_argument("Invalid vegetation list capacity");
    s.temporalAttachments = temporalAttachments;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    for (uint32_t b = 0; b < 12; ++b) {
        const auto &mesh = fixture.assets.meshes[b];
        uint32_t base = uint32_t(vertices.size());
        if (mesh.vertices.empty() || mesh.vertices.size() > 4096 || mesh.indices.empty() ||
            mesh.indices.size() > 16384 || mesh.indices.size() % 3)
            throw std::invalid_argument("Invalid vegetation geometry");
        for (const auto &v : mesh.vertices) {
            for (float x : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                            v.tangent.x, v.tangent.y, v.tangent.z, v.tangent.w, v.uv.x, v.uv.y})
                if (!std::isfinite(x))
                    throw std::invalid_argument("Nonfinite vegetation vertex");
            if (v.position.y < 0 || v.position.y > 1.001f ||
                glm::length(glm::vec2(v.position.x, v.position.z)) > .401f)
                throw std::invalid_argument("Vegetation mesh exceeds conservative envelope");
        }
        s.geometryInfo[b] = {uint32_t(mesh.indices.size()), uint32_t(indices.size()), 0, 0};
        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (auto i : mesh.indices) {
            if (i >= mesh.vertices.size())
                throw std::invalid_argument("Vegetation index out of bounds");
            indices.push_back(i + base);
        }
    }
    s.indexOffset = vertices.size() * sizeof(MeshVertex);
    auto geometryBytes = s.indexOffset + indices.size() * 4,
         instanceBytes = fixture.instances.size() * sizeof(VegetationInstance);
    s.cellBounds.resize(s.cellCount);
    for (uint32_t c = 0; c < s.cellCount; ++c) {
        auto &cell = s.cellBounds[c];
        cell.lo = glm::vec4(INFINITY);
        cell.hi = glm::vec4(-INFINITY);
        for (uint32_t i = c * 128; i < (c + 1) * 128; ++i) {
            auto &plant = fixture.instances[i];
            if (plant.identity != i || plant.cell != c || !std::isfinite(plant.rootScale.x) ||
                !std::isfinite(plant.rootScale.y) || !std::isfinite(plant.rootScale.z) ||
                plant.rootScale.w < .3f || plant.rootScale.w > .6f || !std::isfinite(plant.rootScale.w) ||
                !std::isfinite(plant.yaw) || !std::isfinite(plant.variation))
                throw std::invalid_argument("Invalid vegetation instance");
            cell.lo = glm::min(cell.lo, glm::vec4(glm::vec3(plant.rootScale) - glm::vec3(.9, .05, .9), 0));
            cell.hi = glm::max(cell.hi, glm::vec4(glm::vec3(plant.rootScale) + glm::vec3(.9, .65, .9), 0));
        }
    }
    auto cellBytes = s.cellBounds.size() * sizeof(Cell);
    s.allocate(s.instances, instanceBytes,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    s.allocate(s.geometry, geometryBytes,
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    s.allocate(s.cells, cellBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    s.allocate(s.staging, geometryBytes + instanceBytes + cellBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    auto *data = static_cast<char *>(s.staging.mapped);
    std::memcpy(data, vertices.data(), s.indexOffset);
    std::memcpy(data + s.indexOffset, indices.data(), indices.size() * 4);
    std::memcpy(data + geometryBytes, fixture.instances.data(), instanceBytes);
    std::memcpy(data + geometryBytes + instanceBytes, s.cellBounds.data(), cellBytes);
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 5; ++i)
        bindings[i] = {i, i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.setLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &pi, nullptr, &s.pool));
    for (auto &f : s.frames) {
        s.allocate(f.uniform, sizeof(Uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        s.allocate(f.lists, uint64_t(s.listCapacity) * 6 * 4,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        s.allocate(f.work, workWords * 4,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        // Full lists are copied only for explicit snapshots, never during timed
        // rendering.
        s.allocate(f.readback, workWords * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &s.setLayout;
        check(vkAllocateDescriptorSets(d, &ai, &f.set));
        descriptor(d, f.set, 0, s.instances);
        descriptor(d, f.set, 1, f.uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        descriptor(d, f.set, 2, f.lists);
        descriptor(d, f.set, 3, f.work);
        descriptor(d, f.set, 4, s.cells);
    }
    s.allocate(s.diagnostic, uint64_t(s.listCapacity) * 6 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    auto required = m.textureAllocationRequirements(fixture.assets.cards);
    if (required > s.budget - s.bytes)
        throw std::runtime_error("Vegetation texture exceeds budget");
    s.texture = m.uploadTexture(fixture.assets.cards);
    if (s.texture.slot == ~0u)
        throw std::runtime_error("Vegetation texture capacity exhausted");
    s.bytes += required;
    MeshMaterial material;
    material.doubleSided = true;
    material.baseColour = {.105, .23, .035, 1};
    material.roughness = .85f;
    s.bladeMaterial = m.createMaterial(material);
    material.baseColour = {1, 1, 1, 1};
    material.colourTexture = s.texture;
    material.alpha = AlphaMode::Mask;
    s.cardMaterial = m.createMaterial(material);
    if (s.bladeMaterial.slot == ~0u || s.cardMaterial.slot == ~0u)
        throw std::runtime_error("Vegetation material capacity exhausted");
    for (uint32_t b = 0; b < 12; ++b)
        s.geometryInfo[b].z = m.resolve(b % 6 >= 3 ? s.cardMaterial : s.bladeMaterial);
    VkDescriptorSetLayout layouts[] = {s.setLayout, lighting, m.layout()};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 3;
    pl.pSetLayouts = layouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &s.layout));
    ShaderModule cs(d, shaders / "vegetation_select.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = s.layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                cs.handle,
                "main",
                nullptr};
    check(vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &s.compute));
    ShaderModule vs(d, shaders / "vegetation.vert.spv"),
        fs(d, shaders / (bindless ? "vegetation_bindless.frag.spv" : "vegetation.frag.spv")),
        depth(d, shaders / (bindless ? "vegetation_depth_bindless.frag.spv" : "vegetation_depth.frag.spv"));
    for (uint32_t pass : {0u, 1u, 2u, 4u})
        s.pipeline(pass, vs.handle, pass >= 2 ? depth.handle : fs.handle);
    if (temporalAttachments) {
        ShaderModule temporal(d, shaders / (bindless ? "vegetation_bindless_temporal.frag.spv"
                                                     : "vegetation_temporal.frag.spv"));
        s.pipeline(0, vs.handle, temporal.handle, true);
        s.pipeline(1, vs.handle, temporal.handle, true);
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(p, &props);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &families, nullptr);
    std::vector<VkQueueFamilyProperties> queues(families);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &families, queues.data());
    s.timestampBits = queues.at(family).timestampValidBits;
    if (s.timestampBits && props.limits.timestampComputeAndGraphics) {
        s.timestampPeriod = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = queryCount * 2;
        check(vkCreateQueryPool(d, &qi, nullptr, &s.queries));
    }
    s.current.limits = {s.listCapacity, 1, fixture.side, 0};
    s.last.allocatedBytes = s.bytes;
}
VulkanVegetation::~VulkanVegetation() = default;
void VulkanVegetation::prepare(uint32_t frame, const Camera &camera, TerrainGrid grid,
                               const TemporalView &temporal, const VegetationSettings &settings) {
    auto start = std::chrono::steady_clock::now();
    validate(settings);
    auto &s = *impl_;
    s.frame = frame;
    auto &f = s.frames[frame];
    if (f.submitted)
        s.last = s.decode(frame);
    f.submitted = false;
    f.stamped = 0;
    f.stats = {};
    f.stats.frame = s.serial + 1;
    f.stats.allocatedBytes = s.bytes;
    f.stats.enabled = settings.enabled;
    f.temporal = s.temporalAttachments && temporal.state.x != 0;
    auto position = relativePosition(worldPosition(camera, grid), WorldPosition{});
    s.current.cameraTime = {glm::vec3(position), settings.time};
    s.current.settings = {settings.density, settings.wind, settings.distance, settings.shadowDistance};
    s.current.cards = {settings.cardWidth, settings.cardDensity, settings.coverage ? 1 : 0, 0};
    bool valid = s.history && temporal.state.y != 0 && s.previous.limits.w != 0 &&
                 s.previous.modes.x == settings.representation && s.previous.cards.x == settings.cardWidth;
    s.current.previousCameraTime = s.previous.cameraTime;
    s.current.previousSettings = s.previous.settings;
    s.current.previousCards = s.previous.cards;
    s.current.modes = {settings.representation, s.previous.modes.x, valid ? 1u : 0u, s.count};
    s.current.limits.w = settings.enabled ? 1 : 0;
    s.current.geometry = s.geometryInfo;
    s.current.views[0] = conservativeProjection(temporal.current, glm::uvec2(temporal.pixels));
    f.stats.preparationMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::bounds(VulkanShadows &shadows) {
    auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    if (!s.current.limits.w)
        return;
    for (const auto &c : s.cellBounds) {
        glm::vec3 lo = glm::vec3(c.lo) - glm::vec3(s.current.cameraTime),
                  hi = glm::vec3(c.hi) - glm::vec3(s.current.cameraTime);
        auto nearest = glm::max(glm::max(lo, -hi), glm::vec3(0));
        if (glm::length(nearest) > s.current.settings.w)
            continue;
        shadows.includeCasterBounds({lo, hi, true});
    }
    s.frames[s.frame].stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::select(VkCommandBuffer cmd, const ShadowUniform &shadows, uint32_t cascades) {
    auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled)
        return;
    s.current.limits.y = cascades + 1;
    for (uint32_t i = 0; i < cascades; ++i)
        s.current.views[i + 1] = shadows.matrices[i];
    std::memcpy(f.uniform.mapped, &s.current, sizeof(Uniform));
    if (s.queries)
        vkCmdResetQueryPool(cmd, s.queries, s.frame * queryCount, queryCount);
    s.stamp(cmd, 0);
    if (!s.uploaded) {
        uint64_t geometryBytes = s.geometry.size, instanceBytes = uint64_t(s.count) * 32;
        VkBufferCopy copy{0, 0, geometryBytes};
        vkCmdCopyBuffer(cmd, s.staging.buffer, s.geometry.buffer, 1, &copy);
        if (instanceBytes) {
            copy = {geometryBytes, 0, instanceBytes};
            vkCmdCopyBuffer(cmd, s.staging.buffer, s.instances.buffer, 1, &copy);
        }
        if (s.cellCount) {
            copy = {geometryBytes + instanceBytes, 0, uint64_t(s.cellCount) * sizeof(Cell)};
            vkCmdCopyBuffer(cmd, s.staging.buffer, s.cells.buffer, 1, &copy);
        }
        s.uploaded = true;
    }
    vkCmdFillBuffer(cmd, f.work.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.compute);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 1, &f.set, 0, nullptr);
    for (uint32_t phase = 0; phase < 3; ++phase) {
        for (uint32_t view = 0; view < s.current.limits.y; ++view) {
            uint32_t push[] = {view, 0, phase, 0};
            vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 16,
                               push);
            vkCmdDispatch(cmd, phase == 1 ? 1 : s.cellCount, 1, 1);
        }
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    s.stamp(cmd, 1);
    f.stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::draw(VkCommandBuffer cmd, VkDescriptorSet lighting, uint32_t pass, uint32_t view) {
    auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled)
        return;
    uint32_t query = pass == 4 ? 6 + 2 * (view - 1) : pass == 2 ? 2 : 4;
    s.stamp(cmd, query);
    VkDescriptorSet sets[] = {f.set, lighting, s.materials.set(s.frame)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 3, sets, 0, nullptr);
    const uint32_t pipelineIndex = pass == 4 ? 3 : pass + (f.temporal && pass < 2 ? 4 : 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipelines[pipelineIndex]);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &s.geometry.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, s.geometry.buffer, s.indexOffset, VK_INDEX_TYPE_UINT32);
    for (uint32_t bin = 0; bin < 12; ++bin) {
        uint32_t push[] = {view, bin, 0, 0};
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 16,
                           push);
        vkCmdDrawIndexedIndirect(cmd, f.work.buffer, uint64_t(view * 97 + bin * 8) * 4, 1, 32);
    }
    s.stamp(cmd, query + 1);
    f.stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::finish(VkCommandBuffer cmd) {
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled)
        return;
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy{0, 0, workWords * 4};
    vkCmdCopyBuffer(cmd, f.work.buffer, f.readback.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
}
void VulkanVegetation::submitted() {
    auto &s = *impl_;
    s.previous = s.current;
    s.history = true;
    s.frames[s.frame].submitted = true;
    s.lastFrame = s.frame;
    ++s.serial;
}
const VegetationStats &VulkanVegetation::stats() const { return impl_->last; }
void VulkanVegetation::copySnapshot(VkCommandBuffer cmd) {
    auto &s = *impl_;
    if (!s.frames[s.lastFrame].stats.enabled)
        return;
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy{0, 0, uint64_t(s.listCapacity) * 6 * 4};
    vkCmdCopyBuffer(cmd, s.frames[s.lastFrame].lists.buffer, s.diagnostic.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
}
VegetationSnapshot VulkanVegetation::snapshot() const {
    auto &s = *impl_;
    VegetationSnapshot out;
    out.stats = s.decode(s.lastFrame);
    if (out.stats.enabled) {
        auto *w = static_cast<const uint32_t *>(s.frames[s.lastFrame].readback.mapped);
        for (uint32_t v = 0; v < 6; ++v)
            for (uint32_t b = 0; b < 12; ++b) {
                out.counts[v * 12 + b] = w[v * 97 + b * 8 + 1];
                auto *ids = static_cast<const uint32_t *>(s.diagnostic.mapped) + v * s.listCapacity +
                            w[v * 97 + b * 8 + 7];
                out.identities[v].insert(out.identities[v].end(), ids, ids + out.counts[v * 12 + b]);
            }
    }
    return out;
}
} // namespace ow

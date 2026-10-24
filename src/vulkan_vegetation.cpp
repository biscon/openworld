#include "vulkan_vegetation.hpp"
#include "horizon.hpp"
#include "render_resources.hpp"
#include "vulkan_vegetation_stream.hpp"
#include <chrono>
#include <cmath>
namespace ow {
using namespace render_detail;
namespace {
constexpr uint32_t viewWords = vegetationBinLimit * 8 + 1, workWords = viewWords * vegetationViews,
                   queryCount = 20;
struct Uniform {
    std::array<glm::mat4, 6> views{};
    glm::vec4 cameraTime{}, previousCameraTime{}, settings{}, previousSettings{}, cards{}, previousCards{};
    glm::uvec4 modes{}, limits{}, profile{}, inspection{};
    glm::vec4 wind{}, previousWind{};
    std::array<glm::uvec4, 6> lists{};
    std::array<glm::uvec4, 48> geometry{};
    std::array<glm::vec4, 48> foliage{};
    std::array<glm::vec4, 8> ranges{}, deformation{}, envelope{};
    std::array<glm::uvec4, 24> lods{};
    glm::vec4 curvature{}, detail{};
    glm::mat4 raster{1};
    glm::uvec4 pyramidSize{};
};
static_assert(sizeof(Uniform) == 3088);
using Cell = VegetationGpuCell;
} // namespace
struct VulkanVegetation::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    VulkanMaterials &materials;
    VkDescriptorSetLayout setLayout{};
    VkDescriptorPool pool{};
    VkPipelineLayout layout{};
    VkPipeline compute{}, computeOcclusion{};
    std::array<VkPipeline, 6> pipelines{};
    VkQueryPool queries{};
    bool temporalAttachments = false, uploaded = false, history = false;
    Buffer instances, geometry, casters, staging, diagnostic;
    uint64_t indexOffset = 0, bytes = 0, budget = 0, serial = 0;
    uint32_t count = 0, cellCount = 0, listCapacity = 0, frame = 0, lastFrame = 0, timestampBits = 0;
    double timestampPeriod = 0;
    std::array<glm::uvec4, 48> geometryInfo{};
    bool legacy = true;
    uint32_t bins = 12, cameraGroups = 0, casterGroups = 0, casterCount = 0, listEntries = 0;
    VegetationPopulation population;
    std::unique_ptr<VulkanVegetationStream> streaming;
    Buffer emptyAttachment;
    WorldPosition previousCamera{};
    WorldPosition currentCamera{};
    std::vector<Cell> localCells;
    std::vector<glm::vec4> casterLo, casterHi;
    std::vector<float> casterRange;
    std::vector<TextureHandle> textures;
    std::vector<MaterialHandle> materialHandles;
    std::vector<Cell> cellBounds;
    struct Frame {
        Buffer uniform, lists, work, readback, cells, prefixes, choices, groups;
        VkDescriptorSet set{};
        VegetationStats stats;
        uint32_t stamped = 0;
        bool submitted = false, temporal = false;
    };
    std::array<Frame, 2> frames;
    Uniform current{}, previous{};
    VegetationStats last;

    Impl(VkPhysicalDevice p, VkDevice d, VulkanMaterials &m) : physical(p), device(d), materials(m) {}
    ~Impl() {
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (compute)
            vkDestroyPipeline(device, compute, nullptr);
        if (computeOcclusion)
            vkDestroyPipeline(device, computeOcclusion, nullptr);
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        for (auto h : materialHandles)
            materials.releaseMaterial(h);
        for (auto h : textures)
            materials.releaseTexture(h);
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
            result.overflow += w[view * viewWords + viewWords - 1];
            for (uint32_t b = 0; b < bins; ++b) {
                auto n = w[view * viewWords + b * 8 + 1];
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
                if (q == 0 || q == 18)
                    result.selectionMilliseconds += ms;
                else if (q == 2)
                    result.depthMilliseconds = ms;
                else if (q == 4)
                    result.colourMilliseconds = ms;
                else if (q == 16)
                    result.updateMilliseconds = ms;
                else
                    result.shadowMilliseconds += ms;
            }
        }
        return result;
    }
    void selectView(VkCommandBuffer cmd, uint32_t view, bool occlusion = false) {
        auto &f = frames[frame];
        if (!current.lists[view].w)
            return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, occlusion ? computeOcclusion : compute);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &f.set, 0, nullptr);
        for (uint32_t phase = 0; phase < 4; ++phase) {
            uint32_t push[] = {view, 0, phase, 0};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 16,
                               push);
            const uint32_t n = phase == 1 || phase == 2 ? 1 : current.lists[view].w;
            vkCmdDispatch(cmd, n, 1, 1);
            f.stats.selectionGroups += n;
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
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
        VkVertexInputBindingDescription bind{0, sizeof(FoliageVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FoliageVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FoliageVertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FoliageVertex, tangent)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(FoliageVertex, uv)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(FoliageVertex, deformation)}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &bind;
        input.vertexAttributeDescriptionCount = 5;
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
    s.legacy = !options.library;
    if (bool(options.library) != (bool(options.population) || options.streaming.enabled) ||
        (options.streaming.enabled && (options.population || options.fixture)) ||
        (options.library && options.fixture) || (!options.library && !options.fixture))
        throw std::invalid_argument("Select one vegetation library/population or legacy fixture");
    VegetationLibrary library;
    if (s.legacy) {
        const auto &fixture = *options.fixture;
        if (fixture.instances.size() > vegetationMaximum || fixture.instances.size() % 128 ||
            fixture.side > 256 || !fixture.side || fixture.side % 4)
            throw std::invalid_argument("Invalid legacy vegetation population");
        library.materials = {{{.105f, .23f, .035f, 1}, .85f}, {{1, 1, 1, 1}, .85f, .5f, 0, 0, -1, true}};
        library.textures = {fixture.assets.cards};
        for (uint32_t asset = 0; asset < 4; ++asset) {
            FoliageAsset a;
            a.minimum = {-.401f, 0, -.401f};
            a.maximum = {.401f, 1.001f, .401f};
            for (uint32_t lod = 0; lod < 3; ++lod) {
                const auto &mesh = fixture.assets.meshes[asset * 3 + lod];
                FoliagePart part;
                part.material = asset % 2;
                for (auto &v : mesh.vertices)
                    part.vertices.push_back({v.position,
                                             v.normal,
                                             v.tangent,
                                             v.uv,
                                             {v.position.y, v.position.y * v.position.y, 0, 0}});
                part.indices = mesh.indices;
                a.lods[lod].push_back(std::move(part));
            }
            library.assets.push_back(std::move(a));
        }
        for (uint32_t c = 0; c < fixture.instances.size() / 128; ++c) {
            s.population.cells.push_back({{}, c * 128, 128});
            for (uint32_t i = c * 128; i < (c + 1) * 128; ++i) {
                const auto &v = fixture.instances[i];
                if (v.cell != c || v.identity != i || !std::isfinite(v.yaw) || !std::isfinite(v.variation) ||
                    v.rootScale.w < .3f || v.rootScale.w > .6f)
                    throw std::invalid_argument("Invalid legacy instance");
                for (int k = 0; k < 4; ++k)
                    if (!std::isfinite(v.rootScale[k]))
                        throw std::invalid_argument("Nonfinite legacy instance");
                s.population.instances.push_back(
                    {v.rootScale, v.yaw, v.variation, v.identity, c * 8 + (v.identity % 2) * 2});
            }
        }
        validate(library);
    } else {
        validate(*options.library);
        library = *options.library;
        if (!options.streaming.enabled) {
            validate(*options.population, *options.library);
            s.population = *options.population;
        } else {
            VegetationResidency checked(options.streaming);
            glm::uvec4 assets(~0u);
            for (uint32_t i = 0; i < library.assets.size(); ++i) {
                auto category = library.assets[i].category;
                if (category == FoliageCategory::Grass) {
                    if (assets.x == ~0u)
                        assets.x = i;
                    else if (assets.y == ~0u)
                        assets.y = i;
                }
                if (category == FoliageCategory::Plant && assets.z == ~0u)
                    assets.z = i;
                if (category == FoliageCategory::Shrub && assets.w == ~0u)
                    assets.w = i;
            }
            if (assets.x == ~0u)
                throw std::invalid_argument("Streamed vegetation needs grass");
            for (int k = 1; k < 4; ++k)
                if (assets[k] == ~0u)
                    assets[k] = assets.x;
            for (uint32_t c = 0; c < options.streaming.cellCapacity; ++c) {
                s.population.cells.push_back({{}, c * 128, 128});
                for (uint32_t k = 0; k < 128; ++k) {
                    auto asset = k < 126 ? assets[k % 2] : assets[k - 124];
                    s.population.instances.push_back({{0, 0, 0, 1}, 0, 0, 0, c * 8 + asset});
                }
            }
        }
    }
    s.budget = options.budgetBytes;
    s.count = uint32_t(s.population.instances.size());
    s.cellCount = uint32_t(s.population.cells.size());
    s.temporalAttachments = temporalAttachments;
    s.textures.reserve(library.textures.size());
    s.materialHandles.reserve(library.materials.size());
    for (const auto &texture : library.textures) {
        auto required = m.textureAllocationRequirements(texture);
        if (required > s.budget - s.bytes)
            throw std::runtime_error("Vegetation texture budget exhausted");
        auto h = m.uploadTexture(texture);
        if (h.slot == ~0u)
            throw std::runtime_error("Vegetation texture slots exhausted");
        s.textures.push_back(h);
        s.bytes += required;
    }
    for (const auto &source : library.materials) {
        MeshMaterial material;
        material.doubleSided = true;
        material.baseColour = source.colour;
        material.roughness = source.roughness;
        material.cutoff = source.cutoff;
        material.alpha = source.masked ? AlphaMode::Mask : AlphaMode::Opaque;
        if (source.colourTexture >= 0)
            material.colourTexture = s.textures.at(source.colourTexture);
        if (source.normalTexture >= 0)
            material.normalTexture = s.textures.at(source.normalTexture);
        auto h = m.createMaterial(material);
        if (h.slot == ~0u)
            throw std::runtime_error("Vegetation material slots exhausted");
        s.materialHandles.push_back(h);
    }
    std::vector<FoliageVertex> vertices;
    std::vector<uint32_t> indices;
    s.bins = 0;
    for (uint32_t asset = 0; asset < library.assets.size(); ++asset) {
        const auto &a = library.assets[asset];
        s.current.ranges[asset] = a.ranges;
        s.current.deformation[asset] = {a.stiffness, a.displacement, float(a.category), 0};
        auto horizontalExtent = glm::max(glm::abs(glm::vec2(a.minimum.x, a.minimum.z)),
                                         glm::abs(glm::vec2(a.maximum.x, a.maximum.z)));
        float radius = glm::length(horizontalExtent) + a.displacement;
        s.current.envelope[asset] = {radius, a.minimum.y, a.maximum.y, 0};
        for (uint32_t lod = 0; lod < 3; ++lod) {
            s.current.lods[asset * 3 + lod] = {s.bins, uint32_t(a.lods[lod].size()), 0, 0};
            for (const auto &part : a.lods[lod]) {
                uint32_t base = uint32_t(vertices.size()), bin = s.bins++;
                s.geometryInfo[bin] = {uint32_t(part.indices.size()), uint32_t(indices.size()),
                                       m.resolve(s.materialHandles[part.material]), asset};
                s.current.foliage[bin] = {1, 1, 1, library.materials[part.material].transmission};
                vertices.insert(vertices.end(), part.vertices.begin(), part.vertices.end());
                for (auto i : part.indices)
                    indices.push_back(base + i);
            }
        }
    }
    s.indexOffset = vertices.size() * sizeof(FoliageVertex);
    const uint64_t geometryBytes = s.indexOffset + indices.size() * 4,
                   instanceBytes = s.count * sizeof(FoliageInstance);
    std::vector<glm::uvec4> workGroups, casterWork;
    std::vector<uint32_t> casterIndices;
    s.localCells.resize(s.cellCount);
    s.cellBounds.resize(s.cellCount);
    s.casterRange.resize(s.cellCount, 0);
    s.casterLo.resize(s.cellCount, glm::vec4(INFINITY));
    s.casterHi.resize(s.cellCount, glm::vec4(-INFINITY));
    uint32_t cameraEntries = 0, casterEntries = 0;
    for (uint32_t c = 0; c < s.cellCount; ++c) {
        const auto &source = s.population.cells[c];
        auto &cell = s.localCells[c];
        cell.lo = glm::vec4(INFINITY);
        cell.hi = glm::vec4(-INFINITY);
        auto phase = materialPhase(source.origin.x, source.origin.z, 1024,
                                   glm::dvec2(source.origin.local.x, source.origin.local.z), 256);
        cell.phase = {phase * 256.f, 0, 0};
        for (uint32_t i = source.first; i < source.first + source.count; ++i) {
            const auto &v = s.population.instances[i];
            auto asset = v.assetCell & 7;
            const auto &a = library.assets[asset];
            uint32_t parts = 0;
            for (auto &lod : a.lods)
                parts = std::max(parts, uint32_t(lod.size()));
            cameraEntries += parts;
            auto envelope = s.current.envelope[asset];
            glm::vec3 lo =
                glm::vec3(v.rootScale) + glm::vec3(-envelope.x, envelope.y, -envelope.x) * v.rootScale.w;
            glm::vec3 hi =
                glm::vec3(v.rootScale) + glm::vec3(envelope.x, envelope.z, envelope.x) * v.rootScale.w;
            if (s.legacy) {
                lo = glm::vec3(v.rootScale) - glm::vec3(.9, .05, .9);
                hi = glm::vec3(v.rootScale) + glm::vec3(.9, .65, .9);
            }
            cell.lo = glm::min(cell.lo, glm::vec4(lo, 0));
            cell.hi = glm::max(cell.hi, glm::vec4(hi, 0));
            if (s.legacy || a.ranges.w > 0) {
                s.casterRange[c] = std::max(s.casterRange[c], a.ranges.w);
                casterIndices.push_back(i);
                casterEntries += parts;
                s.casterLo[c] = glm::min(s.casterLo[c], glm::vec4(lo, 0));
                s.casterHi[c] = glm::max(s.casterHi[c], glm::vec4(hi, 0));
            }
        }
        if (!source.count)
            cell.lo = cell.hi = glm::vec4(0);
        for (uint32_t first = source.first; first < source.first + source.count; first += 128)
            workGroups.push_back({first, std::min(128u, source.first + source.count - first), c, 0});
    }
    // Sparse plant/shrub casters share groups across cells instead of leaving
    // almost all lanes idle in each grass-dominated cell.
    for (uint32_t first = 0; first < casterIndices.size(); first += 128)
        casterWork.push_back({first, std::min(128u, uint32_t(casterIndices.size()) - first), 0, 1});
    s.cameraGroups = uint32_t(workGroups.size());
    s.casterGroups = uint32_t(casterWork.size());
    s.casterCount = uint32_t(casterIndices.size());
    workGroups.insert(workGroups.end(), casterWork.begin(), casterWork.end());
    if (options.listCapacity > vegetationMaximum * 2)
        throw std::invalid_argument("Invalid vegetation list capacity");
    if (options.streaming.enabled)
        cameraEntries = s.count * 2; // any grass variant may have two parts
    s.listCapacity = options.listCapacity ? options.listCapacity : std::max(1u, cameraEntries);
    uint32_t offset = 0;
    for (uint32_t view = 0; view < 6; ++view) {
        uint32_t capacity =
            view ? (options.listCapacity ? std::min(options.listCapacity, casterEntries) : casterEntries)
                 : s.listCapacity;
        s.current.lists[view] = {offset, capacity, view ? s.cameraGroups : 0,
                                 view ? s.casterGroups : s.cameraGroups};
        offset += capacity;
    }
    s.listEntries = offset;
    s.allocate(s.instances, instanceBytes,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    s.allocate(s.geometry, geometryBytes,
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    s.allocate(s.casters, casterIndices.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    if (!casterIndices.empty())
        std::memcpy(s.casters.mapped, casterIndices.data(), casterIndices.size() * 4);
    s.allocate(s.staging, geometryBytes + (options.streaming.enabled ? 0 : instanceBytes),
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    auto *data = static_cast<char *>(s.staging.mapped);
    std::memcpy(data, vertices.data(), s.indexOffset);
    std::memcpy(data + s.indexOffset, indices.data(), indices.size() * 4);
    if (instanceBytes && !options.streaming.enabled)
        std::memcpy(data + geometryBytes, s.population.instances.data(), instanceBytes);
    VkDescriptorSetLayoutBinding bindings[11]{};
    for (uint32_t i = 0; i < 10; ++i)
        bindings[i] = {i, i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    li.bindingCount = 11;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &s.setLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &pi, nullptr, &s.pool));
    if (options.streaming.enabled) {
        s.streaming = std::make_unique<VulkanVegetationStream>(
            p, d, shaders, s.instances.buffer, instanceBytes, options.streaming, library, s.budget - s.bytes);
        s.bytes += s.streaming->bytes();
    } else
        s.allocate(s.emptyAttachment, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    for (auto &f : s.frames) {
        const auto groups = std::max(s.cameraGroups, s.casterGroups);
        s.allocate(f.prefixes, uint64_t(groups) * 48 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        s.allocate(f.choices, uint64_t(groups) * 128 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        s.allocate(f.groups, workGroups.size() * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (!workGroups.empty())
            std::memcpy(f.groups.mapped, workGroups.data(), workGroups.size() * 16);
        s.allocate(f.uniform, sizeof(Uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        s.allocate(f.cells, s.cellCount * sizeof(Cell), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        s.allocate(f.lists, uint64_t(s.listEntries) * 4,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        s.allocate(f.work, workWords * 4,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
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
        descriptor(d, f.set, 4, f.cells);
        descriptor(d, f.set, 5, f.groups);
        descriptor(d, f.set, 6, s.casters);
        descriptor(d, f.set, 8, f.prefixes);
        descriptor(d, f.set, 9, f.choices);
        descriptor(d, f.set, 7,
                   s.streaming ? s.streaming->attachments(uint32_t(&f - s.frames.data()))
                               : s.emptyAttachment);
    }
    if (!options.streaming.enabled || options.streaming.diagnostics)
        s.allocate(s.diagnostic, uint64_t(s.listEntries) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
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
    ShaderModule occlusion(d, shaders / "vegetation_select_hiz.comp.spv");
    ci.stage.module = occlusion.handle;
    check(vkCreateComputePipelines(d, {}, 1, &ci, nullptr, &s.computeOcclusion));
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
    s.current.limits = {s.listCapacity, 1, s.bins, 0};
    s.current.profile = {s.legacy ? 1u : 0u, ~0u, ~0u, 1};
    s.last.allocatedBytes = s.bytes;
    if (s.streaming) {
        s.population.instances.clear();
        s.population.instances.shrink_to_fit();
    }
}
VulkanVegetation::~VulkanVegetation() = default;
void VulkanVegetation::prepare(uint32_t frame, const Camera &camera, TerrainGrid grid,
                               const TemporalView &temporal, const VegetationSettings &settings) {
    auto start = std::chrono::steady_clock::now();
    validate(settings);
    auto &s = *impl_;
    s.frame = frame;
    auto &f = s.frames[frame];
    if (f.submitted) {
        s.last = s.decode(frame);
        if (s.streaming && s.last.timingsAvailable)
            s.streaming->observe(s.last.updateMilliseconds, s.last.updatedCells);
    }
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
    s.current.wind = {glm::normalize(settings.windDirection), 0, 0};
    s.current.profile = {s.legacy ? 1u : 0u, uint32_t(settings.isolateAsset), uint32_t(settings.forceLod),
                         settings.transmission ? 1u : 0u};
    s.current.inspection = {settings.inspectIdentity, s.streaming ? 1u : 0u, 0, 0};
    s.current.previousWind = s.previous.wind;
    s.current.curvature = temporal.curvature;
    s.current.raster = temporal.raster;
    const float focal =
        .5f * temporal.pixels.y *
        glm::length(glm::vec3(temporal.current[0][1], temporal.current[1][1], temporal.current[2][1]));
    s.current.detail = {focal / 935.3074f, s.previous.detail.x, 0, 0};
    s.current.pyramidSize = {};
    s.currentCamera = worldPosition(camera, grid);
    auto *cellData = static_cast<Cell *>(f.cells.mapped);
    for (uint32_t c = 0; !s.streaming && c < s.cellCount; ++c) {
        Cell cell = s.localCells[c];
        cell.origin = {glm::vec3(relativePosition(s.population.cells[c].origin, s.currentCamera)), 0};
        cell.previousOrigin = {glm::vec3(relativePosition(s.population.cells[c].origin, s.previousCamera)),
                               0};
        cell.casterLo = s.casterLo[c];
        cell.casterHi = s.casterHi[c];
        cellData[c] = cell;
        s.cellBounds[c] = cell;
    }
    f.stats.casterCandidates = s.casterCount;
    bool valid = s.history && temporal.state.y != 0 && s.previous.limits.w != 0 &&
                 s.previous.modes.x == settings.representation && s.previous.cards.x == settings.cardWidth &&
                 s.previous.profile.y == s.current.profile.y && s.previous.profile.z == s.current.profile.z &&
                 s.previous.inspection.x == s.current.inspection.x &&
                 settings.time >= s.previous.cameraTime.w && settings.time - s.previous.cameraTime.w <= .25f;
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
float VulkanVegetation::streamingRadius() const {
    const auto &s = *impl_;
    return s.streaming && s.current.limits.w ? s.streaming->demandRadius(s.current.settings.z) : 0;
}
void VulkanVegetation::prepareStream(VulkanTerrainStream *terrain, const VegetationSettings &settings,
                                     double terrainPreparationMilliseconds) {
    auto &s = *impl_;
    if (!s.streaming)
        return;
    auto start = std::chrono::steady_clock::now();
    auto &f = s.frames[s.frame];
    f.stats.preparationMilliseconds += terrainPreparationMilliseconds;
    s.streaming->prepare(s.frame, s.currentCamera, s.previousCamera, settings, terrain, s.cellBounds.data(),
                         f.stats);
    f.stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::budgetStream(double remaining) {
    auto &s = *impl_;
    if (!s.streaming)
        return;
    auto start = std::chrono::steady_clock::now();
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled) {
        s.current.lists[0].w = 0;
        return;
    }
    s.streaming->restrictPopulation(remaining, s.cellBounds.data(), f.stats);
    std::memcpy(f.cells.mapped, s.cellBounds.data(), s.cellCount * sizeof(Cell));
    auto *groups = static_cast<glm::uvec4 *>(f.groups.mapped);
    uint32_t active = 0;
    for (uint32_t cell : s.streaming->orderedCells())
        if (s.cellBounds[cell].origin.w != 0)
            groups[active++] = {cell * 128, 128, cell, 0};
    s.current.lists[0].w = active;
    f.stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::bounds(VulkanShadows &shadows) {
    auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    if (!s.current.limits.w || !s.casterCount)
        return;
    if (s.legacy && s.current.settings.w <= 0)
        return;
    for (uint32_t i = 0; i < s.cellCount; ++i) {
        if ((!s.streaming && !std::isfinite(s.casterLo[i].x)) ||
            (s.streaming && s.cellBounds[i].origin.w == 0))
            continue;
        const auto &c = s.cellBounds[i];
        glm::vec3 lo = glm::vec3(c.casterLo + c.origin), hi = glm::vec3(c.casterHi + c.origin);
        auto nearest = glm::max(glm::max(lo, -hi), glm::vec3(0));
        if (glm::length(nearest) > (s.legacy ? s.current.settings.w : s.casterRange[i]))
            continue;
        curveBounds(lo, hi, s.current.curvature);
        shadows.includeCasterBounds({lo, hi, true});
    }
    s.frames[s.frame].stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::updateAndSelectCasters(VkCommandBuffer cmd, const ShadowUniform &shadows,
                                              uint32_t cascades) {
    auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled)
        return;
    s.current.limits.y = (s.casterCount && (!s.legacy || s.current.settings.w > 0)) ? cascades + 1 : 1;
    for (uint32_t i = 0; i < cascades; ++i)
        s.current.views[i + 1] = shadows.matrices[i];
    std::memcpy(f.uniform.mapped, &s.current, sizeof(Uniform));
    if (s.queries)
        vkCmdResetQueryPool(cmd, s.queries, s.frame * queryCount, queryCount);
    if (!s.uploaded) {
        uint64_t geometryBytes = s.geometry.size, instanceBytes = uint64_t(s.count) * 32;
        VkBufferCopy copy{0, 0, geometryBytes};
        vkCmdCopyBuffer(cmd, s.staging.buffer, s.geometry.buffer, 1, &copy);
        if (instanceBytes && !s.streaming) {
            copy = {geometryBytes, 0, instanceBytes};
            vkCmdCopyBuffer(cmd, s.staging.buffer, s.instances.buffer, 1, &copy);
        }
        s.uploaded = true;
    }
    vkCmdFillBuffer(cmd, f.work.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
    if (s.streaming && s.streaming->updates()) {
        s.stamp(cmd, 16);
        s.streaming->record(cmd);
        s.stamp(cmd, 17);
    }
    s.stamp(cmd, 18);
    for (uint32_t view = 1; view < s.current.limits.y; ++view)
        s.selectView(cmd, view);
    s.stamp(cmd, 19);
    f.stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::preparePyramid(VisibilityPyramidView pyramid) {
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (pyramid.image) {
        VkDescriptorImageInfo info{{}, pyramid.image, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = f.set;
        w.dstBinding = 10;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(s.device, 1, &w, 0, nullptr);
        s.current.pyramidSize = {pyramid.width, pyramid.height, pyramid.levels, 0};
    }
}
void VulkanVegetation::selectCamera(VkCommandBuffer cmd, VisibilityPyramidView pyramid) {
    const auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled)
        return;
    // Scratch was also read by the preceding caster scatter dispatches.
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    s.stamp(cmd, 0);
    s.selectView(cmd, 0, pyramid.image != VK_NULL_HANDLE);
    s.stamp(cmd, 1);
    f.stats.preparationMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void VulkanVegetation::draw(VkCommandBuffer cmd, VkDescriptorSet lighting, uint32_t pass, uint32_t view) {
    auto start = std::chrono::steady_clock::now();
    auto &s = *impl_;
    auto &f = s.frames[s.frame];
    if (!f.stats.enabled || view >= s.current.limits.y)
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
    for (uint32_t bin = 0; bin < s.bins; ++bin) {
        if (view && !s.legacy && s.current.ranges[s.geometryInfo[bin].w].w <= 0)
            continue;
        uint32_t push[] = {view, bin, 0, 0};
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 16,
                           push);
        vkCmdDrawIndexedIndirect(cmd, f.work.buffer, uint64_t(view * viewWords + bin * 8) * 4, 1, 32);
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
    if (s.streaming)
        s.streaming->submitted();
    s.previous = s.current;
    s.previousCamera = s.currentCamera;
    s.history = true;
    s.frames[s.frame].submitted = true;
    s.lastFrame = s.frame;
    ++s.serial;
}
const VegetationStats &VulkanVegetation::stats() const { return impl_->last; }
void VulkanVegetation::copySnapshot(VkCommandBuffer cmd) {
    auto &s = *impl_;
    if (!s.diagnostic.buffer)
        throw std::logic_error("Vegetation snapshot requires startup diagnostics");
    if (!s.frames[s.lastFrame].stats.enabled)
        return;
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (s.streaming)
        s.streaming->copySnapshot(cmd);
    VkBufferCopy copy{0, 0, uint64_t(s.listEntries) * 4};
    vkCmdCopyBuffer(cmd, s.frames[s.lastFrame].lists.buffer, s.diagnostic.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
}
VegetationSnapshot VulkanVegetation::snapshot() const {
    auto &s = *impl_;
    VegetationSnapshot out;
    out.stats = s.decode(s.lastFrame);
    out.binCount = s.bins;
    if (s.streaming && out.stats.enabled)
        out.cells = s.streaming->snapshot();
    if (out.stats.enabled) {
        auto *w = static_cast<const uint32_t *>(s.frames[s.lastFrame].readback.mapped);
        for (uint32_t v = 0; v < 6; ++v)
            for (uint32_t b = 0; b < s.bins; ++b) {
                out.counts[v * s.bins + b] = w[v * viewWords + b * 8 + 1];
                auto *ids = static_cast<const uint32_t *>(s.diagnostic.mapped) + s.current.lists[v].x +
                            w[v * viewWords + b * 8 + 7];
                for (uint32_t j = 0; j < out.counts[v * s.bins + b]; ++j)
                    out.identities[v].push_back(s.streaming ? s.streaming->identity(ids[j])
                                                            : s.population.instances.at(ids[j]).identity);
            }
    }
    return out;
}
} // namespace ow

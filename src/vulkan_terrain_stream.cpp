#include "vulkan_terrain_stream.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <stdexcept>
namespace ow {
namespace {
void check(VkResult r, const char *name) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(name) + ": " + std::to_string(r));
}
void memoryBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
void imageGeneral(VkCommandBuffer cmd, VkImage image, uint32_t layers) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
constexpr VkBufferUsageFlags heightUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
VkImageCreateInfo normalImageInfo(uint32_t width, uint32_t layers) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent = {width, width, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return info;
}
uint64_t normalImageBytes(VkDevice device, uint32_t width, uint32_t layers) {
    const auto info = normalImageInfo(width, layers);
    VkImage image;
    check(vkCreateImage(device, &info, nullptr, &image), "Create streaming sizing image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    vkDestroyImage(device, image, nullptr);
    return req.size;
}
uint64_t heightBufferBytes(VkDevice device, uint64_t size) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = heightUsage;
    VkBuffer buffer;
    check(vkCreateBuffer(device, &info, nullptr, &buffer), "Create streaming sizing buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    vkDestroyBuffer(device, buffer, nullptr);
    return req.size;
}
uint64_t alignTo(uint64_t n, uint64_t a) { return (n + a - 1) / a * a; }
} // namespace
VulkanTerrainStream::VulkanTerrainStream(VulkanTerrain &g, TerrainStreamingConfig config,
                                         const std::filesystem::path &shaders, uint32_t temporalCapacity)
    : history_(temporalCapacity), g_(g),
      layout_(g.grid_, g.normalSubdivisions_, g.lod_->patchCells(), config.hierarchical),
      config_(std::move(config)) {
    if (!std::isfinite(config_.maximumDistance) || config_.maximumDistance < 0 ||
        config_.maximumDistance > 16384)
        throw std::invalid_argument("Maximum terrain distance must be finite and in 0..16384 metres");
    if (config_.hierarchical && (layout_.grid.spacing != .5f || layout_.patchCells > 32))
        throw std::invalid_argument("Hierarchical terrain requires 0.5 m base "
                                    "spacing and patches of at most 32 cells");
    const uint16_t endian = 1;
    if (*reinterpret_cast<const uint8_t *>(&endian) != 1)
        throw std::runtime_error("Terrain GPU cache requires a little-endian host");
    if (config_.transferBytesPerFrame < 32768 || config_.transferBytesPerFrame > 8 * terrainMiB ||
        config_.transferBytesPerFrame % 16)
        throw std::invalid_argument(
            "Terrain transfer allowance must be a multiple of 16 in 32 KiB..8 MiB");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_.context_.physical, &props);
    deviceHash_ = terrainChecksum(props.pipelineCacheUUID, VK_UUID_SIZE);
    for (auto v : {props.vendorID, props.deviceID, props.driverVersion})
        deviceHash_ = terrainChecksum(&v, sizeof(v), deviceHash_);
    shaderHash_ = 14695981039346656037ull;
    for (const auto *name :
         {"terrain_height.comp.spv", "terrain_normals.comp.spv", "terrain_normal_filter.comp.spv",
          "terrain_bounds.comp.spv", "terrain_pack.comp.spv", "terrain_tile_height.comp.spv",
          "terrain_tile_normal.comp.spv", "terrain_quality_bounds.comp.spv"}) {
        std::ifstream file(shaders / name, std::ios::binary);
        if (!file)
            throw std::runtime_error("Missing streaming generation shader");
        std::array<char, 4096> bytes{};
        while (file) {
            file.read(bytes.data(), bytes.size());
            shaderHash_ = terrainChecksum(bytes.data(), size_t(file.gcount()), shaderHash_);
        }
    }
    heightStride_ = alignTo(layout_.heightsBytes, props.limits.minStorageBufferOffsetAlignment);
    weightStride_ = alignTo(192, props.limits.minUniformBufferOffsetAlignment);
    const auto reserve = std::min<uint64_t>(64 * terrainMiB, config_.gpuBytes / 8);
    if (config_.gpuBytes <= g_.allocatedBytes() + reserve)
        throw std::invalid_argument("Terrain GPU budget too small for generation scratch");
    const auto available = config_.gpuBytes - g_.allocatedBytes() - reserve;
    const auto coverageCost = [&](uint32_t slots) {
        return heightBufferBytes(g_.context_.device, heightStride_ * slots) +
               normalImageBytes(g_.context_.device, layout_.widths[layout_.levels - 1], slots * 2);
    };
    const auto detailCost = [&](uint32_t slots) {
        uint64_t bytes = 0;
        for (uint32_t l = 0; l + 1 < layout_.levels; ++l)
            bytes += normalImageBytes(g_.context_.device, layout_.widths[l], slots * 2);
        return bytes;
    };
    const uint32_t slotLimit = props.limits.maxImageArrayLayers / 2;
    const auto pools = config_.hierarchical
                           ? terrainHierarchyPoolCapacity(
                                 available, slotLimit,
                                 std::clamp(int(config_.maximumDistance / layout_.grid.extent()), 0,
                                            terrainMaximumRadius(layout_.grid)),
                                 layout_.detailBytes != 0, coverageCost, detailCost)
                           : terrainPoolCapacity(available, slotLimit, layout_.detailBytes != 0,
                                                 coverageCost, detailCost);
    uint32_t extra = 0;
    if (config_.hierarchical)
        for (uint32_t n = 1; n <= 64 && pools.coverage + n <= slotLimit; ++n) {
            if (coverageCost(pools.coverage + n) - coverageCost(pools.coverage) >
                config_.shadowCasterBytes)
                break;
            extra = n;
        }
    stats_.shadowSlots = extra;
    stats_.shadowBytes = coverageCost(pools.coverage + extra) - coverageCost(pools.coverage);
    capacity_ = pools.coverage + extra;
    detailCapacity_ = pools.detail;
    const int maxRadius = pools.maximumRadius;
    cache_ = std::make_unique<TerrainCache>(layout_, config_);
    stream_ = std::make_unique<TerrainStreamer>(capacity_, maxRadius, layout_.grid, config_.hierarchical,
                                                extra);
    lod_ = std::make_unique<TerrainLod>(g_.grid_, g_.capacity_,
                                        TerrainLodConfig{true, layout_.patchCells, 64}, capacity_);
    meta_.resize(capacity_);
    boundsScratch_.resize(size_t(layout_.boundsBytes / sizeof(glm::vec2)));
    details_.resize(detailCapacity_);
    activeCoords_.reserve(capacity_);
    activeSlots_.reserve(capacity_);
    priorities_.reserve(capacity_);
    stats_.capacity = capacity_;
    stats_.detailCapacity = detailCapacity_;
    stats_.maximumRadius = maxRadius;
    try {
        packPipeline_ = g_.pipeline(shaders / "terrain_pack.comp.spv");
        if (config_.hierarchical) {
            tileHeightPipeline_ = g_.pipeline(shaders / "terrain_tile_height.comp.spv");
            tileNormalPipeline_ = g_.pipeline(shaders / "terrain_tile_normal.comp.spv");
            qualityBoundsPipeline_ = g_.pipeline(shaders / "terrain_quality_bounds.comp.spv");
        }
        initialize();
        preparedDraws_.reserve(lod_->drawCapacity());
        casterRecords_.reserve(lod_->drawCapacity() * 2);
        shadowDraws_.reserve(65536);
        if (config_.vertexDiagnostics) {
            vertexProbePipeline_ =
                g_.pipeline(shaders / "terrain_vertex_probe.comp.spv", graphicsLayout_);
            const size_t leaves =
                size_t(g_.capacity_ / layout_.patchCells) * (g_.capacity_ / layout_.patchCells);
            diagnosticDraws_.reserve((4 * leaves - 1) / 3 * capacity_);
        }
        uint64_t host = 0;
        for (uint32_t f = 0; f < 2; ++f)
            host += uploads_[f].allocationSize + readbacks_[f].allocationSize +
                    views_[f].allocationSize + weights_[f].allocationSize;
        stats_.gpuBytes = g_.allocatedBytes() + imageBytes_ - host;
        stats_.hostBytes = host + cache_->allocatedBytes() +
                           uint64_t(capacity_) *
                               (sizeof(TerrainStreamer::Slot) + sizeof(Meta) + sizeof(ChunkCoord) + 8);
        // CPU quadtree and worst-case draw storage are also bounded and charged.
        const uint64_t leaves =
            uint64_t(g_.capacity_ / layout_.patchCells) * (g_.capacity_ / layout_.patchCells);
        stats_.hostBytes +=
            (diagnosticDraws_.capacity() + preparedDraws_.capacity() + casterRecords_.capacity()) *
            sizeof(DrawRecord);
        stats_.hostBytes +=
            lod_->drawCapacity() *
            (sizeof(VisibilityBounds) + 2 * (sizeof(TerrainPatchDraw) + sizeof(VisibilityBounds)));
        stats_.hostBytes += shadowDraws_.capacity() * sizeof(ShadowDraw);
        stats_.hostBytes += 128 * 1024 + layout_.boundsBytes; // bounded cache maintenance metadata
                                                              // and spatial lookup
        stats_.hostBytes +=
            (4 * leaves - 1) / 3 *
            (32 + uint64_t(capacity_) * (2 * sizeof(glm::vec2) + sizeof(TerrainPatchDraw)));
        if (stats_.gpuBytes > config_.gpuBytes + config_.shadowCasterBytes ||
            stats_.hostBytes > config_.hostBytes)
            throw std::runtime_error(
                "Terrain resource budget exceeded: GPU " + std::to_string(stats_.gpuBytes) + " / " +
                std::to_string(config_.gpuBytes) + " bytes, host " + std::to_string(stats_.hostBytes) +
                " / " + std::to_string(config_.hostBytes) + " bytes (required / configured)");
        warmGeneration();
    } catch (...) {
        cleanup();
        throw;
    }
}
VulkanTerrainStream::~VulkanTerrainStream() {
    cache_.reset();
    cleanup();
}
void VulkanTerrainStream::createImage(Image &image, uint32_t width, uint32_t layers) {
    image.width = width;
    image.layers = layers;
    const auto info = normalImageInfo(width, layers);
    check(vkCreateImage(g_.context_.device, &info, nullptr, &image.image), "Create streaming normals");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_.context_.device, image.image, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_.context_.physical, &props);
    uint32_t type = 0;
    while (type < props.memoryTypeCount &&
           (!(req.memoryTypeBits & (1u << type)) ||
            !(props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
        ++type;
    if (type == props.memoryTypeCount)
        throw std::runtime_error("No streaming image memory type");
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    check(vkAllocateMemory(g_.context_.device, &alloc, nullptr, &image.memory),
          "Allocate streaming normals");
    image.bytes = req.size;
    imageBytes_ += req.size;
    check(vkBindImageMemory(g_.context_.device, image.image, image.memory, 0), "Bind streaming normals");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    check(vkCreateImageView(g_.context_.device, &view, nullptr, &image.view),
          "Create streaming normal view");
}
void VulkanTerrainStream::initialize() {
    auto d = g_.context_.device;
    g_.createBuffer(heights_, heightStride_ * capacity_, heightUsage);
    createImage(coarse_, layout_.widths[layout_.levels - 1], capacity_ * 2);
    for (uint32_t l = 0; l + 1 < layout_.levels; ++l)
        createImage(fine_[l], layout_.widths[l], detailCapacity_ * 2);
    const VkDescriptorType types[] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLER,        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    VkDescriptorSetLayoutBinding bindings[11]{};
    for (uint32_t i = 0; i < 11; ++i)
        bindings[i] = {i, types[i], 1,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 11;
    li.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &descriptorLayout_), "Create streaming layout");
    const auto count = 2 * capacity_ * layout_.levels;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * count},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * count},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5 * count},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, count}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = count;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &pi, nullptr, &descriptorPool_), "Create streaming descriptors");
    sets_.resize(count);
    for (uint32_t f = 0; f < 2; ++f) {
        g_.createBuffer(uploads_[f], config_.transferBytesPerFrame, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        true);
        g_.createBuffer(readbacks_[f], config_.transferBytesPerFrame, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        true);
        g_.createBuffer(views_[f], 128, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        g_.createBuffer(weights_[f], weightStride_ * capacity_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        true);
        for (uint32_t s = 0; s < capacity_; ++s)
            for (uint32_t l = 0; l < layout_.levels; ++l) {
                auto &set = sets_[(f * capacity_ + s) * layout_.levels + l];
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = descriptorPool_;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &descriptorLayout_;
                check(vkAllocateDescriptorSets(d, &ai, &set), "Allocate streaming descriptor");
                const auto current = l + 1 < layout_.levels ? fine_[l].view : coarse_.view;
                const auto next = l + 2 < layout_.levels ? fine_[l + 1].view : coarse_.view;
                VkDescriptorBufferInfo buffers[] = {
                    {views_[f].handle, 0, 128},
                    {heights_.handle, heightStride_ * s, layout_.heightsBytes},
                    {weights_[f].handle, weightStride_ * s, 192},
                    {g_.patchIndices_.handle, 0, g_.patchIndices_.size},
                    {heights_.handle, heightStride_ * s, layout_.heightsBytes}};
                VkDescriptorImageInfo images[] = {
                    {VK_NULL_HANDLE, coarse_.view, VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, current, VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, next, VK_IMAGE_LAYOUT_GENERAL},
                    {g_.normalSampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                    {VK_NULL_HANDLE,
                     l > 0 ? fine_[l - 1].view : (layout_.levels > 1 ? fine_[0].view : coarse_.view),
                     VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, current, VK_IMAGE_LAYOUT_GENERAL}};
                VkWriteDescriptorSet writes[11]{};
                for (uint32_t i = 0; i < 11; ++i) {
                    auto &w = writes[i];
                    w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w.dstSet = set;
                    w.dstBinding = i;
                    w.descriptorCount = 1;
                    w.descriptorType = types[i];
                    if (i < 3)
                        w.pBufferInfo = &buffers[i];
                    else if (i < 7)
                        w.pImageInfo = &images[i - 3];
                    else if (i < 9)
                        w.pBufferInfo = &buffers[i - 4];
                    else
                        w.pImageInfo = &images[i - 5];
                }
                vkUpdateDescriptorSets(d, 11, writes, 0, nullptr);
            }
    }
    VkPushConstantRange push{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout graphicsSets[] = {descriptorLayout_, g_.lightingLayout_,
                                                  g_.materialLayout_, g_.visibilityLayout_};
    layout.setLayoutCount = g_.visibilityLayout_ ? 4
                            : g_.materialLayout_ ? 3
                            : g_.lightingLayout_ ? 2
                                                 : 1;
    layout.pSetLayouts = graphicsSets;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &layout, nullptr, &graphicsLayout_),
          "Create streaming pipeline layout");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_.context_.physical, &props);
    if (props.limits.timestampComputeAndGraphics) {
        timestampPeriod_ = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = uint32_t(frames_.size()) * queriesPerFrame;
        check(vkCreateQueryPool(d, &q, nullptr, &queryPool_), "Create generation timing queries");
    }
}
void VulkanTerrainStream::warmGeneration() {
    // Some drivers defer compute compilation until the first dispatch. Pay that
    // cost during startup with a tiny owned workload, before interactive frames.
    constexpr uint32_t cells = 8;
    auto parameters = terrainParameters({cells, layout_.grid.spacing}, {});
    parameters.dimensions.w = layout_.subdivisions;
    parameters.sampling = {layout_.halos[0], cells, layout_.levels, 0};
    std::memcpy(g_.parameters_[0].mapped, &parameters, sizeof(parameters));
    const auto cmd = g_.beginImmediate();
    for (uint32_t level = 0; level < layout_.levels; ++level)
        imageGeneral(cmd, g_.normalImages_[level], 2);
    const auto dispatch = [&](VkPipeline pipeline, uint32_t level, uint32_t width, bool bounds = false) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_.computeLayout_, 0, 1,
                                &g_.slots_[0].descriptors[0][std::min(level, layout_.levels - 1)], 0,
                                nullptr);
        const glm::uvec4 push[2] = {{width, layout_.halos[std::min(level, layout_.levels - 1)],
                                     layout_.halos[level ? level - 1 : 0], level},
                                    {0, 0, 0, 0}};
        vkCmdPushConstants(cmd, g_.computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, bounds ? 1 : (width + 7) / 8, bounds ? 1 : (width + 7) / 8, 1);
        memoryBarrier(cmd);
    };
    dispatch(g_.heightPipeline_, 0, cells * layout_.subdivisions + 3 + 2 * layout_.halos[0]);
    dispatch(g_.normalPipeline_, 0, cells * layout_.subdivisions + 1 + 2 * layout_.halos[0]);
    for (uint32_t level = 1; level < layout_.levels; ++level)
        dispatch(g_.filterPipeline_, level,
                 (cells * layout_.subdivisions >> level) + 1 + 2 * layout_.halos[level]);
    dispatch(g_.boundsPipeline_, 0, 1, true);
    dispatch(packPipeline_, 0, cells + 1);
    if (config_.hierarchical) {
        parameters.sampling.w = 1;
        std::memcpy(g_.parameters_[0].mapped, &parameters, sizeof(parameters));
        dispatch(tileHeightPipeline_, 0, cells + 1);
        dispatch(tileHeightPipeline_, 1, cells / 2 + 1);
        dispatch(qualityBoundsPipeline_, 0, 1, true);
        dispatch(tileNormalPipeline_, 0, cells + 1);
    }
    g_.endImmediate(cmd);
    generatorImagesInitialized_ = true;
}
TerrainCacheKey VulkanTerrainStream::key(TerrainHandle h, TerrainProduct p) const {
    return {{stream_->slot(h.index).chunk, settings_},
            shaderHash_,
            deviceHash_,
            p,
            stream_->slot(h.index).level,
            config_.hierarchical ? (config_.unfilteredHeightReference ? 8u : 7u) : 3u};
}
void VulkanTerrainStream::settings(const TerrainSettings &settings) {
    terrainParameters(g_.grid_, {{}, settings});
    if (!(TerrainRequest{{}, settings_} == TerrainRequest{{}, settings})) {
        settings_ = settings;
        ++epoch_;
        range_ = rangeTarget_ = 64;
        qualityScale_ = 1;
    }
}
void VulkanTerrainStream::error() {
    ++stats_.errors;
    if (serial_ >= ioBackoffUntil_) {
        std::cerr << "Terrain cache I/O failed; procedural generation remains "
                     "available.\n";
        ioBackoffUntil_ = serial_ + 300;
    }
}
int VulkanTerrainStream::allocateDetail(TerrainHandle h) {
    if (!detailCapacity_)
        return -1;
    for (uint32_t i = 0; i < detailCapacity_; ++i)
        if (details_[i].owner.index == invalidTerrainSlot && !details_[i].retiring) {
            details_[i].owner = h;
            return int(i);
        }
    return -1;
}
bool VulkanTerrainStream::detailValid(TerrainHandle h, uint64_t revision) const {
    return stream_->valid(h) && revision == meta_[h.index].revision &&
           stream_->slot(h.index).detailWanted;
}
bool VulkanTerrainStream::detailRunnable(uint32_t i) const {
    const auto &s = stream_->slot(i);
    const TerrainHandle h{i, s.generation};
    if (s.state != TerrainStreamer::State::Resident || !s.detailWanted || s.detail >= 0 ||
        !layout_.detailBytes || meta_[i].readRevision || (gen_.active && gen_.handle == h) ||
        (upload_.lease >= 0 && upload_.handle == h))
        return false;
    for (const auto &d : details_)
        if (d.owner == h)
            return false;
    return true;
}
void VulkanTerrainStream::updateDetailDemand(const Camera &camera, const SceneSettings &settings) {
    const bool lodEnabled = settings.lod;
    const float firstRange =
        config_.hierarchical
            ? terrainHierarchyRange(terrainDetailRangeScale(
                  qualityScale_, settings.terrainLodQuality && lodEnabled, settings.freezeLod, dt_))
            : range_;
    stats_.detailFirstRange = firstRange;
    priorities_.clear();
    for (uint32_t i = 0; i < capacity_; ++i) {
        auto &s = stream_->slot(i);
        auto &m = meta_[i];
        s.detailWanted = false;
        const double maximum = lodEnabled && layout_.levels > 1
                                   ? firstRange * stream_->scale(i) * double(1u << (layout_.levels - 2))
                                   : INFINITY;
        m.rank = detailPolicy_.rank(s.tile(), layout_.grid.extent(), s.detail >= 0, maximum);
        // Prediction only uses already-resident prefetch. Fully covered parents
        // retain their dependency role without acquiring unnecessary normals.
        if (!s.wanted ||
            (!s.visible &&
             !(config_.hierarchical && s.wanted && s.state == TerrainStreamer::State::Resident &&
               m.rank.distance < m.rank.current)) ||
            (config_.hierarchical && s.state == TerrainStreamer::State::Resident && !s.drawMask))
            continue;
        if (!config_.hierarchical && lodEnabled && s.state == TerrainStreamer::State::Resident &&
            layout_.levels > 1) {
            const auto offset = stream_->offset(i, camera);
            const auto lo = offset + glm::vec3(0, m.heights.x, 0);
            const auto hi =
                offset + glm::vec3(layout_.grid.extent(), m.heights.y, layout_.grid.extent());
            if (glm::length(glm::max(glm::max(lo, -hi), glm::vec3(0))) > maximum)
                continue;
        }
        if (m.rank.eligible)
            priorities_.push_back(i);
    }
    std::sort(priorities_.begin(), priorities_.end(), [&](uint32_t a, uint32_t b) {
        const auto &x = meta_[a].rank, &y = meta_[b].rank;
        if (TerrainDetailPolicy::before(x, y))
            return true;
        if (TerrainDetailPolicy::before(y, x))
            return false;
        return a < b;
    });
    const auto wanted = std::min<size_t>(priorities_.size(),
                                         detailCapacity_ > 2 ? detailCapacity_ - 2 : detailCapacity_);
    stats_.detailEligible = uint32_t(priorities_.size());
    stats_.detailSelected = uint32_t(wanted);
    for (size_t n = 0; n < wanted; ++n)
        stream_->slot(priorities_[n]).detailWanted = true;
    for (uint32_t i = 0; i < capacity_; ++i) {
        auto &m = meta_[i];
        const auto &s = stream_->slot(i);
        if (m.wanted && !s.detailWanted) {
            ++m.revision;
            if (s.detail < 0 && s.state == TerrainStreamer::State::Resident)
                ++stats_.detailCancellations;
        }
        m.wanted = s.detailWanted;
    }
}
double VulkanTerrainStream::generationProgress() const {
    if (gen_.stage >= 5)
        return 1;
    uint64_t total = 0;
    const auto groups = [](uint32_t width) {
        const uint64_t side = (width + 7) / 8;
        return side * side;
    };
    if (config_.hierarchical && gen_.detailOnly) {
        for (uint32_t l = 0; l + 1 < layout_.levels; ++l)
            total += groups(layout_.widths[l]);
    } else {
        total += groups(config_.hierarchical ? layout_.grid.cells + 1 : layout_.widths[0] + 2);
        for (uint32_t l = config_.hierarchical ? layout_.levels - 1 : 0; l < layout_.levels; ++l)
            total += groups(layout_.widths[l]);
        const uint64_t side = layout_.grid.cells / layout_.patchCells;
        total += side * side + groups(layout_.grid.cells + 1);
    }
    return total ? std::min(1.0, double(gen_.groups) / double(total)) : 1;
}
void VulkanTerrainStream::cancelGeneration(bool preempted) {
    if (!gen_.active || gen_.cancelled)
        return;
    gen_.cancelled = true;
    if (!gen_.published)
        stats_.discardedGroups += gen_.groups;
    if (config_.diskCache && (gen_.published || gen_.lease >= 0))
        ++stats_.skippedExports;
    if (preempted) {
        ++stats_.detailPreemptions;
        auto &m = meta_[gen_.handle.index];
        ++m.revision;
        m.restartAfter = elapsed_ + .25;
    }
}
TerrainDetailJobSnapshot VulkanTerrainStream::detailJob() const {
    TerrainDetailJobSnapshot out;
    out.completed = completed_;
    out.submitted = serial_;
    if (gen_.active && (gen_.detailOnly || gen_.product == TerrainProduct::Detail)) {
        out.handle = gen_.handle;
        out.revision = gen_.revision;
        out.retireAfter = gen_.readyAfter;
        out.groups = gen_.groups;
        out.cancelled = gen_.cancelled;
        out.progress = generationProgress();
        out.phase = gen_.stage < 5   ? TerrainDetailPhase::Generation
                    : gen_.published ? TerrainDetailPhase::Export
                                     : TerrainDetailPhase::DirectCopy;
    } else if (upload_.lease >= 0 && upload_.product == TerrainProduct::Detail) {
        out.handle = upload_.handle;
        out.revision = upload_.revision;
        out.retireAfter = upload_.readyAfter;
        out.progress = double(upload_.offset) / double(layout_.detailBytes);
        out.phase = TerrainDetailPhase::Upload;
    }
    for (const auto &f : frames_)
        if (f.publish.pending && f.publish.product == TerrainProduct::Detail) {
            if (out.phase == TerrainDetailPhase::Idle) {
                out.handle = f.publish.handle;
                out.revision = f.publish.revision;
                out.retireAfter = f.serial;
            }
            if (out.handle == f.publish.handle && out.revision == f.publish.revision)
                out.publicationPending = true;
        }
    return out;
}
void VulkanTerrainStream::beginFrame(uint32_t frame, const Camera &camera,
                                     const SceneSettings &settings) {
    const int radius = settings.viewRadius;
    auto &f = frames_[frame];
    completed_ = std::max(completed_, f.serial);
    const auto now = std::chrono::steady_clock::now();
    const double seconds = config_.diagnosticFrameSeconds > 0
                               ? config_.diagnosticFrameSeconds
                               : std::chrono::duration<double>(now - previous_).count();
    dt_ = float(std::clamp(seconds, .0001, .1));
    elapsed_ += std::max(0.0, seconds);
    detailPolicy_.update(camera, layout_.grid.extent(), seconds);
    previous_ = now;
    stats_.generationMilliseconds = stats_.terrainMilliseconds = 0;
    stats_.completedGenerationGroups = stats_.completedGenerationSegments = 0;
    if (f.terrainTimestamp && queryPool_) {
        uint64_t q[4]{};
        if (vkGetQueryPoolResults(
                g_.context_.device, queryPool_, frame * queriesPerFrame, 2, sizeof(q), q, 16,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS &&
            q[1] && q[3])
            stats_.terrainMilliseconds = double(q[2] - q[0]) * timestampPeriod_ / 1e6;
        f.terrainTimestamp = false;
    }
    if (f.timestamp && queryPool_) {
        // Frame fence has completed: query storage and metadata belong to this
        // submission, even if the active job has since changed or been cancelled.
        std::array<uint64_t, TerrainGenerationBudget::maximumSegments * 4> q{};
        const uint32_t count = f.segmentCount * 2;
        vkGetQueryPoolResults(g_.context_.device, queryPool_, frame * queriesPerFrame + 2, count,
                              count * 2 * sizeof(uint64_t), q.data(), 16,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const uint32_t last = count * 2 - 2;
        if (q[1] && q[last + 1]) {
            stats_.generationMilliseconds = double(q[last] - q[0]) * timestampPeriod_ / 1e6;
            stats_.completedGenerationGroups = f.generationGroups;
            stats_.completedGenerationSegments = f.segmentCount;
        }
        constexpr auto kernels = uint32_t(TerrainComputeKernel::Count);
        std::array<uint32_t, kernels> groups{};
        std::array<double, kernels> milliseconds{};
        std::array<bool, kernels> missing{};
        for (uint32_t i = 0; i < f.segmentCount; ++i) {
            const auto &segment = f.segments[i];
            const auto k = uint32_t(segment.kernel), at = i * 4;
            if (!q[at + 1] || !q[at + 3] || q[at + 2] <= q[at]) {
                missing[k] = true;
                continue;
            }
            groups[k] += segment.groups;
            milliseconds[k] += double(q[at + 2] - q[at]) * timestampPeriod_ / 1e6;
        }
        for (uint32_t k = 0; k < kernels; ++k)
            if (!missing[k])
                generationBudget_.observe(TerrainComputeKernel(k), groups[k], milliseconds[k]);
        f.timestamp = false;
    }
    if (f.exportLease >= 0) {
        std::memcpy(cache_->data(f.exportLease) + f.exportOffset, readbacks_[frame].mapped,
                    size_t(f.exportBytes));
        gen_.offset += f.exportBytes;
        gen_.exportPending = false;
        f.exportLease = -1;
    }
    if (f.boundsHandle.index != invalidTerrainSlot) {
        if (stream_->valid(f.boundsHandle))
            applyBounds(f.boundsHandle, static_cast<const glm::vec2 *>(readbacks_[frame].mapped));
        f.boundsHandle = {};
    }
    shadowDemandCount_ = 0;
    shadowMissingBoundsCount_ = 0;
    if (config_.hierarchical && settings.terrainVisible && settings.shadows.mode != ShadowMode::Off &&
        settings.lighting.sunEnabled) {
        const int64_t span = int64_t(1) << stream_->topLevel();
        const auto sun = glm::normalize(settings.lighting.sunDirection);
        const double distance = std::min(8192.0, config_.maximumDistance);
        const double dx = double(sun.x) * distance / layout_.grid.extent(),
                     dz = double(sun.z) * distance / layout_.grid.extent();
        const int r = std::min(radius, stream_->maximumRadius());
        const int64_t x0 =
            terrainFloorDivide(camera.chunk.x - r + int64_t(std::floor(std::min(0.0, dx))), span);
        const int64_t z0 =
            terrainFloorDivide(camera.chunk.z - r + int64_t(std::floor(std::min(0.0, dz))), span);
        const int64_t x1 =
            terrainFloorDivide(camera.chunk.x + r + 1 + int64_t(std::ceil(std::max(0.0, dx))), span);
        const int64_t z1 =
            terrainFloorDivide(camera.chunk.z + r + 1 + int64_t(std::ceil(std::max(0.0, dz))), span);
        // Keep the bounded rectangle nearest the camera. Preserve omitted
        // rectangles as missing casters so truncation never certifies empty space.
        const int64_t nx = std::min<int64_t>(8, x1 - x0 + 1), nz = std::min<int64_t>(8, z1 - z0 + 1);
        const int64_t ax =
            std::clamp(terrainFloorDivide(camera.chunk.x, span) - nx / 2, x0, x1 - nx + 1);
        const int64_t az =
            std::clamp(terrainFloorDivide(camera.chunk.z, span) - nz / 2, z0, z1 - nz + 1);
        const int64_t bx = ax + nx - 1, bz = az + nz - 1;
        for (int64_t z = az; z <= bz; ++z)
            for (int64_t x = ax; x <= bx; ++x)
                shadowDemand_[shadowDemandCount_++] = {x, z, stream_->topLevel()};
        auto missing = [&](int64_t a, int64_t b, int64_t c, int64_t d) {
            if (a <= c && b <= d)
                shadowMissingBounds_[shadowMissingBoundsCount_++] = {a, b, c, d};
        };
        missing(x0, z0, ax - 1, z1);
        missing(bx + 1, z0, x1, z1);
        missing(ax, z0, bx, az - 1);
        missing(ax, bz + 1, bx, z1);
    }
    stream_->shadowDemand(shadowDemand_.data(), shadowDemandCount_);
    stream_->demand(camera, radius, completed_, serial_, epoch_);
    stats_.requestedRadius =
        std::clamp(radius, 0, config_.hierarchical ? terrainMaximumRadius(layout_.grid) : 8);
    stats_.effectiveRadius = stream_->radius();
    // Coverage is independent of detail revisions. Accept it after coverage
    // demand, then resolve parent activation before ranking normal requests.
    // Otherwise a just-published inactive child could start detail for one frame.
    if (f.publish.pending && f.publish.product == TerrainProduct::Coverage) {
        if (stream_->valid(f.publish.handle)) {
            auto &s = stream_->slot(f.publish.handle.index);
            s.state = TerrainStreamer::State::Resident;
            s.fade = layout_.detailBytes ? 0 : 1;
        }
        f.publish.pending = false;
    }
    stream_->advance(dt_);
    for (uint32_t i = 0; i < capacity_; ++i) {
        auto &s = stream_->slot(i);
        if (s.state == TerrainStreamer::State::Requested)
            meta_[i] = {};
    }
    updateDetailDemand(camera, settings);
    if (f.publish.pending) {
        const auto p = f.publish;
        f.publish.pending = false;
        if (detailValid(p.handle, p.revision)) {
            auto &s = stream_->slot(p.handle.index);
            s.detail = p.detail;
            s.fade = 0;
            meta_[p.handle.index].detailMissing = false;
        } else if (p.detail >= 0) {
            ++stats_.discardedPublications;
            details_[p.detail] = {}; // This publication's last copy fence has retired.
        }
    }
    for (uint32_t i = 0; i < detailCapacity_; ++i) {
        auto &d = details_[i];
        if (d.retiring) {
            if (d.retireAfter <= completed_)
                d = {};
            continue;
        }
        if (d.owner.index == invalidTerrainSlot)
            continue;
        if (!stream_->valid(d.owner)) {
            d.retiring = true;
            d.retireAfter = serial_;
            continue;
        }
        auto &s = stream_->slot(d.owner.index);
        if (s.detail != int(i))
            continue; // Reserved upload, not visible yet.
        s.fade = std::clamp(s.fade + (s.detailWanted ? dt_ : -dt_) * 4, 0.f, 1.f);
        if (!s.detailWanted && s.fade == 0) {
            s.detail = -1;
            d.retiring = true;
            d.retireAfter = s.lastUse;
        }
    }
    // Cancel reads without releasing a mailbox still owned by a worker.
    for (int i = 0; i < 2; ++i) {
        auto &r = reads_[i];
        if (r.active && !r.cancelled &&
            (!stream_->valid(r.handle) || (r.revision && !detailValid(r.handle, r.revision)))) {
            cache_->cancelRead(i);
            r.cancelled = true;
            if (stream_->valid(r.handle) && meta_[r.handle.index].readRevision == r.revision) {
                meta_[r.handle.index].readRevision = 0;
                meta_[r.handle.index].readLease = -1;
            }
        }
    }
    if (upload_.lease >= 0 &&
        (!stream_->valid(upload_.handle) || (upload_.product == TerrainProduct::Detail &&
                                             !detailValid(upload_.handle, upload_.revision)))) {
        cache_->release(upload_.lease); // Submitted uploads reference frame
                                        // staging, not this payload.
        if (upload_.detail >= 0) {
            auto &d = details_[upload_.detail];
            d.retiring = true;
            d.retireAfter = upload_.readyAfter;
        }
        upload_ = {};
    }
    if (gen_.active &&
        (!stream_->valid(gen_.handle) || ((gen_.detailOnly || gen_.product == TerrainProduct::Detail) &&
                                          !detailValid(gen_.handle, gen_.revision))))
        cancelGeneration();
    if (gen_.active && !gen_.cancelled) {
        int bestPending = -1;
        for (auto i : priorities_) {
            const auto &s = stream_->slot(i);
            if (bestPending < 0 && s.state == TerrainStreamer::State::Resident && s.detailWanted &&
                s.detail < 0)
                bestPending = int(i);
            if (!detailRunnable(i))
                continue;
            if (TerrainDetailPolicy::cooling(
                    elapsed_, meta_[i].restartAfter,
                    bestPending >= 0 &&
                        TerrainDetailPolicy::before(meta_[bestPending].rank, meta_[i].rank)))
                continue;
            // Persistence is optional; urgent missing detail can reclaim scratch.
            if (config_.hierarchical && gen_.published && meta_[i].rank.protectedNear)
                cancelGeneration();
            else if (gen_.detailOnly && gen_.stage < 5 &&
                     TerrainDetailPolicy::preempt(meta_[gen_.handle.index].rank, meta_[i].rank,
                                                  elapsed_ - gen_.started, generationProgress()))
                cancelGeneration(true);
            break;
        }
    }
    if (gen_.active && gen_.cancelled && !gen_.exportPending && completed_ >= gen_.readyAfter) {
        if (gen_.lease >= 0)
            cache_->release(gen_.lease);
        if (gen_.directDetail >= 0 && !gen_.published)
            details_[gen_.directDetail] = {};
        gen_ = {};
    }
    pollCache();
    schedule();
    stats_.coverage = stats_.detail = stats_.pending = stats_.detailPending = stats_.detailReady = 0;
    stats_.residentLevels.fill(0);
    stats_.pressureEvents = stream_->pressureEvents();
    stats_.ioJobs = cache_->pending();
    for (const auto &s : stream_->slots()) {
        if (s.state == TerrainStreamer::State::Resident) {
            ++stats_.coverage;
            ++stats_.residentLevels[s.level];
            if (s.detail >= 0 || !layout_.detailBytes) {
                ++stats_.detail;
                if (s.detailWanted)
                    ++stats_.detailReady;
            } else if (s.detailWanted)
                ++stats_.detailPending;
        } else if (s.state != TerrainStreamer::State::Empty &&
                   s.state != TerrainStreamer::State::Retiring)
            ++stats_.pending;
    }
    stats_.desired = config_.hierarchical
                         ? stream_->demandCount()
                         : uint32_t((2 * stream_->radius() + 1) * (2 * stream_->radius() + 1));
    rangeTarget_ = std::max(rangeTarget_, range_);
    for (uint32_t i = 0; i < capacity_; ++i)
        if (!config_.hierarchical && stream_->slot(i).state == TerrainStreamer::State::Resident)
            rangeTarget_ = std::max(rangeTarget_, meta_[i].range);
    range_ = std::min(rangeTarget_, range_ + std::max(64.f, rangeTarget_) * dt_ * 4);
    stats_.transferBytes = stats_.uploadBytes = stats_.cacheReadbackBytes = stats_.directBytes = 0;
    stats_.dispatches = stats_.generationGroups = stats_.generationGroupBudget =
        stats_.generationSegments = 0;
    stats_.generationEstimatedMilliseconds = 0;
    stats_.generationStop = TerrainGenerationStop::Idle;
}
void VulkanTerrainStream::setCoverageBounds(TerrainHandle h, int lease) {
    std::memcpy(boundsScratch_.data(), cache_->data(lease) + layout_.boundsOffset,
                size_t(layout_.boundsBytes));
    applyBounds(h, boundsScratch_.data());
}
void VulkanTerrainStream::applyBounds(TerrainHandle h, const glm::vec2 *bounds) {
    lod_->setBounds(h.index, bounds);
    if (config_.hierarchical)
        lod_->setQualityMetadata(h.index, bounds + layout_.leafBoundsBytes / 8);
    auto &meta = meta_[h.index];
    meta.range = config_.hierarchical ? 64 : lod_->requiredRange(h.index);
    meta.heights = {INFINITY, -INFINITY};
    for (uint64_t i = 0; i < layout_.leafBoundsBytes / sizeof(glm::vec2); ++i) {
        meta.heights.x = std::min(meta.heights.x, bounds[i].x);
        meta.heights.y = std::max(meta.heights.y, bounds[i].y);
    }
}
void VulkanTerrainStream::pollCache() {
    for (int i = 0; i < 2; ++i) {
        if (upload_.lease == i || gen_.lease == i || !cache_->complete(i))
            continue;
        const auto h = cache_->handle(i);
        const auto product = cache_->key(i).product;
        const auto result = cache_->result(i);
        const auto revision = cache_->revision(i);
        if (h.index == invalidTerrainSlot) {
            if (result == TerrainCache::Result::Error)
                error();
            cache_->release(i);
            continue;
        }
        const auto clearRead = [&] {
            if (stream_->valid(h) && meta_[h.index].readRevision == revision &&
                meta_[h.index].readLease == i) {
                meta_[h.index].readRevision = 0;
                meta_[h.index].readLease = -1;
            }
            reads_[i] = {};
        };
        if (!stream_->valid(h) || reads_[i].cancelled || result == TerrainCache::Result::Cancelled ||
            (product == TerrainProduct::Detail && !detailValid(h, revision))) {
            ++stats_.discardedReads;
            clearRead();
            cache_->release(i);
            continue;
        }
        if (result == TerrainCache::Result::Hit) {
            if (upload_.lease >= 0)
                continue;
            int detail = -1;
            if (product == TerrainProduct::Detail && (detail = allocateDetail(h)) < 0)
                continue; // Keep the matching read lease; do not reread it.
            ++stats_.hits;
            upload_ = {i, h, product, detail, 0, false, revision};
            clearRead();
            if (product == TerrainProduct::Coverage) {
                setCoverageBounds(h, i);
                stream_->slot(h.index).state = TerrainStreamer::State::Working;
            }
        } else {
            ++stats_.misses;
            if (result == TerrainCache::Result::Error)
                error();
            clearRead();
            if (product == TerrainProduct::Coverage)
                stream_->slot(h.index).state = TerrainStreamer::State::Generate;
            else
                meta_[h.index].detailMissing = true;
            cache_->release(i);
        }
    }
}
void VulkanTerrainStream::schedule() {
    // Leave a mailbox available for completed generation instead of starving
    // export with speculative reads.
    if (!config_.hierarchical && gen_.active && gen_.stage == 5 && gen_.lease < 0)
        return;
    const auto requestCoverage = [&](TerrainHandle h) {
        if (h.index == invalidTerrainSlot)
            return;
        if (!config_.diskCache)
            stream_->slot(h.index).state = TerrainStreamer::State::Generate;
        else {
            const int lease = cache_->read(key(h, TerrainProduct::Coverage), h);
            if (lease >= 0) {
                reads_[lease] = {h, 0, true};
                stream_->slot(h.index).state = TerrainStreamer::State::Reading;
            }
        }
    };
    const auto generateCoverage = [&](TerrainHandle h) {
        if (h.index == invalidTerrainSlot || gen_.active)
            return;
        gen_ = {};
        gen_.handle = h;
        gen_.request = {stream_->slot(h.index).chunk, settings_};
        gen_.active = true;
        stream_->slot(h.index).state = TerrainStreamer::State::Working;
    };
    auto h = stream_->next(TerrainStreamer::State::Requested);
    if (h.index != invalidTerrainSlot && stream_->slot(h.index).visible)
        requestCoverage(h);
    h = stream_->next(TerrainStreamer::State::Generate);
    if (h.index != invalidTerrainSlot && stream_->slot(h.index).visible &&
        (!config_.hierarchical || stats_.generations % 3 != 2))
        generateCoverage(h);
    // Sorted candidates let a better runnable request bypass cooled work.
    // If every better request is busy, even a cooled request may keep scratch
    // useful.
    int fallback = -1, bestPending = -1;
    bool readIssued = false;
    const auto startDetail = [&](uint32_t i) {
        const auto &s = stream_->slot(i);
        gen_ = {};
        gen_.handle = {i, s.generation};
        gen_.request = {s.chunk, settings_};
        gen_.active = gen_.detailOnly = true;
        gen_.revision = meta_[i].revision;
        gen_.started = elapsed_;
        if (config_.hierarchical)
            gen_.stage = 1;
    };
    for (auto i : priorities_) {
        const auto &s = stream_->slot(i);
        if (bestPending < 0 && s.state == TerrainStreamer::State::Resident && s.detailWanted &&
            s.detail < 0)
            bestPending = int(i);
        if (!detailRunnable(i))
            continue;
        auto &m = meta_[i];
        h = {i, s.generation};
        if (m.detailMissing || !config_.diskCache) {
            if (!gen_.active) {
                const bool better =
                    bestPending >= 0 && TerrainDetailPolicy::before(meta_[bestPending].rank, m.rank);
                if (TerrainDetailPolicy::cooling(elapsed_, m.restartAfter, better)) {
                    if (fallback < 0)
                        fallback = int(i);
                    continue;
                }
                startDetail(i);
                break;
            }
        } else if (!readIssued) {
            const int lease = cache_->read(key(h, TerrainProduct::Detail), h, m.revision);
            if (lease >= 0) {
                m.readRevision = m.revision;
                m.readLease = lease;
                reads_[lease] = {h, m.revision, true};
            }
            readIssued = true;
        }
    }
    // Refinement precedes the speculative outer ring, both on disk and on the
    // GPU.
    h = stream_->next(TerrainStreamer::State::Requested);
    if (h.index != invalidTerrainSlot && !stream_->slot(h.index).visible)
        requestCoverage(h);
    if (!gen_.active)
        generateCoverage(stream_->next(TerrainStreamer::State::Generate));
    if (!gen_.active && fallback >= 0)
        startDetail(uint32_t(fallback)); // A cooldown must never leave scratch idle.
}
void VulkanTerrainStream::generate(VkCommandBuffer cmd, uint32_t frame) {
    if (!gen_.active || gen_.cancelled || gen_.stage >= 5 || !stream_->valid(gen_.handle))
        return;
    if (!generatorImagesInitialized_) {
        for (uint32_t l = 0; l < layout_.levels; ++l)
            imageGeneral(cmd, g_.normalImages_[l], 2);
        generatorImagesInitialized_ = true;
    }
    auto grid = layout_.grid;
    grid.spacing *= stream_->scale(gen_.handle.index);
    auto parameters = terrainParameters(grid, gen_.request);
    parameters.dimensions.w = layout_.subdivisions;
    parameters.sampling = {layout_.halos[0], layout_.patchCells, layout_.levels,
                           config_.hierarchical ? 1u | (config_.unfilteredHeightReference ? 2u : 0u) |
                                                      (stream_->slot(gen_.handle.index).level << 8)
                                                : 0u};
    std::memcpy(g_.parameters_[frame].mapped, &parameters, sizeof(parameters));
    auto &f = frames_[frame];
    f.segmentCount = f.generationGroups = 0;
    TerrainGenerationFrameBudget work;
    stats_.generationGroupBudget = TerrainGenerationBudget::maximumGroups;
    while (gen_.stage < 5) {
        uint32_t width = 0, level = 0, groupSide = 8;
        VkPipeline pipeline{};
        TerrainComputeKernel kernel{};
        switch (gen_.stage) {
        case 0:
            level = config_.hierarchical ? gen_.level : 0;
            width = config_.hierarchical ? (layout_.grid.cells >> level) + 1 : layout_.widths[0] + 2;
            pipeline = config_.hierarchical ? tileHeightPipeline_ : g_.heightPipeline_;
            kernel = !config_.hierarchical ? TerrainComputeKernel::Height
                     : level + stream_->slot(gen_.handle.index).level == 0
                         ? TerrainComputeKernel::TileHeight
                         : TerrainComputeKernel::TileFilteredHeight;
            break;
        case 1:
            level = config_.hierarchical ? (gen_.detailOnly ? gen_.level : layout_.levels - 1) : 0;
            width = layout_.widths[level];
            pipeline = config_.hierarchical ? tileNormalPipeline_ : g_.normalPipeline_;
            kernel = !config_.hierarchical ? TerrainComputeKernel::Normal
                     : level + stream_->slot(gen_.handle.index).level == 0
                         ? TerrainComputeKernel::TileNormal
                         : TerrainComputeKernel::TileFilteredNormal;
            break;
        case 2:
            level = gen_.level;
            width = layout_.widths[level];
            pipeline = g_.filterPipeline_;
            kernel = TerrainComputeKernel::Filter;
            break;
        case 3:
            level = config_.hierarchical ? gen_.level : 0;
            width = (layout_.grid.cells / layout_.patchCells) >> level;
            groupSide = 1;
            pipeline = config_.hierarchical ? qualityBoundsPipeline_ : g_.boundsPipeline_;
            kernel = config_.hierarchical ? TerrainComputeKernel::QualityBounds
                                          : TerrainComputeKernel::Bounds;
            break;
        default:
            width = layout_.grid.cells + 1;
            pipeline = packPipeline_;
            kernel = TerrainComputeKernel::Pack;
            break;
        }
        const uint32_t rowGroups = (width + groupSide - 1) / groupSide;
        const uint32_t totalGroups = rowGroups * rowGroups;
        const double cost = generationBudget_.groupMilliseconds(kernel);
        uint32_t remaining = work.allowance(cost);
        if (!remaining)
            break;
        auto &segment = f.segments[work.segments];
        segment = {kernel, 0};
        const uint32_t query = frame * queriesPerFrame + 2 + work.segments * 2;
        if (queryPool_) {
            vkCmdResetQueryPool(cmd, queryPool_, query, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query);
        }
        memoryBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_.computeLayout_, 0, 1,
                                &g_.slots_[0].descriptors[frame][std::min(level, layout_.levels - 1)], 0,
                                nullptr);
        while (work.dispatches < TerrainGenerationBudget::maximumDispatches && remaining &&
               gen_.group < totalGroups) {
            const auto batch = terrainComputeBatch(rowGroups, gen_.group, remaining);
            const glm::uvec4 push[2] = {{width, layout_.halos[std::min(level, layout_.levels - 1)],
                                         layout_.halos[level ? level - 1 : 0], level},
                                        {batch.x * groupSide, batch.y * groupSide, 0, 0}};
            vkCmdPushConstants(cmd, g_.computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                               push);
            vkCmdDispatch(cmd, batch.width, batch.height, 1);
            work.consume(batch.groups(), cost);
            gen_.group += batch.groups();
            gen_.groups += batch.groups();
            segment.groups += batch.groups();
            remaining -= batch.groups();
        }
        if (gen_.group == totalGroups) {
            gen_.group = 0;
            memoryBarrier(cmd);
            if (gen_.stage == 0 && config_.hierarchical) {
                if (++gen_.level > layout_.levels) {
                    gen_.level = 0;
                    gen_.stage = 1;
                }
            } else if (gen_.stage == 3 && config_.hierarchical) {
                if (++gen_.level >= layout_.levels) {
                    gen_.level = 0;
                    gen_.stage = 5;
                }
            } else if (gen_.stage == 1 && config_.hierarchical) {
                if (!gen_.detailOnly)
                    gen_.stage = 3;
                else if (++gen_.level + 1 >= layout_.levels)
                    gen_.stage = 5;
            } else if (gen_.stage == 1) {
                gen_.level = 1;
                gen_.stage = layout_.levels > 1 ? 2 : 3;
            } else if (gen_.stage == 2) {
                if (++gen_.level >= layout_.levels)
                    gen_.stage = 3;
            } else
                ++gen_.stage;
        }
        if (queryPool_)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, query + 1);
        ++work.segments;
    }
    f.generationGroups = stats_.generationGroups = work.groups;
    f.segmentCount = stats_.generationSegments = work.segments;
    f.timestamp = queryPool_ && work.segments > 0;
    stats_.dispatches = work.dispatches;
    stats_.generationEstimatedMilliseconds = work.milliseconds;
    stats_.generationStop = gen_.stage == 5 ? TerrainGenerationStop::JobComplete : work.limit();
    if (gen_.detailOnly && !stream_->slot(gen_.handle.index).detailWanted)
        stats_.obsoleteDetailGroups += work.groups;
    gen_.readyAfter = serial_ + 1;
    if (gen_.stage == 5) {
        ++stats_.generations;
        gen_.product = gen_.detailOnly ? TerrainProduct::Detail : TerrainProduct::Coverage;
    }
}
uint64_t VulkanTerrainStream::copyPayload(VkCommandBuffer cmd, uint32_t frame, TerrainProduct product,
                                          bool uploading, uint64_t offset, uint64_t budget,
                                          uint32_t slot, int detail) {
    uint64_t copied = 0;
    const auto total = layout_.size(product);
    auto &staging = uploading ? uploads_[frame] : readbacks_[frame];
    memoryBarrier(cmd);
    while (offset < total && budget >= 16) {
        uint64_t start = 0, size = 0;
        VkBuffer buffer{};
        VkDeviceSize bufferBase = 0;
        VkImage image{};
        uint32_t width = 0, layerBase = 0;
        if (product == TerrainProduct::Coverage) {
            if (offset < layout_.boundsOffset) {
                size = layout_.heightsBytes;
                buffer = uploading ? heights_.handle : g_.heights_.handle;
                bufferBase = uploading ? heightStride_ * slot : 0;
            } else if (offset < layout_.coarseOffset) {
                start = layout_.boundsOffset;
                size = layout_.boundsBytes;
                buffer = g_.bounds_.handle;
                if (uploading) {
                    const auto n = std::min(budget, size - (offset - start));
                    offset += n;
                    copied += n;
                    budget -= std::min(budget, n);
                    continue;
                }
            } else {
                start = layout_.coarseOffset;
                size = layout_.bytes[layout_.levels - 1];
                width = layout_.widths[layout_.levels - 1];
                image = uploading ? coarse_.image : g_.normalImages_[layout_.levels - 1];
                layerBase = uploading ? slot * 2 : 0;
            }
        } else {
            for (uint32_t l = 0; l + 1 < layout_.levels; ++l)
                if (offset >= layout_.offsets[l] && offset < layout_.offsets[l] + layout_.bytes[l]) {
                    start = layout_.offsets[l];
                    size = layout_.bytes[l];
                    width = layout_.widths[l];
                    image = uploading ? fine_[l].image : g_.normalImages_[l];
                    layerBase = uploading ? uint32_t(detail) * 2 : 0;
                    break;
                }
        }
        const auto local = offset - start;
        uint64_t n = std::min(budget, size - local);
        if (image) {
            const uint64_t rowBytes = uint64_t(width) * 8;
            const uint32_t row = uint32_t(local / rowBytes), y = row % width, layer = row / width;
            const auto rows = uint32_t(std::min<uint64_t>(n / rowBytes, width - y));
            if (!rows)
                break;
            n = rows * rowBytes;
            VkBufferImageCopy c{};
            c.bufferOffset = copied;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layerBase + layer, 1};
            c.imageOffset = {0, int32_t(y), 0};
            c.imageExtent = {width, rows, 1};
            if (uploading)
                vkCmdCopyBufferToImage(cmd, staging.handle, image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            else
                vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, staging.handle, 1, &c);
        } else {
            n -= n % 4;
            if (!n)
                break;
            VkBufferCopy c = uploading ? VkBufferCopy{copied, bufferBase + local, n}
                                       : VkBufferCopy{bufferBase + local, copied, n};
            vkCmdCopyBuffer(cmd, uploading ? staging.handle : buffer,
                            uploading ? buffer : staging.handle, 1, &c);
        }
        if (uploading)
            std::memcpy(static_cast<uint8_t *>(staging.mapped) + copied,
                        cache_->data(upload_.lease) + offset, size_t(n));
        copied += n;
        offset += n;
        budget -= n;
    }
    memoryBarrier(cmd);
    if (!uploading) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    return copied;
}

bool VulkanTerrainStream::directPublication(VkCommandBuffer cmd, uint32_t frame, uint64_t &budget) {
    if (!config_.hierarchical || !gen_.active || gen_.cancelled || gen_.stage != 5 || gen_.published ||
        !stream_->valid(gen_.handle) || completed_ < gen_.readyAfter)
        return false;
    const auto h = gen_.handle;
    if (gen_.product == TerrainProduct::Detail && gen_.directDetail < 0) {
        if ((gen_.directDetail = allocateDetail(h)) < 0)
            return true; // Keep useful completed scratch while reserved slots retire.
    }
    memoryBarrier(cmd);
    const auto product = gen_.product;
    const auto total = layout_.size(product);
    while (gen_.directOffset < total && budget >= 16) {
        const uint64_t offset = gen_.directOffset;
        if (product == TerrainProduct::Coverage && offset < layout_.boundsOffset) {
            const auto n = std::min(budget & ~uint64_t(7), layout_.heightsBytes - offset);
            VkBufferCopy c{offset, heightStride_ * h.index + offset, n};
            vkCmdCopyBuffer(cmd, g_.heights_.handle, heights_.handle, 1, &c);
            gen_.directOffset += n;
            budget -= n;
            stats_.directBytes += n;
            continue;
        }
        if (product == TerrainProduct::Coverage && offset < layout_.coarseOffset) {
            gen_.directOffset = layout_.coarseOffset;
            continue;
        }
        uint32_t level = layout_.levels - 1;
        uint64_t start = layout_.coarseOffset;
        if (product == TerrainProduct::Detail) {
            for (level = 0; level + 1 < layout_.levels; ++level)
                if (offset < layout_.offsets[level] + layout_.bytes[level])
                    break;
            start = layout_.offsets[level];
        }
        const uint32_t width = layout_.widths[level];
        const uint64_t rowBytes = uint64_t(width) * 8;
        const auto row = uint32_t((offset - start) / rowBytes), y = row % width, layer = row / width;
        const auto rows = uint32_t(std::min<uint64_t>(budget / rowBytes, width - y));
        if (!rows)
            break;
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        c.dstSubresource = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0,
            (product == TerrainProduct::Coverage ? h.index : uint32_t(gen_.directDetail)) * 2 + layer,
            1};
        c.srcOffset = c.dstOffset = {0, int32_t(y), 0};
        c.extent = {width, rows, 1};
        vkCmdCopyImage(cmd, g_.normalImages_[level], VK_IMAGE_LAYOUT_GENERAL,
                       product == TerrainProduct::Coverage ? coarse_.image : fine_[level].image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        const auto bytes = rows * rowBytes;
        gen_.directOffset += bytes;
        budget -= bytes;
        stats_.directBytes += bytes;
    }
    if (gen_.directOffset == total &&
        (product != TerrainProduct::Coverage || budget >= layout_.boundsBytes)) {
        if (product == TerrainProduct::Coverage) {
            VkBufferCopy c{0, 0, layout_.boundsBytes};
            vkCmdCopyBuffer(cmd, g_.bounds_.handle, readbacks_[frame].handle, 1, &c);
            frames_[frame].boundsHandle = h;
            budget -= layout_.boundsBytes;
            stats_.directBytes += layout_.boundsBytes;
        }
        frames_[frame].publish = {h, product, gen_.directDetail, true, gen_.revision};
        gen_.published = true;
    }
    memoryBarrier(cmd);
    // Make the tiny bounds readback visible after this frame's fence.
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dependency);
    gen_.readyAfter = serial_ + 1;
    stats_.transferBytes += stats_.directBytes;
    return true;
}
void VulkanTerrainStream::transfer(VkCommandBuffer cmd, uint32_t frame, uint64_t &budget) {
    if (upload_.lease < 0 && directPublication(cmd, frame, budget))
        return;
    if (config_.hierarchical && gen_.active && !gen_.cancelled && gen_.published &&
        completed_ >= gen_.readyAfter && !gen_.exportPending) {
        if (gen_.lease >= 0 && gen_.offset == layout_.size(gen_.product)) {
            cache_->write(gen_.lease, key(gen_.handle, gen_.product));
            gen_ = {};
        } else if (gen_.lease < 0) {
            if (config_.diskCache && serial_ >= ioBackoffUntil_)
                gen_.lease = cache_->acquire();
            if (gen_.lease < 0) {
                if (config_.diskCache)
                    ++stats_.skippedExports;
                gen_ = {}; // Optional persistence must never gate visibility.
            }
        }
    }
    if (gen_.active && !gen_.cancelled && gen_.stage == 5 && stream_->valid(gen_.handle) &&
        completed_ >= gen_.readyAfter && !gen_.exportPending) {
        if (!config_.hierarchical && gen_.lease >= 0 && gen_.offset == layout_.size(gen_.product) &&
            upload_.lease < 0) {
            const auto h = gen_.handle;
            const auto product = gen_.product;
            const bool wantUpload =
                product == TerrainProduct::Coverage || stream_->slot(h.index).detailWanted;
            int detail = -1;
            if (product == TerrainProduct::Detail && wantUpload)
                detail = allocateDetail(h);
            if (product == TerrainProduct::Coverage || (wantUpload && detail >= 0)) {
                upload_ = {gen_.lease, h, product, detail, 0, true, gen_.revision};
                if (product == TerrainProduct::Coverage) {
                    setCoverageBounds(h, gen_.lease);
                }
            } else {
                if (config_.diskCache && serial_ >= ioBackoffUntil_)
                    cache_->write(gen_.lease, key(h, product));
                else
                    cache_->release(gen_.lease);
            }
            gen_.lease = -1;
            gen_.offset = 0;
            if (!config_.hierarchical && product == TerrainProduct::Coverage && layout_.detailBytes) {
                gen_.product = TerrainProduct::Detail;
                gen_.revision = meta_[h.index].revision;
                if (!stream_->slot(h.index).detailWanted)
                    cancelGeneration();
            } else {
                meta_[h.index].detailMissing = false;
                gen_ = {};
            }
        }
    }
    if (upload_.lease >= 0 && stream_->valid(upload_.handle)) {
        const auto n = copyPayload(cmd, frame, upload_.product, true, upload_.offset, budget,
                                   upload_.handle.index, upload_.detail);
        upload_.offset += n;
        if (n)
            upload_.readyAfter = serial_ + 1;
        budget -= n;
        stats_.transferBytes += n;
        stats_.uploadBytes += n;
        if (upload_.offset == layout_.size(upload_.product)) {
            frames_[frame].publish = {upload_.handle, upload_.product, upload_.detail, true,
                                      upload_.revision};
            if (upload_.generated && config_.diskCache && serial_ >= ioBackoffUntil_)
                cache_->write(upload_.lease, key(upload_.handle, upload_.product));
            else
                cache_->release(upload_.lease);
            upload_ = {};
        }
        // One transfer direction per frame keeps staging offsets and ownership
        // simple.
        return;
    }
    if (gen_.active && !gen_.cancelled && gen_.stage == 5 && stream_->valid(gen_.handle) &&
        completed_ >= gen_.readyAfter && !gen_.exportPending &&
        (!config_.hierarchical || gen_.published)) {
        if (gen_.lease < 0)
            gen_.lease = cache_->acquire();
        if (gen_.lease >= 0) {
            const auto n = copyPayload(cmd, frame, gen_.product, false, gen_.offset, budget, 0, -1);
            if (n) {
                frames_[frame].exportLease = gen_.lease;
                frames_[frame].exportOffset = gen_.offset;
                frames_[frame].exportBytes = n;
                gen_.exportPending = true;
                gen_.readyAfter = serial_ + 1;
                budget -= n;
                stats_.transferBytes += n;
                stats_.cacheReadbackBytes += n;
            }
        }
    }
}
void VulkanTerrainStream::record(VkCommandBuffer cmd, uint32_t frame) {
    if (queryPool_) {
        vkCmdResetQueryPool(cmd, queryPool_, frame * queriesPerFrame, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_,
                             frame * queriesPerFrame);
        frames_[frame].terrainTimestamp = true;
    }
    if (!imagesInitialized_) {
        imageGeneral(cmd, coarse_.image, coarse_.layers);
        for (uint32_t l = 0; l + 1 < layout_.levels; ++l)
            imageGeneral(cmd, fine_[l].image, fine_[l].layers);
        imagesInitialized_ = true;
    }
    generate(cmd, frame);
    uint64_t budget = config_.transferBytesPerFrame;
    transfer(cmd, frame, budget);
    const auto job = detailJob();
    stats_.activeDetail = job.phase != TerrainDetailPhase::Idle;
    stats_.activeDetailProgress = float(job.progress);
    stats_.detailRetiring = job.cancelled;
    if (stream_->valid(job.handle))
        stats_.activeDetailTile = stream_->slot(job.handle.index).tile();
}
void VulkanTerrainStream::prepareDraw(uint32_t frame, const Camera &camera, const Camera &observer,
                                      const glm::mat4 &vp, const SceneSettings &settings,
                                      uint32_t viewportHeight) {
    const auto eye = camera.terrainOffset(observer.chunk, layout_.grid) + glm::vec3(observer.local);
    struct View {
        glm::mat4 matrix;
        glm::vec4 observer, grid, camera, clip;
    };
    const View view{
        vp, glm::vec4(eye, 0),
        glm::vec4(layout_.grid.cells, layout_.grid.spacing, layout_.subdivisions, layout_.patchCells),
        config_.hierarchical ? glm::vec4(glm::vec3(camera.local), 0) : glm::vec4(0),
        glm::vec4(-stream_->radius() * layout_.grid.extent() - camera.local.x,
                  -stream_->radius() * layout_.grid.extent() - camera.local.z,
                  (stream_->radius() + 1) * layout_.grid.extent() - camera.local.x,
                  (stream_->radius() + 1) * layout_.grid.extent() - camera.local.z)};
    std::memcpy(views_[frame].mapped, &view, sizeof(view));
    lod_->collectCasters(settings.shadows.mode != ShadowMode::Off && settings.lighting.sunEnabled &&
                         !settings.wireframe);
    activeCoords_.clear();
    activeSlots_.clear();
    if (config_.hierarchical)
        lod_->beginHierarchy(camera, observer, vp, stream_->radius());
    for (uint32_t i = 0; i < capacity_; ++i) {
        auto &s = stream_->slot(i);
        if (s.state != TerrainStreamer::State::Resident || !s.visible || meta_[i].range > range_)
            continue;
        float weights[48];
        std::fill(weights + 32, weights + 48, 0.f);
        stream_->weights(i, weights);
        if (config_.hierarchical)
            stream_->heightWeights(i, weights + 16);
        else
            std::fill(weights + 16, weights + 32, 1.f);

        s.lastUse = serial_ + 1;
        if (config_.hierarchical) {
            const auto scale = int64_t(1) << s.level;
            auto envelope = meta_[i].heights;
            const int parent = stream_->findTile(terrainParent(s.tile()));
            if (parent >= 0 && stream_->slot(parent).state == TerrainStreamer::State::Resident) {
                envelope.x = std::min(envelope.x, meta_[parent].heights.x);
                envelope.y = std::max(envelope.y, meta_[parent].heights.y);
                stream_->slot(parent).lastUse = serial_ + 1;
                stream_->weights(uint32_t(parent), weights + 32);
            }
            lod_->selectTile(i, {s.chunk.x * scale, s.chunk.z * scale}, s.level, s.drawMask,
                             settings.lod, settings.frustumCulling, envelope);
        } else {
            activeCoords_.push_back(s.chunk);
            activeSlots_.push_back(i);
        }
        std::memcpy(static_cast<uint8_t *>(weights_[frame].mapped) + weightStride_ * i, weights,
                    sizeof(weights));
    }
    if (!config_.hierarchical)
        lod_->select(activeCoords_.data(), activeSlots_.data(), uint32_t(activeSlots_.size()), camera,
                     observer, vp, settings.lod, settings.frustumCulling, true, range_);
    if (config_.hierarchical) {
        const auto baseline = lod_->stats().triangles;
        const float target = std::clamp(settings.terrainLodPixelError, .5f, 8.f);
        if (!settings.freezeLod || qualityFocalPixels_ == 0)
            qualityFocalPixels_ = float(viewportHeight) * .5f / std::tan(glm::radians(30.f));
        const auto error = [&](const TerrainPatchDraw &p) {
            const auto &tile = stream_->slot(p.slot);
            const auto scale = int64_t(1) << tile.level;
            return lod_->projectedError(p, {tile.chunk.x * scale, tile.chunk.z * scale}, tile.level,
                                        observer, qualityFocalPixels_, settings.eroded);
        };
        const auto select = [&](float scale) {
            lod_->beginHierarchy(camera, observer, vp, stream_->radius(), terrainHierarchyRange(scale));
            for (uint32_t i = 0; i < capacity_; ++i) {
                const auto &tile = stream_->slot(i);
                if (tile.state != TerrainStreamer::State::Resident || !tile.visible)
                    continue;
                const auto stride = int64_t(1) << tile.level;
                auto envelope = meta_[i].heights;
                const int parent = stream_->findTile(terrainParent(tile.tile()));
                if (parent >= 0 && stream_->slot(parent).state == TerrainStreamer::State::Resident)
                    envelope = {std::min(envelope.x, meta_[parent].heights.x),
                                std::max(envelope.y, meta_[parent].heights.y)};
                lod_->selectTile(i, {tile.chunk.x * stride, tile.chunk.z * stride}, tile.level,
                                 tile.drawMask, settings.lod, settings.frustumCulling, envelope);
            }
        };
        float maximum = 0;
        for (const auto &p : lod_->draws())
            maximum = std::max(maximum, error(p));
        float desired = maximum < target * .5f ? 1.f : qualityScale_;
        if (settings.terrainLodQuality && settings.lod && maximum > target)
            desired = std::clamp(std::sqrt(maximum / target), 1.f, terrainLodMaximumScale);
        // One shared scale preserves the nesting and coherent anchor proof.
        // Eight bounded trial selections enforce the geometry allowance.
        float proposed = qualityScale_ + std::clamp(desired - qualityScale_, -dt_ * terrainLodScaleSlew,
                                                    dt_ * terrainLodScaleSlew);
        if (settings.freezeLod)
            proposed = qualityScale_;
        if (!settings.terrainLodQuality || !settings.lod)
            proposed = 1;
        float lo = 1, hi = proposed;
        select(proposed);
        bool limited = lod_->stats().triangles > baseline * 2;
        if (limited) {
            for (int iteration = 0; iteration < 8; ++iteration) {
                const float mid = (lo + hi) * .5f;
                select(mid);
                if (lod_->stats().triangles <= baseline * 2)
                    lo = mid;
                else
                    hi = mid;
            }
            proposed = lo;
            select(proposed);
        }
        qualityScale_ = proposed;
        maximum = 0;
        uint32_t over = 0;
        for (const auto &p : lod_->draws()) {
            const float e = error(p);
            maximum = std::max(maximum, e);
            over += e > target;
        }
        lod_->qualityStats(baseline, maximum, over, limited || (over && proposed >= 1.999f));
    }
}
void VulkanTerrainStream::collectDrawRecords(uint32_t frame, const Camera &camera,
                                             const Camera &observer, const SceneSettings &settings,
                                             bool casters, std::vector<DrawRecord> &records) {
    records.clear();
    uint32_t order = 0;
    for (const auto &patch : (casters ? lod_->casterDraws() : lod_->draws())) {
        auto &s = stream_->slot(patch.slot);
        s.lastUse = serial_ + 1;
        const auto set = sets_[(frame * capacity_ + patch.slot) * layout_.levels + patch.level];
        const auto indices = g_.patchTopology_.ranges[patch.mask];
        const float range = lod_->stats().firstRange * patch.step * stream_->scale(patch.slot);
        struct Draw {
            glm::vec4 offset;
            glm::uvec4 options, patch;
            glm::vec4 morph;
            glm::uvec4 normals, indices, parent;
            glm::vec4 tile;
        };
        const int parent = config_.hierarchical && !patch.exterior && s.level < stream_->topLevel()
                               ? stream_->findTile(terrainParent(s.tile()))
                               : -1;
        const uint32_t qx = uint32_t(s.chunk.x - terrainFloorDivide(s.chunk.x, 2) * 2);
        const uint32_t qz = uint32_t(s.chunk.z - terrainFloorDivide(s.chunk.z, 2) * 2);
        auto offset = stream_->offset(patch.slot, camera);
        if (config_.hierarchical) {
            const auto scale = int64_t(1) << s.level;
            offset = {float(double(s.chunk.x * scale - camera.chunk.x) * layout_.grid.extent()), 0,
                      float(double(s.chunk.z * scale - camera.chunk.z) * layout_.grid.extent())};
        }
        const Draw draw{
            glm::vec4(offset, 0),
            {settings.eroded, settings.filteredNormals, layout_.grid.cells,
             (settings.normalView ? 1u : 0u) | (settings.wireframe ? 2u : 0u) |
                 (settings.chunkBorders ? 4u : 0u) | (settings.lodColours ? 8u : 0u) |
                 (patch.exterior ? 16u : 0u)},
            {patch.x, patch.z, patch.step, layout_.patchCells},
            {range * .75f, range, float(patch.level),
             settings.lod && (patch.level + 1 < layout_.levels || parent >= 0) ? 1.f : 0.f},
            {layout_.halos[patch.level], layout_.halos[std::min(patch.level + 1, layout_.levels - 1)],
             layout_.subdivisions, uint32_t(s.detail)},
            {indices.first, patch.slot,
             parent >= 0 ? uint32_t(stream_->slot(parent).detail) : invalidTerrainSlot,
             layout_.levels - 1},
            {parent >= 0 ? uint32_t(parent) : invalidTerrainSlot, qx * layout_.grid.cells / 2,
             qz * layout_.grid.cells / 2, uint32_t(heightStride_ / 8)},
            {layout_.grid.spacing * stream_->scale(patch.slot), config_.hierarchical ? 1.f : 0.f,
             float(s.level),
             settings.terrainLodErrorView
                 ? std::min(1.f, lod_->projectedError(patch,
                                                      {s.chunk.x * (int64_t(1) << s.level),
                                                       s.chunk.z * (int64_t(1) << s.level)},
                                                      s.level, observer, qualityFocalPixels_,
                                                      settings.eroded) /
                                     std::max(.5f, settings.terrainLodPixelError))
                 : -1.f}};
        DrawRecord record{};
        record.set = set;
        record.count = indices.count;
        record.first = indices.first;
        record.group = patch.slot * layout_.levels + patch.level;
        record.order = order;
        record.bounds = (casters ? lod_->casterBounds() : lod_->drawBounds())[order++];
        std::memcpy(record.constants.data(), &draw, sizeof(draw));
        records.push_back(record);
    }
}
void VulkanTerrainStream::prepareVisibility(VulkanVisibility &visibility, uint32_t frame,
                                            const Camera &camera, const Camera &observer,
                                            const SceneSettings &settings, VulkanPost *post) {
    diagnosticDraws_.clear();
    preparedDraws_.clear();
    collectDrawRecords(frame, camera, observer, settings, false, preparedDraws_);
    if (config_.vertexDiagnostics)
        diagnosticDraws_.assign(preparedDraws_.begin(), preparedDraws_.end());
    std::sort(preparedDraws_.begin(), preparedDraws_.end(),
              [](const DrawRecord &a, const DrawRecord &b) {
                  return a.group != b.group ? a.group < b.group : a.order < b.order;
              });
    if (post)
        std::memcpy(views_[frame].mapped, &post->view().raster, sizeof(glm::mat4));
    history_.begin(post && post->stats().historyValid);
    uint32_t temporalIndex = 0;
    uint32_t previous = ~0u;
    for (auto &record : preparedDraws_) {
        if (post && post->stats().taa) {
            auto slot = record.constants[5].y;
            const auto &resident = stream_->slot(slot);
            TemporalTerrainRecord current{};
            current.draw = record.constants;
            std::memcpy(current.weights.data(),
                        static_cast<const uint8_t *>(weights_[frame].mapped) + weightStride_ * slot,
                        192);
            current.observer = glm::vec4(
                camera.terrainOffset(observer.chunk, layout_.grid) + glm::vec3(observer.local), 0);
            current.camera = config_.hierarchical ? glm::vec4(glm::vec3(camera.local), 0) : glm::vec4(0);
            current.valid.x = 1;
            TemporalTerrainKey key;
            key.words = {uint64_t(resident.chunk.x),
                         uint64_t(resident.chunk.z),
                         resident.epoch,
                         (uint64_t(resident.generation) << 32) | slot,
                         (uint64_t(record.constants[2].x) << 32) | record.constants[2].y,
                         (uint64_t(record.constants[2].z) << 32) | record.first,
                         (uint64_t(record.count) << 32) | resident.level,
                         (uint64_t(settings.eroded) << 63) ^ meta_[slot].revision ^
                             uint64_t(uint32_t(resident.detail))};
            post->terrainRecord(temporalIndex, history_.append(key, current));
        }
        ++temporalIndex;
        record.batch = visibility.append(record.bounds, record.count, record.first, 0,
                                         record.constants.data(), 128, previous != record.group);
        previous = record.group;
    }
}
void VulkanTerrainStream::draw(VkCommandBuffer cmd, uint32_t frame, const Camera &, const Camera &,
                               const SceneSettings &settings, bool finalPass,
                               VulkanVisibility *visibility) {
    vkCmdBindIndexBuffer(cmd, g_.patchIndices_.handle, 0, VK_INDEX_TYPE_UINT32);
    if (visibility)
        visibility->bindRecords(cmd, graphicsLayout_);
    const bool indirect = visibility && visibility->indirect();
    uint32_t previous = ~0u, temporalIndex = 0;
    for (const auto &record : preparedDraws_) {
        const auto recordIndex = temporalIndex++;
        if (indirect && previous == record.batch)
            continue;
        previous = record.batch;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsLayout_, 0, 1, &record.set,
                                0, nullptr);
        auto constants = record.constants;
        if (indirect)
            constants[1].w |= 0x80000000u;
        vkCmdPushConstants(cmd, graphicsLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                               VK_SHADER_STAGE_COMPUTE_BIT,
                           0, 128, constants.data());
        if (indirect)
            visibility->draw(cmd, record.batch);
        else if (settings.wireframe)
            vkCmdDraw(cmd, record.count, 1, 0, 0);
        else
            vkCmdDrawIndexed(cmd, record.count, 1, record.first, 0, recordIndex);
    }
    if (queryPool_ && finalPass)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_,
                             frame * queriesPerFrame + 1);
}
void VulkanTerrainStream::prepareShadows(uint32_t frame, const Camera &camera,
                                         const SceneSettings &settings, VulkanShadows &shadows) {
    shadowDraws_.clear();
    if (!shadows.active() || !settings.terrainVisible)
        return;
    stats_.shadowRequested = shadowDemandCount_;
    stats_.shadowMissing = 0;
    for (uint32_t i = 0; i < shadowDemandCount_; ++i) {
        int slot = stream_->findTile(shadowDemand_[i]);
        if (slot < 0 || stream_->slot(slot).state != TerrainStreamer::State::Resident) {
            ++stats_.shadowMissing;
            auto key = shadowDemand_[i];
            const int64_t scale = int64_t(1) << key.level;
            glm::vec3 lo{
                float(double(key.x * scale - camera.chunk.x) * layout_.grid.extent() - camera.local.x),
                -2 * settings_.heightScale - float(camera.local.y),
                float(double(key.z * scale - camera.chunk.z) * layout_.grid.extent() - camera.local.z)};
            shadows.missing(
                {lo,
                 lo + glm::vec3(float(scale * layout_.grid.extent()), 4 * settings_.heightScale,
                                float(scale * layout_.grid.extent())),
                 true});
        }
    }

    for (uint32_t i = 0; i < shadowMissingBoundsCount_; ++i) {
        const auto &b = shadowMissingBounds_[i];
        const int64_t scale = int64_t(1) << stream_->topLevel();
        glm::vec3 lo{
            float(double(b[0] * scale - camera.chunk.x) * layout_.grid.extent() - camera.local.x),
            -2 * settings_.heightScale - float(camera.local.y),
            float(double(b[1] * scale - camera.chunk.z) * layout_.grid.extent() - camera.local.z)};
        shadows.missing({lo,
                         lo + glm::vec3(float((b[2] - b[0] + 1) * scale * layout_.grid.extent()),
                                        4 * settings_.heightScale,
                                        float((b[3] - b[1] + 1) * scale * layout_.grid.extent())),
                         true});
        ++stats_.shadowMissing;
    }

    // Extra root tiles only cast outside the camera footprint. The shader clips
    // them against the exact same boundary used by camera terrain.
    if (config_.hierarchical)
        for (uint32_t slot = 0; slot < capacity_; ++slot) {
            auto &tile = stream_->slot(slot);
            if (tile.state != TerrainStreamer::State::Resident || tile.level != stream_->topLevel())
                continue;
            const int64_t scale = int64_t(1) << tile.level;
            lod_->selectExteriorTile(slot, {tile.chunk.x * scale, tile.chunk.z * scale}, tile.level,
                                     meta_[slot].heights);
            if (!tile.visible) {
                float weights[48];
                std::fill_n(weights, 48, 1.f);
                std::memcpy(static_cast<uint8_t *>(weights_[frame].mapped) + weightStride_ * slot,
                            weights, sizeof(weights));
            }
        }
    collectDrawRecords(frame, camera, camera, settings, true, casterRecords_);
    std::sort(casterRecords_.begin(), casterRecords_.end(),
              [](const DrawRecord &a, const DrawRecord &b) {
                  return a.group != b.group ? a.group < b.group : a.order < b.order;
              });
    for (const auto &record : casterRecords_) {
        auto caster =
            shadows.append(record.bounds, record.count, record.first, 0, record.constants.data(), 128);
        if (caster != ~0u)
            shadowDraws_.push_back({record.set, caster});
    }
}

void VulkanTerrainStream::drawShadows(VkCommandBuffer cmd, VkPipelineLayout layout,
                                      VulkanShadows &shadows) {
    vkCmdBindIndexBuffer(cmd, g_.patchIndices_.handle, 0, VK_INDEX_TYPE_UINT32);
    shadows.bind(cmd, layout);
    for (size_t i = 0; i < shadowDraws_.size();) {
        auto &draw = shadowDraws_[i];
        size_t end = i + 1;
        while (end < shadowDraws_.size() && shadowDraws_[end].set == draw.set &&
               shadowDraws_[end].caster == draw.caster + end - i)
            ++end;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &draw.set, 0,
                                nullptr);
        shadows.drawRange(cmd, draw.caster, uint32_t(end - i));
        i = end;
    }
}
void VulkanTerrainStream::submitted(uint32_t frame) {
    frames_[frame].serial = ++serial_;
    history_.submitted();
}
std::vector<glm::vec2> VulkanTerrainStream::readHeights(ChunkCoord coord, uint32_t tileLevel,
                                                        uint32_t geometryLevel) {
    const int i = stream_->findTile({coord.x, coord.z, tileLevel});
    if (i < 0 || stream_->slot(uint32_t(i)).state != TerrainStreamer::State::Resident)
        throw std::invalid_argument("Chunk is not resident");
    if (geometryLevel > (config_.hierarchical ? layout_.levels : 0))
        throw std::invalid_argument("Height level outside resident representation");
    const size_t width = (layout_.grid.cells >> geometryLevel) + 1;
    std::vector<glm::vec2> result(width * width);
    g_.readBuffer(heights_, result.data(), result.size() * sizeof(glm::vec2),
                  heightStride_ * uint32_t(i) + layout_.heightOffsets[geometryLevel]);
    return result;
}
std::vector<glm::vec4> VulkanTerrainStream::readNormals(ChunkCoord coord, uint32_t level,
                                                        uint32_t tileLevel) {
    const int slot = stream_->findTile({coord.x, coord.z, tileLevel});
    if (slot < 0 || stream_->slot(uint32_t(slot)).state != TerrainStreamer::State::Resident ||
        level >= layout_.levels)
        throw std::invalid_argument("Normal product is not resident");
    const bool coarse = level + 1 == layout_.levels;
    const int detail = stream_->slot(uint32_t(slot)).detail;
    if (!coarse && detail < 0)
        throw std::invalid_argument("Fine normals are not resident");
    check(vkDeviceWaitIdle(g_.context_.device), "Wait for streaming diagnostic");
    const auto width = (layout_.grid.cells * layout_.subdivisions >> level) + 1;
    const auto count = size_t(width) * width * 2;
    Buffer read;
    try {
        g_.createBuffer(read, count * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        auto cmd = g_.beginImmediate();
        memoryBarrier(cmd);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(coarse ? slot : detail) * 2, 2};
        copy.imageOffset = {int32_t(layout_.halos[level]), int32_t(layout_.halos[level]), 0};
        copy.imageExtent = {width, width, 1};
        vkCmdCopyImageToBuffer(cmd, coarse ? coarse_.image : fine_[level].image, VK_IMAGE_LAYOUT_GENERAL,
                               read.handle, 1, &copy);
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
        g_.endImmediate(cmd);
        std::vector<glm::vec4> out(count);
        const auto *packed = static_cast<const uint32_t *>(read.mapped);
        for (size_t i = 0; i < count; ++i)
            out[i] =
                glm::vec4(glm::unpackHalf2x16(packed[i * 2]), glm::unpackHalf2x16(packed[i * 2 + 1]));
        g_.destroyBuffer(read);
        return out;
    } catch (...) {
        g_.destroyBuffer(read);
        throw;
    }
}
std::vector<TerrainTileSnapshot> VulkanTerrainStream::residency() const {
    std::vector<TerrainTileSnapshot> out;
    for (uint32_t i = 0; i < capacity_; ++i) {
        const auto &s = stream_->slot(i);
        if (s.state == TerrainStreamer::State::Empty || s.state == TerrainStreamer::State::Retiring)
            continue;
        out.push_back({s.tile(),
                       {i, s.generation},
                       s.state == TerrainStreamer::State::Resident,
                       s.active,
                       s.drawMask,
                       s.heightFade,
                       meta_[i].revision,
                       s.detailWanted,
                       s.detail >= 0});
    }
    return out;
}
std::vector<glm::vec4> VulkanTerrainStream::readVertexPositions() {
    if (!vertexProbePipeline_)
        throw std::invalid_argument("Vertex diagnostics disabled");
    check(vkDeviceWaitIdle(g_.context_.device), "Wait for vertex diagnostic");
    const auto vertices = (layout_.patchCells + 1) * (layout_.patchCells + 1);
    std::vector<glm::vec4> result(diagnosticDraws_.size() * vertices);
    if (result.empty())
        return result;
    Buffer output;
    try {
        g_.createBuffer(output, result.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        true);
        for (const auto &draw : diagnosticDraws_) {
            VkDescriptorBufferInfo buffer{output.handle, 0, output.size};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = draw.set;
            write.dstBinding = 8;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(g_.context_.device, 1, &write, 0, nullptr);
        }
        const auto cmd = g_.beginImmediate();
        memoryBarrier(cmd);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vertexProbePipeline_);
        uint32_t offset = 0;
        for (auto draw : diagnosticDraws_) {
            float f = float(offset);
            std::memcpy(&draw.constants[7].w, &f, 4);
            draw.constants[1].w &= ~2u; // Linear patch vertices, independent of wireframe index pulling.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, graphicsLayout_, 0, 1,
                                    &draw.set, 0, nullptr);
            vkCmdPushConstants(cmd, graphicsLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                   VK_SHADER_STAGE_COMPUTE_BIT,
                               0, 128, draw.constants.data());
            vkCmdDispatch(cmd, (vertices + 63) / 64, 1, 1);
            offset += vertices;
        }
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        g_.endImmediate(cmd);
        std::memcpy(result.data(), output.mapped, size_t(output.size));
        // Binding eight is unused by rendering; restore a live buffer before
        // destruction.
        for (const auto &draw : diagnosticDraws_) {
            VkDescriptorBufferInfo buffer{heights_.handle, 0, layout_.heightsBytes};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = draw.set;
            write.dstBinding = 8;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffer;
            vkUpdateDescriptorSets(g_.context_.device, 1, &write, 0, nullptr);
        }
        g_.destroyBuffer(output);
        return result;
    } catch (...) {
        g_.destroyBuffer(output);
        throw;
    }
}
void VulkanTerrainStream::cleanup() {
    const auto d = g_.context_.device;
    if (queryPool_)
        vkDestroyQueryPool(d, queryPool_, nullptr);
    if (vertexProbePipeline_)
        vkDestroyPipeline(d, vertexProbePipeline_, nullptr);
    if (qualityBoundsPipeline_)
        vkDestroyPipeline(g_.context_.device, qualityBoundsPipeline_, nullptr);
    if (tileHeightPipeline_)
        vkDestroyPipeline(d, tileHeightPipeline_, nullptr);
    if (tileNormalPipeline_)
        vkDestroyPipeline(d, tileNormalPipeline_, nullptr);
    if (packPipeline_)
        vkDestroyPipeline(d, packPipeline_, nullptr);
    if (graphicsLayout_)
        vkDestroyPipelineLayout(d, graphicsLayout_, nullptr);
    if (descriptorPool_)
        vkDestroyDescriptorPool(d, descriptorPool_, nullptr);
    if (descriptorLayout_)
        vkDestroyDescriptorSetLayout(d, descriptorLayout_, nullptr);
    const auto destroy = [&](Image &i) {
        if (i.view)
            vkDestroyImageView(d, i.view, nullptr);
        if (i.image)
            vkDestroyImage(d, i.image, nullptr);
        if (i.memory)
            vkFreeMemory(d, i.memory, nullptr);
        i = {};
    };
    destroy(coarse_);
    for (auto &i : fine_)
        destroy(i);
    for (auto &b : uploads_)
        g_.destroyBuffer(b);
    for (auto &b : readbacks_)
        g_.destroyBuffer(b);
    for (auto &b : views_)
        g_.destroyBuffer(b);
    for (auto &b : weights_)
        g_.destroyBuffer(b);
    g_.destroyBuffer(heights_);
}
} // namespace ow

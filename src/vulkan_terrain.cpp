#include "vulkan_terrain.hpp"
#include <cstring>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <stdexcept>

namespace ow {
namespace {
void check(VkResult r, const char *operation) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (VkResult " + std::to_string(r) + ")");
}
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}
void beforeOverwrite(VkCommandBuffer cmd) {
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}
} // namespace
VulkanTerrain::VulkanTerrain(TerrainDevice context, TerrainGrid grid, uint32_t capacity,
                             uint32_t normalSubdivisions, uint32_t chunkCapacity,
                             const std::filesystem::path &shaders, TerrainLodConfig config,
                             VkDescriptorSetLayout lightingLayout, VkDescriptorSetLayout materialLayout,
                             VkDescriptorSetLayout visibilityLayout)
    : context_(context), visibilityLayout_(visibilityLayout), materialLayout_(materialLayout),
      lightingLayout_(lightingLayout), grid_(grid), capacity_(capacity),
      normalSubdivisions_(normalSubdivisions), chunkCapacity_(chunkCapacity), updates_(grid, chunkCapacity) {
    if (normalSubdivisions_ < 1 || normalSubdivisions_ > 2)
        throw std::invalid_argument("Normal subdivisions must be 1 or 2");
    validateGrid({capacity_, grid.spacing});
    if (grid.cells > capacity_)
        throw std::invalid_argument("Terrain grid exceeds buffer capacity");
    if (config.enabled) {
        lod_ = std::make_unique<TerrainLod>(grid, capacity, config);
        patchTopology_ = terrainPatchTopology(lod_->patchCells());
        for (auto n = capacity / lod_->patchCells(); n > 1; n >>= 1)
            ++normalLevels_;
    }
    try {
        initialize(shaders);
    } catch (...) {
        cleanup();
        throw;
    }
}
void VulkanTerrain::setGrid(TerrainGrid grid) {
    if (grid.cells > capacity_)
        throw std::invalid_argument("Terrain grid exceeds buffer capacity");
    if (lod_)
        validateLodGrid(grid, lod_->patchCells());
    updates_.setGrid(grid);
    grid_ = grid;
}
VulkanTerrain::~VulkanTerrain() { cleanup(); }
void VulkanTerrain::createBuffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    b.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    check(vkCreateBuffer(context_.device, &info, nullptr, &b.handle), "Create terrain buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(context_.device, b.handle, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(context_.physical, &props);
    const VkMemoryPropertyFlags flags =
        host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    for (; type < props.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) && (props.memoryTypes[type].propertyFlags & flags) == flags)
            break;
    if (type == props.memoryTypeCount)
        throw std::runtime_error("No suitable terrain memory type");
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    check(vkAllocateMemory(context_.device, &alloc, nullptr, &b.memory), "Allocate terrain memory");
    b.allocationSize = req.size;
    allocatedBytes_ += req.size;
    check(vkBindBufferMemory(context_.device, b.handle, b.memory, 0), "Bind terrain buffer");
    if (host)
        check(vkMapMemory(context_.device, b.memory, 0, size, 0, &b.mapped), "Map terrain buffer");
}
void VulkanTerrain::destroyBuffer(Buffer &b) {
    if (b.mapped)
        vkUnmapMemory(context_.device, b.memory);
    if (b.handle)
        vkDestroyBuffer(context_.device, b.handle, nullptr);
    if (b.memory)
        vkFreeMemory(context_.device, b.memory, nullptr);
    allocatedBytes_ -= b.allocationSize;
    b = {};
}
VkCommandBuffer VulkanTerrain::beginImmediate() {
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = context_.commandPool;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    check(vkAllocateCommandBuffers(context_.device, &a, &cmd), "Allocate terrain commands");
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const auto result = vkBeginCommandBuffer(cmd, &b);
    if (result != VK_SUCCESS) {
        vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &cmd);
        check(result, "Begin terrain commands");
    }
    return cmd;
}
void VulkanTerrain::endImmediate(VkCommandBuffer cmd) {
    try {
        check(vkEndCommandBuffer(cmd), "End terrain commands");
        VkCommandBufferSubmitInfo c{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        c.commandBuffer = cmd;
        VkSubmitInfo2 s{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        s.commandBufferInfoCount = 1;
        s.pCommandBufferInfos = &c;
        check(vkQueueSubmit2(context_.queue, 1, &s, VK_NULL_HANDLE), "Submit terrain commands");
        check(vkQueueWaitIdle(context_.queue), "Wait for terrain diagnostics/upload");
    } catch (...) {
        vkDeviceWaitIdle(context_.device);
        vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &cmd);
        throw;
    }
    vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &cmd);
}
VkPipeline VulkanTerrain::pipeline(const std::filesystem::path &path, VkPipelineLayout layout) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open shader: " + path.string());
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % 4)
        throw std::runtime_error("Invalid terrain SPIR-V size");
    std::vector<uint32_t> words(size_t(bytes) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(words.data()), bytes))
        throw std::runtime_error("Cannot read terrain shader");
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size_t(bytes);
    mi.pCode = words.data();
    VkShaderModule module{};
    check(vkCreateShaderModule(context_.device, &mi, nullptr, &module), "Create terrain shader");
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.layout = layout ? layout : computeLayout_;
    pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = module;
    pi.stage.pName = "main";
    VkPipeline p{};
    const auto r = vkCreateComputePipelines(context_.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
    vkDestroyShaderModule(context_.device, module, nullptr);
    if (r != VK_SUCCESS && p)
        vkDestroyPipeline(context_.device, p, nullptr);
    check(r, "Create terrain compute pipeline");
    return p;
}
void VulkanTerrain::createNormalImage(uint32_t level) {
    constexpr VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(context_.physical, format, &formatProperties);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((formatProperties.optimalTilingFeatures & required) != required)
        throw std::runtime_error("Terrain normals require filterable RGBA16F storage images");
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    const auto width = (capacity_ * normalSubdivisions_ >> level) + 1 + 2 * normalHalo(level);
    image.extent = {width, width, 1};
    image.mipLevels = 1;
    image.arrayLayers = chunkCapacity_ * 2; // base, eroded
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(context_.device, &image, nullptr, &normalImages_[level]),
          "Create terrain normal image");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(context_.device, normalImages_[level], &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(context_.physical, &properties);
    uint32_t type = 0;
    for (; type < properties.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            break;
    if (type == properties.memoryTypeCount)
        throw std::runtime_error("No terrain image memory type");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    check(vkAllocateMemory(context_.device, &allocation, nullptr, &normalMemories_[level]),
          "Allocate terrain normal image");
    allocatedBytes_ += requirements.size;
    check(vkBindImageMemory(context_.device, normalImages_[level], normalMemories_[level], 0),
          "Bind terrain normal image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = normalImages_[level];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = format;
    for (uint32_t slot = 0; slot < chunkCapacity_; ++slot) {
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot * 2, 2};
        check(vkCreateImageView(context_.device, &view, nullptr, &slots_[slot].normalViews[level]),
              "Create terrain normal view");
    }
    if (level)
        return;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(context_.device, &sampler, nullptr, &normalSampler_),
          "Create terrain normal sampler");
}
void VulkanTerrain::transitionNormals(VkCommandBuffer cmd, uint32_t slot, uint32_t level, bool writing) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask =
        writing ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = writing ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = writing ? (slots_[slot].normalImageInitialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_UNDEFINED)
                          : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = writing ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = normalImages_[level];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot * 2, 2};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
void VulkanTerrain::initialize(const std::filesystem::path &shaders) {
    const size_t halo = capacity_ * normalSubdivisions_ + 3 + 2 * normalHalo(0), vertices = capacity_ + 1;
    createBuffer(heights_, std::max(halo * halo, vertices * vertices * 2) * sizeof(glm::vec2),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(context_.physical, &properties);
    const auto align = [](VkDeviceSize value, VkDeviceSize alignment) {
        return (value + alignment - 1) / alignment * alignment;
    };
    sampleBytes_ = vertices * vertices * sizeof(TerrainSample);
    sampleStride_ = align(sampleBytes_, properties.limits.minStorageBufferOffsetAlignment);
    parameterStride_ = align(sizeof(TerrainParameters), properties.limits.minUniformBufferOffsetAlignment);
    if (sampleBytes_ > properties.limits.maxStorageBufferRange ||
        heights_.size > properties.limits.maxStorageBufferRange ||
        capacity_ * normalSubdivisions_ + 1 > properties.limits.maxImageDimension2D ||
        chunkCapacity_ * 2 > properties.limits.maxImageArrayLayers)
        throw std::runtime_error("Terrain capacity exceeds device limits");
    createBuffer(samples_, sampleStride_ * chunkCapacity_,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    createBuffer(indices_,
                 lod_ ? sizeof(uint32_t) : VkDeviceSize(capacity_) * capacity_ * 6 * sizeof(uint32_t),
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (uint32_t level = 0; level < normalLevels_; ++level)
        createNormalImage(level);
    if (lod_) {
        createBuffer(patchIndices_, patchTopology_.indices.size() * sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Buffer upload;
        try {
            createBuffer(upload, patchIndices_.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            std::memcpy(upload.mapped, patchTopology_.indices.data(), size_t(upload.size));
            auto cmd = beginImmediate();
            VkBufferCopy copy{0, 0, upload.size};
            vkCmdCopyBuffer(cmd, upload.handle, patchIndices_.handle, 1, &copy);
            barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
            endImmediate(cmd);
        } catch (...) {
            destroyBuffer(upload);
            throw;
        }
        destroyBuffer(upload);
    }
    const auto leaves = lod_ ? capacity_ / lod_->patchCells() : 1;
    boundsStride_ =
        align(VkDeviceSize(leaves) * leaves * 32, properties.limits.minStorageBufferOffsetAlignment);
    createBuffer(bounds_, boundsStride_ * chunkCapacity_,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkDescriptorSetLayoutBinding bindings[12]{};
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      VK_DESCRIPTOR_TYPE_SAMPLER,        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkShaderStageFlags stages[] = {VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT,
                                         VK_SHADER_STAGE_COMPUTE_BIT,
                                         VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT,
                                         VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT,
                                         VK_SHADER_STAGE_COMPUTE_BIT,
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                         VK_SHADER_STAGE_COMPUTE_BIT,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         VK_SHADER_STAGE_VERTEX_BIT,
                                         VK_SHADER_STAGE_COMPUTE_BIT};
    for (uint32_t i = 0; i < 12; ++i)
        bindings[i] = {i, types[i], 1, stages[i], nullptr};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 12;
    layout.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(context_.device, &layout, nullptr, &descriptorLayout_),
          "Create terrain descriptor layout");
    const uint32_t sets = 2 * chunkCapacity_ * normalLevels_;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * sets},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * sets},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * sets},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * sets},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, sets}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = sets;
    pool.poolSizeCount = 5;
    pool.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(context_.device, &pool, nullptr, &descriptorPool_),
          "Create terrain descriptor pool");
    for (uint32_t frame = 0; frame < 2; ++frame) {
        createBuffer(parameters_[frame], parameterStride_ * chunkCapacity_,
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        createBuffer(views_[frame], 80, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        createBuffer(boundsReadbacks_[frame], bounds_.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        for (uint32_t slot = 0; slot < chunkCapacity_; ++slot)
            for (uint32_t level = 0; level < normalLevels_; ++level) {
                auto &set = slots_[slot].descriptors[frame][level];
                VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                a.descriptorPool = descriptorPool_;
                a.descriptorSetCount = 1;
                a.pSetLayouts = &descriptorLayout_;
                check(vkAllocateDescriptorSets(context_.device, &a, &set), "Allocate terrain descriptors");
                const auto &topology = lod_ ? patchIndices_ : indices_;
                const VkDescriptorBufferInfo buffers[] = {
                    {parameters_[frame].handle, parameterStride_ * slot, sizeof(TerrainParameters)},
                    {heights_.handle, 0, heights_.size},
                    {samples_.handle, sampleStride_ * slot, sampleBytes_},
                    {indices_.handle, 0, indices_.size},
                    {views_[frame].handle, 0, 80},
                    {topology.handle, 0, topology.size},
                    {bounds_.handle, boundsStride_ * slot, boundsStride_}};
                const VkDescriptorImageInfo images[] = {
                    {VK_NULL_HANDLE, slots_[slot].normalViews[level], VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, slots_[slot].normalViews[level],
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {normalSampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                    {VK_NULL_HANDLE, slots_[slot].normalViews[std::min(level + 1, normalLevels_ - 1)],
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                    {VK_NULL_HANDLE, slots_[slot].normalViews[level ? level - 1 : 0],
                     VK_IMAGE_LAYOUT_GENERAL}};
                VkWriteDescriptorSet writes[12]{};
                for (uint32_t i = 0; i < 12; ++i) {
                    auto &w = writes[i];
                    w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w.dstSet = set;
                    w.dstBinding = i;
                    w.descriptorCount = 1;
                    w.descriptorType = types[i];
                    if (i < 4)
                        w.pBufferInfo = &buffers[i];
                    else if (i < 9)
                        w.pImageInfo = &images[i - 4];
                    else
                        w.pBufferInfo = &buffers[i - 5];
                }
                vkUpdateDescriptorSets(context_.device, 12, writes, 0, nullptr);
            }
    }
    VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    l.setLayoutCount = 1;
    l.pSetLayouts = &descriptorLayout_;
    VkPushConstantRange computePush{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    l.pushConstantRangeCount = 1;
    l.pPushConstantRanges = &computePush;
    check(vkCreatePipelineLayout(context_.device, &l, nullptr, &computeLayout_),
          "Create terrain compute layout");
    const VkDescriptorSetLayout graphicsSets[] = {descriptorLayout_, lightingLayout_, materialLayout_,
                                                  visibilityLayout_};
    l.setLayoutCount = visibilityLayout_ ? 4 : materialLayout_ ? 3 : lightingLayout_ ? 2 : 1;
    l.pSetLayouts = graphicsSets;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 96};
    l.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(context_.device, &l, nullptr, &graphicsLayout_),
          "Create terrain draw layout");
    geographyShader_ = shaders / "terrain_geography.comp.spv";
    heightPipeline_ = pipeline(shaders / "terrain_height.comp.spv");
    normalPipeline_ = pipeline(shaders / "terrain_normals.comp.spv");
    filterPipeline_ = pipeline(shaders / "terrain_normal_filter.comp.spv");
    boundsPipeline_ = pipeline(shaders / "terrain_bounds.comp.spv");
}

void VulkanTerrain::recordNormals(VkCommandBuffer cmd, uint32_t frame, uint32_t slot) {
    for (uint32_t level = 0; level < normalLevels_; ++level) {
        transitionNormals(cmd, slot, level, true);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, level ? filterPipeline_ : normalPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1,
                                &slots_[slot].descriptors[frame][level], 0, nullptr);
        const uint32_t width = (grid_.cells * normalSubdivisions_ >> level) + 1 + 2 * normalHalo(level);
        const glm::uvec4 filter(width, normalHalo(level), normalHalo(level ? level - 1 : 0), level);
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(filter), &filter);
        const glm::uvec4 tile(0);
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 16, sizeof(tile), &tile);
        vkCmdDispatch(cmd, (width + 7) / 8, (width + 7) / 8, 1);
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    for (uint32_t level = 0; level < normalLevels_; ++level)
        transitionNormals(cmd, slot, level, false);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
}
uint32_t VulkanTerrain::record(VkCommandBuffer cmd, uint32_t frame) {
    uint32_t generated = 0;
    bool rebuildIndices = grid_.cells != indexCells_;
    for (uint32_t slot = 0; slot < chunkCount(); ++slot) {
        auto &p = slots_[slot].parameters;
        const bool generate = !slots_[slot].valid || updates_.pending(slot);
        if (generate)
            p = terrainParameters(grid_, {coordinate(slot), updates_.requested().settings});
        p.dimensions.z = !lod_ && generate && rebuildIndices;
        p.dimensions.w = normalSubdivisions_;
        p.sampling = {normalHalo(0), lod_ ? lod_->patchCells() : 0, normalLevels_, 0};
        std::memcpy(static_cast<char *>(parameters_[frame].mapped) + parameterStride_ * slot, &p, sizeof(p));
        if (generate) {
            // Also orders the previous slot's scratch reads before this slot
            // overwrites it.
            beforeOverwrite(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, heightPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1,
                                    &slots_[slot].descriptors[frame][0], 0, nullptr);
            const glm::uvec4 tile(0);
            vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 16, sizeof(tile), &tile);
            const auto groups = (grid_.cells * normalSubdivisions_ + 3 + 2 * normalHalo(0) + 7) / 8;
            vkCmdDispatch(cmd, groups, groups, 1);
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            recordNormals(cmd, frame, slot);
            slots_[slot].boundsValid = false;
            rebuildIndices = false;
            generated |= 1u << slot;
        }
        const uint64_t revision = slots_[slot].revision + (generate ? 1 : 0);
        bool boundsPending = false;
        for (const auto &frameTickets : tickets_) {
            const auto &ticket = frameTickets[slot];
            boundsPending |=
                ticket.pending && ticket.revision == revision && ticket.grid == grid_ &&
                ticket.request == TerrainRequest{coordinate(slot), updates_.requested().settings};
        }
        // Reactivated slots may retain valid samples but need bounds for a rebuilt
        // CPU hierarchy. Refresh only metadata; don't regenerate the terrain.
        if (lod_ && !slots_[slot].boundsValid && !boundsPending) {
            beforeOverwrite(cmd);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, boundsPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1,
                                    &slots_[slot].descriptors[frame][0], 0, nullptr);
            const glm::uvec4 tile(0);
            vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 16, sizeof(tile), &tile);
            const auto side = grid_.cells / lod_->patchCells();
            vkCmdDispatch(cmd, side, side, 1);
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferCopy copy{boundsStride_ * slot, boundsStride_ * slot,
                              VkDeviceSize(side) * side * sizeof(glm::vec2)};
            vkCmdCopyBuffer(cmd, bounds_.handle, boundsReadbacks_[frame].handle, 1, &copy);
            barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            tickets_[frame][slot] = {
                true, revision, grid_, {coordinate(slot), updates_.requested().settings}};
        }
    }
    return generated;
}
void VulkanTerrain::submitted(uint32_t generated) {
    updates_.submitted(generated);
    for (uint32_t slot = 0; slot < chunkCount(); ++slot)
        if (generated & (1u << slot)) {
            ++slots_[slot].revision;
            slots_[slot].valid = true;
            slots_[slot].normalImageInitialized = true;
        }
    if (generated)
        indexCells_ = grid_.cells;
}
void VulkanTerrain::bind(VkCommandBuffer cmd, uint32_t frame, uint32_t slot, uint32_t level) const {
    vkCmdBindIndexBuffer(cmd, lod_ ? patchIndices_.handle : indices_.handle, 0, VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsLayout_, 0, 1,
                            &slots_[slot].descriptors[frame][level], 0, nullptr);
}
void VulkanTerrain::setView(uint32_t frame, const glm::mat4 &matrix, glm::vec3 observer) {
    struct View {
        glm::mat4 matrix;
        glm::vec4 observer;
    };
    const View view{matrix, glm::vec4(observer, 0)};
    static_assert(sizeof(View) == 80);
    std::memcpy(views_[frame].mapped, &view, sizeof(view));
}
void VulkanTerrain::consumeBounds(uint32_t frame) {
    if (!lod_)
        return;
    // Coalesce grid edits before rebuilding CPU metadata. Matching readback
    // tickets below are interpreted using this current hierarchy.
    if (!(lod_->grid() == grid_)) {
        lod_->setGrid(grid_);
        for (auto &slot : slots_)
            slot.boundsValid = false;
    }
    for (uint32_t slot = 0; slot < chunkCapacity_; ++slot) {
        auto &t = tickets_[frame][slot];
        if (!t.pending)
            continue;
        t.pending = false;
        if (slot >= chunkCount() || t.revision != slots_[slot].revision || !(t.grid == grid_) ||
            !(t.request == TerrainRequest{coordinate(slot), updates_.requested().settings}) ||
            updates_.pending(slot))
            continue;
        const auto *b = reinterpret_cast<const glm::vec2 *>(
            static_cast<const char *>(boundsReadbacks_[frame].mapped) + boundsStride_ * slot);
        lod_->setBounds(slot, b);
        slots_[slot].boundsValid = true;
    }
}
bool VulkanTerrain::boundsReady() const {
    for (uint32_t slot = 0; slot < chunkCount(); ++slot)
        if (!slots_[slot].boundsValid || updates_.pending(slot))
            return false;
    return true;
}
void VulkanTerrain::requireReadable(uint32_t slot) const {
    if (slot >= chunkCount())
        throw std::out_of_range("Inactive terrain slot");
    if (!slots_[slot].valid || updates_.pending(slot))
        throw std::runtime_error("Terrain readback requires the current request to be submitted");
}
std::vector<TerrainSample> VulkanTerrain::readback(uint32_t slot) {
    requireReadable(slot);
    std::vector<TerrainSample> out(size_t(grid_.cells + 1) * (grid_.cells + 1));
    readBuffer(samples_, out.data(), out.size() * sizeof(TerrainSample), sampleStride_ * slot);
    return out;
}
std::vector<uint32_t> VulkanTerrain::readIndices() {
    requireReadable(0);
    if (lod_)
        return patchTopology_.indices;
    std::vector<uint32_t> out(indexCount());
    readBuffer(indices_, out.data(), out.size() * sizeof(uint32_t));
    return out;
}
void VulkanTerrain::readBuffer(const Buffer &source, void *out, VkDeviceSize bytes, VkDeviceSize offset) {
    check(vkDeviceWaitIdle(context_.device), "Wait before terrain readback");
    Buffer read;
    try {
        createBuffer(read, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        auto cmd = beginImmediate();
        barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy copy{offset, 0, bytes};
        vkCmdCopyBuffer(cmd, source.handle, read.handle, 1, &copy);
        barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        endImmediate(cmd);
        std::memcpy(out, read.mapped, size_t(bytes));
    } catch (...) {
        destroyBuffer(read);
        throw;
    }
    destroyBuffer(read);
}
std::vector<glm::vec4> VulkanTerrain::readNormals(uint32_t slot, uint32_t level) {
    requireReadable(slot);
    if (level >= normalLevels_)
        throw std::out_of_range("Normal level");
    check(vkDeviceWaitIdle(context_.device), "Wait before normal readback");
    const uint32_t width = (grid_.cells * normalSubdivisions_ >> level) + 1;
    const size_t count = size_t(width) * width * 2;
    Buffer read;
    try {
        createBuffer(read, count * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        auto cmd = beginImmediate();
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = normalImages_[level];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot * 2, 2};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dependency);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, slot * 2, 2};
        copy.imageExtent = {width, width, 1};
        copy.imageOffset = {int32_t(normalHalo(level)), int32_t(normalHalo(level)), 0};
        vkCmdCopyImageToBuffer(cmd, normalImages_[level], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read.handle,
                               1, &copy);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);
        barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        endImmediate(cmd);
        std::vector<glm::vec4> result(count);
        const auto *packed = static_cast<const uint32_t *>(read.mapped);
        for (size_t i = 0; i < count; ++i)
            result[i] = glm::vec4(glm::unpackHalf2x16(packed[i * 2]), glm::unpackHalf2x16(packed[i * 2 + 1]));
        destroyBuffer(read);
        return result;
    } catch (...) {
        destroyBuffer(read);
        throw;
    }
}
std::vector<TerrainGeographySample>
VulkanTerrain::sampleTerrainGeography(TerrainGrid grid, const TerrainRequest &request, bool canonical) {
    if (lod_ || chunkCapacity_ != 1 || grid.cells > capacity_)
        throw std::invalid_argument("Geography probe requires one non-LOD slot and a grid within capacity");
    auto p = terrainParameters(grid, request);
    if (canonical && std::round(grid.spacing * 4) != grid.spacing * 4)
        throw std::invalid_argument("Canonical probe spacing must be a multiple of 0.25 m");
    p.sampling.w = canonical ? 1u : 0u;
    check(vkDeviceWaitIdle(context_.device), "Wait before geography probe");
    if (!geographyPipeline_)
        geographyPipeline_ = pipeline(geographyShader_);
    // Invalidate before overwriting: the next ordinary render restores its own
    // requested parameters and data even if readback fails.
    slots_[0].valid = false;
    std::memcpy(parameters_[0].mapped, &p, sizeof(p));
    const auto cmd = beginImmediate();
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, geographyPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1,
                            &slots_[0].descriptors[0][0], 0, nullptr);
    const glm::uvec4 push[2] = {};
    vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, (grid.cells + 8) / 8, (grid.cells + 8) / 8, 1);
    endImmediate(cmd);
    std::vector<TerrainGeographySample> out(size_t(grid.cells + 1) * (grid.cells + 1));
    static_assert(sizeof(TerrainGeographySample) == sizeof(TerrainSample));
    readBuffer(samples_, out.data(), out.size() * sizeof(TerrainGeographySample));
    return out;
}
std::vector<glm::vec2> VulkanTerrain::sampleTerrainHeightLevel(TerrainGrid grid,
                                                               const TerrainRequest &request, uint32_t level,
                                                               bool fixture) {
    if (lod_ || chunkCapacity_ != 1 || grid.cells > capacity_ || level >= maxTerrainLodLevels ||
        (grid.cells >> level) < 1 || std::round(grid.spacing * 4) != grid.spacing * 4)
        throw std::invalid_argument("Filtered height probe grid or level invalid");
    auto p = terrainParameters(grid, request);
    p.sampling.w = 1;
    const auto width = (grid.cells >> level) + 1;
    uint64_t offset = 0;
    for (uint32_t l = 0; l < level; ++l) {
        auto w = (grid.cells >> l) + 1;
        offset += uint64_t(w) * w * 8;
    }
    if (offset + uint64_t(width) * width * 8 > heights_.size)
        throw std::invalid_argument("Height probe capacity");
    check(vkDeviceWaitIdle(context_.device), "Wait before filtered height probe");
    auto &probe = heightLevelPipelines_[fixture ? 1 : 0];
    if (!probe)
        probe = pipeline(geographyShader_.parent_path() /
                         (fixture ? "terrain_height_fixture.comp.spv" : "terrain_tile_height.comp.spv"));
    slots_[0].valid = false;
    std::memcpy(parameters_[0].mapped, &p, sizeof(p));
    auto cmd = beginImmediate();
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1,
                            &slots_[0].descriptors[0][0], 0, nullptr);
    const glm::uvec4 push[2] = {{width, 0, 0, level}, {0, 0, 0, 0}};
    vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, (width + 7) / 8, (width + 7) / 8, 1);
    endImmediate(cmd);
    std::vector<glm::vec2> result(size_t(width) * width);
    readBuffer(heights_, result.data(), result.size() * 8, offset);
    return result;
}
std::vector<TerrainSample> VulkanTerrain::sampleTerrainNormalFixture(TerrainGrid grid, uint32_t level,
                                                                     uint32_t tileLevel) {
    if (lod_ || chunkCapacity_ != 1 || grid.cells > capacity_ || level >= maxTerrainLodLevels ||
        tileLevel >= maxTerrainLodLevels || (grid.cells >> level) < 1)
        throw std::invalid_argument("Normal fixture grid or level invalid");
    auto p = terrainParameters(grid, {});
    p.dimensions.w = normalSubdivisions_;
    p.sampling.w = tileLevel << 8;
    const uint32_t width = (grid.cells * normalSubdivisions_ >> level) + 1;
    const uint64_t bytes = uint64_t(width) * width * sizeof(TerrainSample);
    if (bytes > sampleBytes_)
        throw std::invalid_argument("Normal fixture capacity");
    check(vkDeviceWaitIdle(context_.device), "Wait before normal fixture");
    if (!normalFixturePipeline_)
        normalFixturePipeline_ = pipeline(geographyShader_.parent_path() / "terrain_normal_fixture.comp.spv");
    slots_[0].valid = false;
    std::memcpy(parameters_[0].mapped, &p, sizeof(p));
    auto cmd = beginImmediate();
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, normalFixturePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1,
                            &slots_[0].descriptors[0][0], 0, nullptr);
    const glm::uvec4 push[2] = {{width, 0, 0, level}, {0, 0, 0, 0}};
    vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, (width + 7) / 8, (width + 7) / 8, 1);
    endImmediate(cmd);
    std::vector<TerrainSample> out(size_t(width) * width);
    readBuffer(samples_, out.data(), bytes);
    return out;
}
std::vector<TerrainSample> VulkanTerrain::normalsForFixture(const std::vector<glm::vec2> &h) {
    if (lod_)
        throw std::invalid_argument("Normal fixtures use the diagnostic non-LOD configuration");
    if (h.size() != size_t(grid_.cells * normalSubdivisions_ + 3) * (grid_.cells * normalSubdivisions_ + 3))
        throw std::invalid_argument("Fixture must include terrain halo");
    check(vkDeviceWaitIdle(context_.device), "Wait before normal fixture");
    auto p = terrainParameters(grid_, {coordinate(0), updates_.requested().settings});
    p.dimensions.w = normalSubdivisions_;
    std::memcpy(parameters_[0].mapped, &p, sizeof(p));
    Buffer upload;
    try {
        createBuffer(upload, h.size() * sizeof(glm::vec2), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(upload.mapped, h.data(), size_t(upload.size));
        auto cmd = beginImmediate();
        barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        VkBufferCopy copy{0, 0, upload.size};
        vkCmdCopyBuffer(cmd, upload.handle, heights_.handle, 1, &copy);
        barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        recordNormals(cmd, 0, 0);
        endImmediate(cmd);
        slots_[0].normalImageInitialized = true;
        std::vector<TerrainSample> result(size_t(grid_.cells + 1) * (grid_.cells + 1));
        readBuffer(samples_, result.data(), result.size() * sizeof(TerrainSample));
        slots_[0].valid = false;
        destroyBuffer(upload);
        return result;
    } catch (...) {
        slots_[0].valid = false;
        destroyBuffer(upload);
        throw;
    }
}
void VulkanTerrain::cleanup() {
    auto d = context_.device;
    if (normalFixturePipeline_)
        vkDestroyPipeline(d, normalFixturePipeline_, nullptr);
    for (auto p : heightLevelPipelines_)
        if (p)
            vkDestroyPipeline(d, p, nullptr);
    if (geographyPipeline_)
        vkDestroyPipeline(d, geographyPipeline_, nullptr);
    if (heightPipeline_)
        vkDestroyPipeline(d, heightPipeline_, nullptr);
    if (normalPipeline_)
        vkDestroyPipeline(d, normalPipeline_, nullptr);
    if (graphicsLayout_)
        vkDestroyPipelineLayout(d, graphicsLayout_, nullptr);
    if (computeLayout_)
        vkDestroyPipelineLayout(d, computeLayout_, nullptr);
    if (descriptorPool_)
        vkDestroyDescriptorPool(d, descriptorPool_, nullptr);
    if (descriptorLayout_)
        vkDestroyDescriptorSetLayout(d, descriptorLayout_, nullptr);
    if (normalSampler_)
        vkDestroySampler(d, normalSampler_, nullptr);
    for (auto &slot : slots_)
        for (auto view : slot.normalViews)
            if (view)
                vkDestroyImageView(d, view, nullptr);
    for (auto image : normalImages_)
        if (image)
            vkDestroyImage(d, image, nullptr);
    for (auto memory : normalMemories_)
        if (memory)
            vkFreeMemory(d, memory, nullptr);
    if (filterPipeline_)
        vkDestroyPipeline(d, filterPipeline_, nullptr);
    if (boundsPipeline_)
        vkDestroyPipeline(d, boundsPipeline_, nullptr);
    for (auto &b : views_)
        destroyBuffer(b);
    for (auto &b : boundsReadbacks_)
        destroyBuffer(b);
    destroyBuffer(bounds_);
    destroyBuffer(patchIndices_);
    for (auto &p : parameters_)
        destroyBuffer(p);
    destroyBuffer(heights_);
    destroyBuffer(samples_);
    destroyBuffer(indices_);
}
} // namespace ow

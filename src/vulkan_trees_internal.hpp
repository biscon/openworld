#pragma once
#include "tree_pipeline.hpp"
#include "vulkan_terrain_stream.hpp"
#include "vulkan_trees.hpp"
#include <chrono>
#include <cmath>
namespace ow {
using namespace render_detail;
namespace tree_detail {
constexpr uint32_t bins = 96, words = 769, queryCount = 20;
struct Uniform {
    std::array<glm::mat4, 6> views{};
    glm::vec4 settings{}, previousSettings{}, wind{}, previousWind{}, detail{}, sun{}, curvature{};
    glm::uvec4 limits{}, mode{};
    std::array<glm::vec4, 24> capture{}, bounds{};
    std::array<glm::vec4, 6> shadowFootprint{};
    std::array<glm::uvec4, 192> geometry{};
    std::array<glm::uvec4, 6> lists{};
    glm::mat4 raster{1};
    glm::uvec4 pyramid{};
};
static_assert(sizeof(Uniform) == 4640);
struct Cell {
    glm::vec4 origin{}, previous{}, lo{}, hi{}, phase{};
    glm::uvec4 meta{};
};
static_assert(sizeof(Cell) == 96);
struct Job {
    glm::uvec4 key{}, config{};
    glm::vec4 ecology{};
    glm::uvec4 publication{};
    std::array<VegetationTerrainPatch, 256> patches{};
    TerrainParameters geography{};
};
static_assert(sizeof(Job) == 16944);
inline VkPipeline compute(VkDevice d, VkPipelineLayout layout, const std::filesystem::path &p) {
    ShaderModule s(d, p);
    VkComputePipelineCreateInfo c{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    c.layout = layout;
    c.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    c.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    c.stage.module = s.handle;
    c.stage.pName = "main";
    VkPipeline result;
    check(vkCreateComputePipelines(d, {}, 1, &c, nullptr, &result));
    return result;
}
inline void imageDescriptor(VkDevice d, VkDescriptorSet set, uint32_t binding, VkImageView image,
                            VkSampler sampler = {},
                            VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    VkDescriptorImageInfo i{sampler, image, imageLayout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &i;
    vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
}
} // namespace tree_detail
using namespace tree_detail;
struct VulkanTrees::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    VkDescriptorSetLayout setLayout{}, assetLayout{}, updateLayout{};
    VkDescriptorPool pool{};
    VkPipelineLayout layout{}, populateLayout{};
    VkPipeline select{}, populate{}, canopyPipeline{};
    std::array<VkPipeline, 6> pipelines{};
    VkSampler sampler{}, atlasSampler{};
    VkQueryPool queries{};
    TreeOptions options;
    TreeLibrary library;
    TreeResidency residency;
    Buffer population, attachment, geometry, emptyHeight, debugReadback, lodHistory;
    std::array<TreeCellKey, 64> debugKeys{};
    uint32_t debugCount = 0;
    bool eroded = true;
    std::vector<std::unique_ptr<TreeImage>> images;
    std::array<VkDescriptorSet, 24> assetSets{};
    struct Frame {
        Buffer uniform, cells, groups, lists, work, prefixes, choices, indirect, jobs, readback, canopy;
        VkDescriptorSet set{}, updateSet{};
        uint64_t serial = 0;
        uint32_t stamped = 0;
        TreeStats stats;
    };
    std::array<Frame, 2> frames;
    Uniform current{}, previous{};
    uint32_t frame = 0, groupCount = 0, jobCount = 0, repairCursor = 0, nearRepairCursor = 0,
             listCapacity = 0, timestampBits = 0;
    uint64_t serial = 0, completed = 0, bytes = 0, indexOffset = 0, revision = 1;
    double timestampPeriod = 0, cellCost = .03;
    bool fixture = false, history = false, initialized = false;
    WorldPosition camera, previousCamera;
    TreeStats last;
    TreeSettings settings{}, oldSettings{};
    VulkanTerrainStream *terrain = nullptr;
    std::vector<uint64_t> signatures;
    std::vector<uint32_t> generations, previousGenerations;
    std::vector<glm::vec2> heights;
    std::array<uint32_t, 32> updatedSlots{};
    std::array<uint64_t, 32> updatedSignatures{};
    std::array<bool, 32> updatedGenerate{};
    TreeTerrainCell surface;
    Impl(VkPhysicalDevice p, VkDevice d, const TreeOptions &o)
        : physical(p), device(d), options(o), residency(o.streaming) {}
    ~Impl() {
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(device, p, nullptr);
        if (select)
            vkDestroyPipeline(device, select, nullptr);
        if (canopyPipeline)
            vkDestroyPipeline(device, canopyPipeline, nullptr);
        if (populate)
            vkDestroyPipeline(device, populate, nullptr);
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (populateLayout)
            vkDestroyPipelineLayout(device, populateLayout, nullptr);
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (assetLayout)
            vkDestroyDescriptorSetLayout(device, assetLayout, nullptr);
        if (updateLayout)
            vkDestroyDescriptorSetLayout(device, updateLayout, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (atlasSampler)
            vkDestroySampler(device, atlasSampler, nullptr);
    }
    void allocate(Buffer &b, uint64_t n, VkBufferUsageFlags usage, bool host = false) {
        VkBufferCreateInfo c{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        c.size = std::max<uint64_t>(n, 16);
        c.usage = usage;
        VkBuffer temporary;
        check(vkCreateBuffer(device, &c, nullptr, &temporary));
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(device, temporary, &r);
        vkDestroyBuffer(device, temporary, nullptr);
        if (bytes > options.streaming.budgetBytes || r.size > options.streaming.budgetBytes - bytes)
            throw std::runtime_error("Tree GPU budget exhausted");
        b.create(physical, device, c.size, usage, host);
        bytes += b.bytes;
    }
    void stamp(VkCommandBuffer cmd, uint32_t i) {
        if (queries) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, frame * queryCount + i);
            frames[frame].stamped |= 1u << i;
        }
    }
    void selectView(VkCommandBuffer cmd, uint32_t view) {
        if (!settings.enabled || !groupCount)
            return;
        auto &f = frames[frame];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, select);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &f.set, 0, nullptr);
        for (uint32_t phase = 0; phase < 4; ++phase) {
            uint32_t push[] = {view, 0, phase, 0};
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                   VK_SHADER_STAGE_COMPUTE_BIT,
                               0, 16, push);
            vkCmdDispatch(cmd, phase == 1 || phase == 2 ? 1 : groupCount, 1, 1);
            barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    TreeStats decode(Frame &f) {
        auto result = f.stats;
        result.completed = f.serial != 0;
        if (!result.completed)
            return result;
        auto *w = static_cast<const uint32_t *>(f.readback.mapped);
        for (uint32_t v = 0; v < 6; ++v) {
            result.overflow += w[v * words + 768];
            for (uint32_t b = 0; b < library.assets.size() * 4; ++b) {
                auto n = w[v * words + b * 8 + 1];
                if (v == 0) {
                    result.visible += n;
                    result.lodCounts[b % 4] += n;
                    result.triangles +=
                        uint64_t(n) * (current.geometry[b * 2].x + current.geometry[b * 2 + 1].x) / 3;
                } else
                    result.casters += n;
            }
        }
        if (queries) {
            std::array<uint64_t, queryCount * 2> t{};
            vkGetQueryPoolResults(device, queries, frame * queryCount, queryCount, sizeof(t), t.data(), 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            result.timingsAvailable = true;
            uint64_t mask = timestampBits == 64 ? ~0ull : (1ull << timestampBits) - 1;
            for (uint32_t i = 0; i < queryCount; i += 2) {
                if (!(f.stamped & (1u << i)))
                    continue;
                if (!t[i * 2 + 1] || !t[(i + 1) * 2 + 1]) {
                    result.timingsAvailable = false;
                    continue;
                }
                double ms = double((t[(i + 1) * 2] - t[i * 2]) & mask) * timestampPeriod / 1e6;
                if (i == 0)
                    result.updateMilliseconds = ms;
                else if (i == 2 || i == 18)
                    result.selectionMilliseconds += ms;
                else if (i == 4)
                    result.depthMilliseconds = ms;
                else if (i == 6)
                    result.colourMilliseconds = ms;
                else
                    result.shadowMilliseconds += ms;
            }
        }
        return result;
    }
};

} // namespace ow

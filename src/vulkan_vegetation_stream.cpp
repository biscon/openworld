#include "vulkan_vegetation_stream.hpp"
#include <chrono>
#include <cstring>
namespace ow {
using namespace render_detail;
void VulkanVegetationStream::allocate(Buffer &buffer, uint64_t size, VkBufferUsageFlags usage, bool host) {
    buffer.create(physical_, device_, size, usage, host);
    if (buffer.bytes > budget_ - bytes_)
        throw std::runtime_error("Vegetation stream exceeds GPU budget");
    bytes_ += buffer.bytes;
}
VulkanVegetationStream::VulkanVegetationStream(VkPhysicalDevice physical, VkDevice device,
                                               const std::filesystem::path &shaders, VkBuffer population,
                                               uint64_t populationBytes, VegetationStreamConfig config,
                                               const VegetationLibrary &library, uint64_t budget)
    : physical_(physical), device_(device), residency_(config), config_(config), budget_(budget) {
    for (uint32_t i = 0; i < library.assets.size(); ++i) {
        const auto &asset = library.assets[i];
        shadowDistance_ = std::max(shadowDistance_, asset.ranges.w);
        if (asset.category == FoliageCategory::Grass) {
            if (assets_.x == ~0u)
                assets_.x = i;
            else if (assets_.y == ~0u)
                assets_.y = i;
        }
        if (asset.category == FoliageCategory::Plant && assets_.z == ~0u)
            assets_.z = i;
        if (asset.category == FoliageCategory::Shrub && assets_.w == ~0u)
            assets_.w = i;
        envelope_ =
            std::max(envelope_, 2 * (glm::length(glm::max(glm::abs(asset.minimum), glm::abs(asset.maximum))) +
                                     asset.displacement));
    }
    if (assets_.x == ~0u)
        throw std::invalid_argument("Streamed vegetation needs a grass asset");
    if (assets_.y == ~0u)
        assets_.y = assets_.x;
    surfaces_.resize(config.cellCapacity);
    previousSignatures_.resize(config.cellCapacity);
    currentSignatures_.resize(config.cellCapacity);
    previousGenerations_.resize(config.cellCapacity);
    currentGenerations_.resize(config.cellCapacity);
    population_ = population;
    if (config.diagnostics) {
        allocate(diagnostic_, uint64_t(config.cellCapacity) * 128 * 48, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 true);
        diagnosticSurfaces_.resize(config.cellCapacity);
        diagnosticHistory_.resize(config.cellCapacity);
    }
    // RAII cleanup on initialization failure also covers Vulkan handles.
    try {
        VkDescriptorSetLayoutBinding bindings[4]{};
        for (uint32_t i = 0; i < 4; ++i)
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 4;
        li.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout_));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        check(vkCreateDescriptorPool(device, &pi, nullptr, &pool_));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout_;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &layout_));
        ShaderModule shader(device, shaders / "vegetation_populate.comp.spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = layout_;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    nullptr,
                    0,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    shader.handle,
                    "main",
                    nullptr};
        check(vkCreateComputePipelines(device, {}, 1, &ci, nullptr, &pipeline_));
        for (auto &f : frames_) {
            allocate(f.attachments, uint64_t(config.cellCapacity) * 128 * 16,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            allocate(f.jobs, 32 * sizeof(Job), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            f.signatures.resize(config.cellCapacity);
            f.generations.resize(config.cellCapacity);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &setLayout_;
            check(vkAllocateDescriptorSets(device, &ai, &f.set));
            VkDescriptorBufferInfo info{population, 0, populationBytes};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = f.set;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &info;
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
            descriptor(device, f.set, 1, f.attachments);
            descriptor(device, f.set, 2, f.jobs);
        }
    } catch (...) {
        if (pipeline_)
            vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_)
            vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (pool_)
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (setLayout_)
            vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        throw;
    }
}
VulkanVegetationStream::~VulkanVegetationStream() {
    if (pipeline_)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_)
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pool_)
        vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (setLayout_)
        vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
}
void VulkanVegetationStream::observe(double ms, uint32_t updates) {
    if (ms > 0 && updates)
        cellCost_ = std::max(ms / updates, cellCost_ * .75 + ms / updates * .25);
}
void VulkanVegetationStream::prepare(uint32_t frame, WorldPosition camera, WorldPosition previous,
                                     const VegetationSettings &settings, VulkanTerrainStream *terrain,
                                     VegetationGpuCell *cells, VegetationStats &stats) {
    frame_ = frame;
    auto &f = frames_[frame];
    completed_ = std::max(completed_, f.serial);
    updateCount_ = 0;
    uint64_t revision = terrain ? terrain->vegetationRevision() : 0;
    revision = revision * 1099511628211ull ^ settings.habitat.revision;
    revision = revision * 1099511628211ull ^ settings.exclusion.revision;
    // Include values as well as caller revisions, so interactive edits cannot
    // leave stale populations.
    auto mix = [&](const void *ptr, size_t size) {
        auto p = static_cast<const unsigned char *>(ptr);
        for (size_t i = 0; i < size; ++i)
            revision = (revision ^ p[i]) * 1099511628211ull;
    };
    mix(&settings.habitat.weights, sizeof(settings.habitat.weights));
    mix(&settings.habitat.minimumElevation, 3 * sizeof(float));
    mix(&settings.exclusion.centre.x, 8);
    mix(&settings.exclusion.centre.z, 8);
    mix(&settings.exclusion.centre.local, sizeof(glm::dvec3));
    mix(&settings.exclusion.radius, 4);
    residency_.demand(camera, revision, completed_, settings.enabled && terrain,
                      demandRadius(settings.distance));
    seed_ = terrain ? terrain->vegetationSeed() : 0;
    std::fill(currentSignatures_.begin(), currentSignatures_.end(), 0);
    std::fill(currentGenerations_.begin(), currentGenerations_.end(), 0);
    stats.retiringCells = residency_.retiring();
    stats.deferredCells = residency_.deferred();
    if (!terrain || !settings.enabled) {
        // Disabled frames do not consume these records. A missing visible terrain
        // provider still needs empty records so selection cannot reuse old roots.
        if (settings.enabled)
            std::fill_n(cells, config_.cellCapacity, VegetationGpuCell{});
        return;
    }
    std::fill_n(cells, config_.cellCapacity, VegetationGpuCell{});
    if (f.heightBuffer != terrain->vegetationHeights()) {
        VkDescriptorBufferInfo info{terrain->vegetationHeights(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = f.set;
        w.dstBinding = 3;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        f.heightBuffer = terrain->vegetationHeights();
    }
    auto *jobs = jobs_.data();
    const uint32_t allowance =
        std::min(config_.updatesPerFrame, std::max(1u, uint32_t(.5 / std::max(cellCost_, .000001))));
    // Repairs before generation. Cached cells require no GPU work.
    for (uint32_t pass = 0; pass < 2; ++pass)
        for (uint32_t i : residency_.ordered()) {
            auto &slot = residency_.slot(i);
            if (slot.generated != (pass == 0))
                continue;
            auto &cached = surfaces_[i];
            if (cached.version != terrain->vegetationSurfaceVersion() ||
                cached.generation != slot.generation) {
                cached.ready = terrain->vegetationSurface(slot.key, cached.surface, false);
                cached.version = terrain->vegetationSurfaceVersion();
                cached.generation = slot.generation;
            }
            auto &surface = cached.surface;
            if (!cached.ready || (!slot.generated && !surface.canonical.w)) {
                ++stats.missingTerrainCells;
                ++stats.pendingCells;
                continue;
            }
            const bool dirty = f.signatures[i] != surface.signature || f.generations[i] != slot.generation;
            if (dirty) {
                if (updateCount_ >= allowance) {
                    ++stats.pendingCells;
                    continue;
                }
                terrain->vegetationSurface(slot.key, surface, true);
                auto &job = jobs[updateCount_++];
                job = {};
                job.key = {uint32_t(slot.key.x), uint32_t(uint64_t(slot.key.x) >> 32), uint32_t(slot.key.z),
                           uint32_t(uint64_t(slot.key.z) >> 32)};
                job.config = {i, seed_, slot.generated ? 0u : 1u, surface.count};
                job.canonical = surface.canonical;
                job.canonicalGrid = surface.canonicalGrid;
                job.habitat = {settings.habitat.weights, settings.habitat.minimumUp};
                job.elevation = {settings.habitat.minimumElevation, settings.habitat.maximumElevation, 0, 0};
                const auto clearing =
                    relativePosition(settings.exclusion.centre, vegetationCellOrigin(slot.key));
                job.clearing = {float(clearing.x), float(clearing.z), settings.exclusion.radius, 0};
                job.assets = assets_;
                job.patches = surface.patches;
                job.geography = terrain->vegetationGeography(slot.key);
                if (!slot.generated) {
                    ++stats.generatedCells;
                }
                slot.generated = true;
                f.signatures[i] = surface.signature;
                f.generations[i] = slot.generation;
            }
            auto origin = vegetationCellOrigin(slot.key);
            auto &cell = cells[i];
            cell.origin = {glm::vec3(relativePosition(origin, camera)), 1};
            cell.previousOrigin = {glm::vec3(relativePosition(origin, previous)),
                                   previousSignatures_[i] == surface.signature &&
                                           previousGenerations_[i] == slot.generation
                                       ? 1
                                       : 0};
            const float verticalEnvelope = envelope_ / settings.habitat.minimumUp;
            cell.lo = {-envelope_, surface.bounds.x - verticalEnvelope, -envelope_, 0};
            cell.hi = {4 + envelope_, surface.bounds.y + verticalEnvelope, 4 + envelope_, 0};
            cell.casterLo = cell.lo;
            cell.casterHi = cell.hi;
            cell.phase = {float((slot.key.x % 64 + 64) % 64) * 4, float((slot.key.z % 64 + 64) % 64) * 4, 1,
                          0};
            if (config_.diagnostics) {
                diagnosticSurfaces_[i] = surface;
                diagnosticHistory_[i] = cell.previousOrigin.w != 0;
            }
            currentSignatures_[i] = surface.signature;
            currentGenerations_[i] = slot.generation;
            residency_.used(i, serial_ + 1);
            ++stats.residentCells;
        }
    stats.updatedCells = updateCount_;
    uint32_t repairs = 0;
    while (repairs < updateCount_ && !jobs[repairs].config.z)
        ++repairs;
    terrain->vegetationReservation(std::min(.5, repairs * cellCost_), updateCount_);
}
void VulkanVegetationStream::restrictPopulation(double remaining, VegetationGpuCell *cells,
                                                VegetationStats &stats) {
    auto &f = frames_[frame_];
    auto *jobs = jobs_.data();
    uint32_t repairs = 0;
    while (repairs < updateCount_ && !jobs[repairs].config.z)
        ++repairs;
    uint32_t allowed =
        std::max(repairs, std::min(updateCount_, uint32_t(std::max(0.0, remaining) / cellCost_)));
    for (uint32_t j = allowed; j < updateCount_; ++j) {
        uint32_t i = jobs[j].config.x;
        residency_.slot(i).generated = false;
        f.signatures[i] = 0;
        f.generations[i] = 0;
        currentSignatures_[i] = 0;
        currentGenerations_[i] = 0;
        cells[i] = {};
        --stats.generatedCells;
        --stats.residentCells;
        ++stats.pendingCells;
    }
    updateCount_ = allowed;
    stats.updatedCells = allowed;
}
void VulkanVegetationStream::record(VkCommandBuffer cmd) {
    if (!updateCount_)
        return;
    std::memcpy(frames_[frame_].jobs.mapped, jobs_.data(), updateCount_ * sizeof(Job));
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &frames_[frame_].set, 0,
                            nullptr);
    vkCmdDispatch(cmd, updateCount_, 1, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}
void VulkanVegetationStream::submitted() {
    frames_[frame_].serial = ++serial_;
    previousSignatures_ = currentSignatures_;
    previousGenerations_ = currentGenerations_;
}
uint32_t VulkanVegetationStream::identity(uint32_t index) const {
    const auto &s = residency_.slots().at(index / 128);
    uint32_t lane = index % 128;
    return vegetationCandidateHash(s.key, seed_, lane < 126 ? 0 : lane - 125, lane < 126 ? lane : 0);
}
void VulkanVegetationStream::copySnapshot(VkCommandBuffer cmd) {
    if (!config_.diagnostics)
        return;
    VkBufferCopy copy{0, 0, uint64_t(config_.cellCapacity) * 128 * 32};
    vkCmdCopyBuffer(cmd, population_, diagnostic_.buffer, 1, &copy);
    copy = {0, copy.size, uint64_t(config_.cellCapacity) * 128 * 16};
    vkCmdCopyBuffer(cmd, frames_[frame_].attachments.buffer, diagnostic_.buffer, 1, &copy);
}
std::vector<VegetationCellSnapshot> VulkanVegetationStream::snapshot() const {
    std::vector<VegetationCellSnapshot> out;
    if (!config_.diagnostics)
        return out;
    const auto *population = static_cast<const FoliageInstance *>(diagnostic_.mapped);
    const auto *attachments = reinterpret_cast<const glm::vec4 *>(population + config_.cellCapacity * 128);
    for (uint32_t i : residency_.ordered())
        if (currentSignatures_[i]) {
            const auto &slot = residency_.slots()[i];
            VegetationCellSnapshot cell;
            cell.key = slot.key;
            cell.handle = {i, slot.generation};
            cell.surface = diagnosticSurfaces_[i];
            cell.historyValid = diagnosticHistory_[i] != 0;
            std::copy_n(population + i * 128, 128, cell.population.begin());
            std::copy_n(attachments + i * 128, 128, cell.attachment.begin());
            out.push_back(std::move(cell));
        }
    return out;
}
} // namespace ow

#include "vulkan_trees_internal.hpp"
namespace ow {
VulkanTrees::~VulkanTrees() = default;
void VulkanTrees::prepare(uint32_t frame, const Camera &cam, TerrainGrid grid, const TemporalView &temporal,
                          const TreeSettings &settings, VulkanTerrainStream *terrain, bool eroded) {
    auto start = std::chrono::steady_clock::now();
    auto &x = *impl_;
    validate(settings);
    x.frame = frame;
    auto &f = x.frames[frame];
    x.last = x.decode(f);
    if (x.last.timingsAvailable && x.last.updates)
        x.cellCost = std::clamp(x.last.updateMilliseconds / x.last.updates, .001, 4.0);
    x.completed = std::max(x.completed, f.serial);
    f.stats = {};
    f.stats.enabled = settings.enabled;
    f.stats.bytes = x.bytes;
    x.settings = settings;
    x.terrain = terrain;
    x.eroded = eroded;
    x.camera = worldPosition(cam, grid);
    x.groupCount = x.jobCount = 0;
    bool changed = x.oldSettings.coverage != settings.coverage || x.oldSettings.density != settings.density ||
                   x.oldSettings.treeline != settings.treeline;
    if (changed)
        ++x.revision;
    uint64_t revision = x.revision ^ (terrain ? terrain->vegetationRevision() * 0x9e3779b97f4a7c15ull : 0);
    float radius = std::min(settings.distance, x.options.streaming.maximumDistance) + 1024;
    if (!x.fixture)
        x.residency.demand(x.camera, revision, x.completed, settings.enabled && terrain, radius);
    x.current.views[0] = temporal.raster;
    x.current.raster = temporal.raster;
    x.current.curvature = temporal.curvature;
    x.current.settings = {std::min(settings.distance, x.options.streaming.maximumDistance), settings.density,
                          settings.coverage, settings.time};
    x.current.previousSettings = x.previous.settings;
    auto direction =
        glm::length(settings.windDirection) > 1e-5 ? glm::normalize(settings.windDirection) : glm::vec2(1, 0);
    x.current.wind = {direction, settings.wind, 0};
    x.current.previousWind = x.previous.wind;
    x.current.detail = {temporal.pixels.y / (2 * std::tan(glm::radians(Camera::verticalFovDegrees) * .5f)), 0,
                        temporal.state.x, 0};
    bool valid = x.history && temporal.state.y != 0 && settings.forceLod == x.oldSettings.forceLod &&
                 settings.time >= x.previous.settings.w && settings.time - x.previous.settings.w <= .25f;
    x.current.mode = {settings.forceLod < 0 ? ~0u : uint32_t(settings.forceLod), valid ? 1u : 0u,
                      settings.diagnostic ? 1u : 0u, uint32_t(x.serial + 1)};
    x.current.pyramid.w = 0;
    auto *cells = static_cast<Cell *>(f.cells.mapped);
    auto *groups = static_cast<uint32_t *>(f.groups.mapped);
    std::fill_n(cells, x.options.streaming.cellCapacity, Cell{});
    const uint32_t capacity = x.options.streaming.cellCapacity;
    float maxRadius = 0, maxHeight = 0;
    for (auto &a : x.current.bounds) {
        maxRadius = std::max(maxRadius, a.x * 1.2f);
        maxHeight = std::max(maxHeight, a.z * 1.2f);
    }
    auto job = [&](uint32_t slot) {
        if (x.jobCount >= x.options.streaming.updatesPerFrame || !terrain)
            return;
        for (uint32_t j = 0; j < x.jobCount; ++j)
            if (x.updatedSlots[j] == slot)
                return;
        auto &s = x.residency.slot(slot);
        if (!terrain->treeSurface(s.key, x.surface, eroded)) {
            ++f.stats.surfaceMisses;
            return;
        }
        bool generate = !s.generated || x.generations[slot] != s.generation;
        if (!generate && x.signatures[slot] == x.surface.signature)
            return;
        auto &j = static_cast<Job *>(f.jobs.mapped)[x.jobCount];
        j.key = {uint32_t(s.key.x), uint32_t(uint64_t(s.key.x) >> 32), uint32_t(s.key.z),
                 uint32_t(uint64_t(s.key.z) >> 32)};
        j.config = {slot, terrain->vegetationSeed(), generate ? 1u : 0u, x.surface.count};
        j.ecology = {settings.coverage, settings.density, settings.treeline, 0};
        j.publication = {uint32_t(x.serial + 1), uint32_t(x.library.assets.size()), 0, 0};
        std::copy_n(x.surface.patches.begin(), x.surface.count, j.patches.begin());
        j.geography = x.surface.geography;
        x.updatedSlots[x.jobCount] = slot;
        x.updatedSignatures[x.jobCount] = x.surface.signature;
        x.updatedGenerate[x.jobCount] = generate;
        ++x.jobCount;
        // Deferred/missing repairs retain published roots. Keep their old height
        // enclosure as well as the new terrain enclosure until the slot is regenerated.
        x.heights[slot] = generate ? x.surface.bounds
                                   : glm::vec2(std::min(x.heights[slot].x, x.surface.bounds.x),
                                               std::max(x.heights[slot].y, x.surface.bounds.y));
    };
    if (settings.enabled && !x.fixture && terrain) {
        // Reserve half of the update batch for new near-first population, half for
        // round-robin repairs. Missing providers never monopolize the queue.
        uint32_t attempts = 0;
        for (auto slot : x.residency.ordered()) {
            auto &s = x.residency.slot(slot);
            if (!s.generated) {
                job(slot);
                if (++attempts >= 16 || x.jobCount >= 16)
                    break;
            }
        }
        uint32_t examined = 0;
        const auto &ordered = x.residency.ordered();
        uint32_t nearCount = 0;
        while (nearCount < ordered.size() && nearCount < 96 &&
               x.residency.slot(ordered[nearCount]).distanceSquared < 256 * 256)
            ++nearCount;
        for (uint32_t k = 0; k < std::min(8u, nearCount) && x.jobCount < x.options.streaming.updatesPerFrame;
             ++k) {
            auto slot = ordered[x.nearRepairCursor++ % nearCount];
            if (x.residency.slot(slot).generated)
                job(slot);
        }
        while (!ordered.empty() && examined < std::min<size_t>(24, ordered.size()) &&
               x.jobCount < x.options.streaming.updatesPerFrame) {
            auto slot = ordered[x.repairCursor++ % ordered.size()];
            ++examined;
            if (x.residency.slot(slot).generated)
                job(slot);
        }
        VkDescriptorBufferInfo bi{terrain->vegetationHeights(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = f.updateSet;
        w.dstBinding = 3;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(x.device, 1, &w, 0, nullptr);
    }
    for (uint32_t i = 0; i < capacity; ++i) {
        auto &s = x.residency.slot(i);
        if (s.state != TreeResidency::State::Active)
            continue;
        bool ready = s.generated;
        for (uint32_t j = 0; j < x.jobCount; ++j)
            ready |= x.updatedSlots[j] == i;
        if (!ready) {
            ++f.stats.pending;
            continue;
        }
        if (!settings.enabled)
            continue;
        auto origin = treeCellOrigin(s.key);
        auto rel = relativePosition(origin, x.camera), prev = relativePosition(origin, x.previousCamera);
        auto &c = cells[i];
        c.origin = {glm::vec3(rel), 1};
        c.previous = {glm::vec3(prev), valid && x.previousGenerations[i] == s.generation ? 1.f : 0.f};
        c.lo = {-maxRadius, x.heights[i].x - 2, -maxRadius, 0};
        c.hi = {64 + maxRadius, x.heights[i].y + maxHeight, 64 + maxRadius, 0};
        c.phase = {float((uint64_t(s.key.x) & 1023) * 64), float((uint64_t(s.key.z) & 1023) * 64), 0, 0};
        c.meta = {1, s.generation, 0, 0};
        groups[x.groupCount++] = i;
        ++f.stats.resident;
    }
    x.current.limits = {x.groupCount, capacity * 128, uint32_t(x.library.assets.size()), 0};
    f.stats.deferred = x.residency.deferred();
    f.stats.updates = x.jobCount;
    if (terrain && !x.fixture)
        terrain->reserveTreePopulation(std::min(.75, x.jobCount * x.cellCost), x.jobCount);
    std::memcpy(f.uniform.mapped, &x.current, sizeof(Uniform));
    f.stats.cpuMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
VkBuffer VulkanTrees::canopyBuffer() const { return impl_->frames[impl_->frame].canopy.buffer; }
double VulkanTrees::budgetStream(double milliseconds) {
    auto &x = *impl_;
    x.jobCount =
        populationUpdateAllowance(x.jobCount, 0, std::max(0.0, std::min(.75, milliseconds)) * .9, x.cellCost);
    auto &f = x.frames[x.frame];
    f.stats.updates = x.jobCount;
    auto *cells = static_cast<Cell *>(f.cells.mapped);
    auto *groups = static_cast<uint32_t *>(f.groups.mapped);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < x.groupCount; ++k) {
        uint32_t slot = groups[k];
        bool ready = x.residency.slot(slot).generated;
        for (uint32_t j = 0; j < x.jobCount; ++j)
            ready |= x.updatedSlots[j] == slot;
        if (ready)
            groups[kept++] = slot;
        else {
            cells[slot].meta.x = 0;
            ++f.stats.pending;
            --f.stats.resident;
        }
    }
    x.groupCount = kept;
    x.current.limits.x = kept;
    return x.jobCount * x.cellCost;
}
void VulkanTrees::bounds(VulkanShadows &shadows) {
    auto &x = *impl_;
    if (!x.settings.enabled)
        return;
    auto *cells = static_cast<Cell *>(x.frames[x.frame].cells.mapped);
    auto *groups = static_cast<uint32_t *>(x.frames[x.frame].groups.mapped);
    for (uint32_t i = 0; i < x.groupCount; ++i) {
        auto &c = cells[groups[i]];
        auto lo = glm::vec3(c.lo + c.origin), hi = glm::vec3(c.hi + c.origin);
        curveBounds(lo, hi, x.current.curvature);
        shadows.includeCasterBounds({lo, hi, true});
    }
}
void VulkanTrees::preparePyramid(VisibilityPyramidView p) {
    auto &x = *impl_;
    if (!p.image)
        return;
    imageDescriptor(x.device, x.frames[x.frame].set, 9, p.image, {}, VK_IMAGE_LAYOUT_GENERAL);
    x.current.pyramid = {p.width, p.height, p.levels, 0};
}
void VulkanTrees::updateAndSelectCasters(VkCommandBuffer cmd, const ShadowUniform &shadow, uint32_t views) {
    auto &x = *impl_;
    auto &f = x.frames[x.frame];
    f.stamped = 0;
    if (x.queries)
        vkCmdResetQueryPool(cmd, x.queries, x.frame * queryCount, queryCount);
    vkCmdFillBuffer(cmd, f.work.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, f.indirect.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    x.current.sun = shadow.sun;
    for (uint32_t v = 1; v < 6; ++v) {
        x.current.views[v] = shadow.matrices[v - 1];
        x.current.shadowFootprint[v] = {shadow.ranges[v - 1].z, 0, 0, 0};
    }
    std::memcpy(f.uniform.mapped, &x.current, sizeof(Uniform));
    x.stamp(cmd, 0);
    if (x.jobCount && x.settings.enabled) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, x.populate);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, x.populateLayout, 0, 1, &f.updateSet, 0,
                                nullptr);
        vkCmdDispatch(cmd, x.jobCount, 1, 1);
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    x.stamp(cmd, 1);
    x.stamp(cmd, 18);
    vkCmdFillBuffer(cmd, f.canopy.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (x.settings.enabled && x.groupCount) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, x.canopyPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, x.layout, 0, 1, &f.set, 0, nullptr);
        vkCmdDispatch(cmd, x.groupCount, 1, 1);
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    for (uint32_t v = 1; v <= views; ++v)
        x.selectView(cmd, v);
    x.stamp(cmd, 19);
}
void VulkanTrees::selectCamera(VkCommandBuffer cmd, bool occlusion) {
    auto &x = *impl_;
    x.current.pyramid.w = occlusion && x.settings.occlusion && x.current.pyramid.z ? 1 : 0;
    std::memcpy(x.frames[x.frame].uniform.mapped, &x.current, sizeof(Uniform));
    x.stamp(cmd, 2);
    x.selectView(cmd, 0);
    x.stamp(cmd, 3);
}
void VulkanTrees::draw(VkCommandBuffer cmd, VkDescriptorSet lighting, uint32_t pass, uint32_t view) {
    auto &x = *impl_;
    if (!x.settings.enabled || !x.groupCount)
        return;
    auto &f = x.frames[x.frame];
    uint32_t pipeline = pass == 4 ? 3 : pass;
    // Temporal attachment presence is captured during prepare from the renderer
    // view.
    if (pass < 2 && x.current.detail.z != 0)
        pipeline = pass + 4;
    uint32_t stamp = pass == 4 ? 8 + (view - 1) * 2 : pass == 2 ? 4 : 6;
    x.stamp(cmd, stamp);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.pipelines[pipeline]);
    VkDescriptorSet sets[] = {f.set, lighting};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.layout, 0, 2, sets, 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &x.geometry.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, x.geometry.buffer, x.indexOffset, VK_INDEX_TYPE_UINT32);
    for (uint32_t a = 0; a < x.library.assets.size(); ++a) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.layout, 2, 1, &x.assetSets[a], 0,
                                nullptr);
        for (uint32_t lod = 0; lod < 4; ++lod)
            for (uint32_t part = 0; part < (lod == 3 ? 1u : 2u); ++part) {
                uint32_t bin = a * 4 + lod, push[] = {view, bin, 0, part};
                vkCmdPushConstants(cmd, x.layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                       VK_SHADER_STAGE_COMPUTE_BIT,
                                   0, 16, push);
                vkCmdDrawIndexedIndirect(cmd, f.indirect.buffer, ((view * 96 + bin) * 2 + part) * 20, 1, 20);
            }
    }
    x.stamp(cmd, stamp + 1);
}
void VulkanTrees::finish(VkCommandBuffer cmd) {
    auto &x = *impl_;
    auto &f = x.frames[x.frame];
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy{0, 0, f.readback.size};
    vkCmdCopyBuffer(cmd, f.work.buffer, f.readback.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
void VulkanTrees::submitted() {
    auto &x = *impl_;
    ++x.serial;
    x.frames[x.frame].serial = x.serial;
    for (uint32_t j = 0; j < x.jobCount; ++j) {
        auto i = x.updatedSlots[j];
        auto &s = x.residency.slot(i);
        s.generated = true;
        x.signatures[i] = x.updatedSignatures[j];
        x.generations[i] = s.generation;
    }
    auto *groups = static_cast<uint32_t *>(x.frames[x.frame].groups.mapped);
    for (uint32_t i = 0; i < x.groupCount; ++i) {
        auto slot = groups[i];
        x.residency.used(slot, x.serial);
        x.previousGenerations[slot] = x.residency.slot(slot).generation;
    }
    x.previousCamera = x.camera;
    x.previous = x.current;
    x.oldSettings = x.settings;
    x.history = x.settings.enabled;
}
const TreeStats &VulkanTrees::stats() const { return impl_->last; }
void VulkanTrees::copySnapshot(VkCommandBuffer cmd) {
    auto &x = *impl_;
    constexpr uint64_t stride = 128 * (sizeof(TreeInstance) + sizeof(glm::vec4));
    if (!x.debugReadback.buffer)
        x.allocate(x.debugReadback, stride * 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    x.debugCount = std::min(64u, x.groupCount);
    auto *groups = static_cast<const uint32_t *>(x.frames[x.frame].groups.mapped);
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (uint32_t i = 0; i < x.debugCount; ++i) {
        uint32_t slot = groups[i];
        x.debugKeys[i] = x.residency.slot(slot).key;
        VkBufferCopy a{uint64_t(slot) * 128 * sizeof(TreeInstance), i * stride, 128 * sizeof(TreeInstance)};
        vkCmdCopyBuffer(cmd, x.population.buffer, x.debugReadback.buffer, 1, &a);
        VkBufferCopy b{uint64_t(slot) * 128 * sizeof(glm::vec4), i * stride + 128 * sizeof(TreeInstance),
                       128 * sizeof(glm::vec4)};
        vkCmdCopyBuffer(cmd, x.attachment.buffer, x.debugReadback.buffer, 1, &b);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}
TreeSnapshot VulkanTrees::snapshot() const {
    auto &x = *impl_;
    TreeSnapshot out;
    out.cells.resize(x.debugCount);
    constexpr uint64_t stride = 128 * (sizeof(TreeInstance) + sizeof(glm::vec4));
    for (uint32_t i = 0; i < x.debugCount; ++i) {
        auto &c = out.cells[i];
        c.key = x.debugKeys[i];
        const auto *p = static_cast<const char *>(x.debugReadback.mapped) + stride * i;
        std::memcpy(c.population.data(), p, sizeof(c.population));
        std::memcpy(c.attachments.data(), p + sizeof(c.population), sizeof(c.attachments));
        if (x.terrain)
            x.terrain->treeSurface(c.key, c.surface, x.eroded);
    }
    return out;
}
} // namespace ow

#include "vulkan_terrain_stream.hpp"
#include <cmath>
namespace ow {
bool VulkanTerrainStream::treeSurface(TreeCellKey key, TreeTerrainCell &out, bool eroded) {
    out.count = 0;
    out.signature = 0;
    out.bounds = {INFINITY, -INFINITY};
    if (!ground_)
        return false;
    auto origin = treeCellOrigin(key);
    const double ratio = 64.0 / layout_.grid.spacing;
    if (ratio < 1 || ratio > 1048576 || std::floor(ratio) != ratio)
        return false;
    const int64_t span = int64_t(ratio), limit = INT64_MAX / span - 1;
    if (key.x < -limit || key.x > limit || key.z < -limit || key.z > limit)
        return false;
    std::array<uint32_t, 128> supporting{};
    uint32_t supportCount = 0;
    for (uint32_t i = 0; i < 128; ++i) {
        auto p = treeCandidatePosition(key, settings_.seed, i);
        auto patch = ground_->casterAt(key.x * span + int64_t(std::floor(p.x / layout_.grid.spacing)),
                                       key.z * span + int64_t(std::floor(p.y / layout_.grid.spacing)));
        if (patch != ~0u)
            supporting[supportCount++] = patch;
    }
    std::sort(supporting.begin(), supporting.begin() + supportCount);
    supportCount =
        uint32_t(std::unique(supporting.begin(), supporting.begin() + supportCount) - supporting.begin());
    uint64_t hash = epoch_, patchSum = 0, patchXor = 0;
    auto mix = [&](uint64_t w) { hash = (hash ^ w) * 1099511628211ull; };
    mix(eroded);
    // Query the existing partition index instead of scanning every terrain patch.
    for (uint32_t i = 0; i < supportCount; ++i) {
        const auto &p = ground_->casters()[supporting[i]];
        auto &slot = stream_->slot(p.slot);
        if (slot.state != TerrainStreamer::State::Resident)
            continue;
        Camera provider;
        provider.chunk = {slot.chunk.x * (int64_t(1) << slot.level),
                          slot.chunk.z * (int64_t(1) << slot.level)};
        auto delta = relativePosition(worldPosition(provider, layout_.grid), origin);
        double spacing = layout_.grid.spacing * stream_->scale(p.slot);
        if (out.count == out.maximumPatches)
            return false;
        auto &r = out.patches[out.count++];
        r.rectangle = {float(delta.x), float(delta.z), float(spacing), 0};
        r.geometry = {p.x, p.z, p.step, p.cells};
        auto base = heightOffset(p.slot) - heightSkip(p.slot);
        if (p.level + 1 >= layout_.heightOffsets.size())
            return false;
        r.heights = {uint32_t((base + layout_.heightOffsets[p.level]) / 8),
                     uint32_t((base + layout_.heightOffsets[p.level + 1]) / 8),
                     (layout_.grid.cells >> p.level) + 1, (layout_.grid.cells >> (p.level + 1)) + 1};
        r.topology = {p.edges, p.corners, p.level, eroded ? 1u : 0u};
        uint64_t ph = 1469598103934665603ull;
        for (uint64_t w :
             {uint64_t(p.slot), uint64_t(slot.generation), slot.epoch, (uint64_t(p.x) << 32) | p.z,
              (uint64_t(p.step) << 32) | p.cells, (uint64_t(p.edges) << 32) | p.corners, uint64_t(p.level)})
            ph = (ph ^ w) * 1099511628211ull;
        patchSum += ph;
        patchXor ^= ph;
        out.bounds.x = std::min(out.bounds.x, meta_[p.slot].heights.x);
        out.bounds.y = std::max(out.bounds.y, meta_[p.slot].heights.y);
        slot.lastUse = std::max(slot.lastUse, serial_ + 1);
    }
    if (!out.count)
        return false;
    out.geography = terrainParameters({128, .5f}, {{key.x, key.z}, settings_});
    out.geography.sampling.w = 1;
    mix(patchSum);
    mix(patchXor);
    mix(out.count);
    out.signature = hash ? hash : 1;
    return true;
}
} // namespace ow

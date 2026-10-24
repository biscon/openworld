#include "vulkan_terrain_stream.hpp"
#include <cmath>
#include <cstring>
namespace ow {
void VulkanTerrainStream::prepareVegetation(const Camera &camera, bool eroded) {
    if (!ground_) {
        vegetationReady_ = false;
        return;
    }
    const auto centre = vegetationCellAt(worldPosition(camera, layout_.grid));
    const double extent = layout_.grid.extent();
    if (std::fmod(extent, 4.0) != 0 || extent < 4) {
        vegetationReady_ = false;
        return;
    }
    uint64_t partition = 1469598103934665603ull;
    auto mix = [&](uint64_t word) { partition = (partition ^ word) * 1099511628211ull; };
    mix(epoch_);
    mix(eroded);
    for (const auto &slot : stream_->slots()) {
        mix(slot.generation);
        mix(uint32_t(slot.state));
    }
    for (const auto &p : ground_->casters()) {
        mix(p.slot);
        mix((uint64_t(p.x) << 32) | p.z);
        mix((uint64_t(p.step) << 32) | p.cells);
        mix((uint64_t(p.edges) << 32) | p.corners);
        mix(p.level);
    }
    if (vegetationReady_ && partition == vegetationPartition_ && centre == vegetationCentre_)
        return;
    vegetationReady_ = true;
    vegetationEroded_ = eroded;
    vegetationCentre_ = centre;
    vegetationPartition_ = partition;
    for (auto &b : vegetationBuckets_)
        b.count = 0;
    const auto origin = vegetationCellOrigin(vegetationCentre_);
    const auto &patches = ground_->casters();
    for (uint32_t i = 0; i < patches.size(); ++i) {
        const auto &p = patches[i];
        const auto &slot = stream_->slot(p.slot);
        Camera provider;
        provider.chunk = {slot.chunk.x * (int64_t(1) << slot.level),
                          slot.chunk.z * (int64_t(1) << slot.level)};
        const auto offset = relativePosition(worldPosition(provider, layout_.grid), origin);
        const double spacing = layout_.grid.spacing * stream_->scale(p.slot);
        const double x = offset.x + p.x * spacing, z = offset.z + p.z * spacing;
        const double side = p.cells * p.step * spacing;
        // Expanded for footprint samples; only nearby cells enter this bounded index.
        int x0 = int(std::clamp(std::floor((x - 1.5) / 4), -25.0, 26.0));
        int z0 = int(std::clamp(std::floor((z - 1.5) / 4), -25.0, 26.0));
        int x1 = int(std::clamp(std::floor((x + side + 1.5) / 4), -26.0, 25.0));
        int z1 = int(std::clamp(std::floor((z + side + 1.5) / 4), -26.0, 25.0));
        for (int iz = z0; iz <= z1; ++iz)
            for (int ix = x0; ix <= x1; ++ix) {
                auto &b = vegetationBuckets_[(iz + 25) * 51 + ix + 25];
                if (b.count < b.indices.size())
                    b.indices[b.count] = i;
                ++b.count; // overflow is explicit; never silently truncate a surface
            }
    }
}
bool VulkanTerrainStream::vegetationSurface(VegetationCellKey key, VegetationTerrainCell &out, bool retain) {
    out = {};
    if (!vegetationReady_)
        return false;
    int64_t x = key.x - vegetationCentre_.x, z = key.z - vegetationCentre_.z;
    if (std::abs(x) > 25 || std::abs(z) > 25)
        return false;
    const auto &bucket = vegetationBuckets_[size_t(z + 25) * 51 + size_t(x + 25)];
    if (!bucket.count || bucket.count > out.maximumPatches)
        return false;
    out.bounds = {INFINITY, -INFINITY};
    out.populationRevision = epoch_;
    out.seed = settings_.seed;
    uint64_t signature = 1469598103934665603ull;
    auto mix = [&](uint64_t word) { signature = (signature ^ word) * 1099511628211ull; };
    mix(epoch_);
    mix(vegetationEroded_);
    const auto origin = vegetationCellOrigin(key);
    const auto &patches = ground_->casters();
    for (uint32_t i = 0; i < bucket.count; ++i) {
        const auto &p = patches[bucket.indices[i]];
        auto &slot = stream_->slot(p.slot);
        if (slot.state != TerrainStreamer::State::Resident)
            return false;
        Camera provider;
        provider.chunk = {slot.chunk.x * (int64_t(1) << slot.level),
                          slot.chunk.z * (int64_t(1) << slot.level)};
        const auto offset = relativePosition(worldPosition(provider, layout_.grid), origin);
        auto &r = out.patches[out.count++];
        r.rectangle = {float(offset.x), float(offset.z), layout_.grid.spacing * stream_->scale(p.slot), 0};
        r.geometry = {p.x, p.z, p.step, p.cells};
        const auto base = heightOffset(p.slot) - heightSkip(p.slot);
        const auto level = p.level;
        if (level + 1 >= layout_.heightOffsets.size())
            return false;
        r.heights = {uint32_t((base + layout_.heightOffsets[level]) / 8),
                     uint32_t((base + layout_.heightOffsets[level + 1]) / 8),
                     (layout_.grid.cells >> level) + 1, (layout_.grid.cells >> (level + 1)) + 1};
        r.topology = {p.edges, p.corners, level, vegetationEroded_ ? 1u : 0u};
        mix(p.slot);
        mix(slot.generation);
        mix(slot.epoch);
        mix((uint64_t(p.x) << 32) | p.z);
        mix((uint64_t(p.step) << 32) | p.cells);
        mix((uint64_t(p.edges) << 32) | p.corners);
        mix(p.level);
        out.bounds.x = std::min(out.bounds.x, meta_[p.slot].heights.x);
        out.bounds.y = std::max(out.bounds.y, meta_[p.slot].heights.y);
        if (retain)
            slot.lastUse = std::max(slot.lastUse, serial_ + 1);
    }
    const int64_t cells = int64_t(std::llround(layout_.grid.extent() / 4));
    const auto tx = terrainFloorDivide(key.x, cells), tz = terrainFloorDivide(key.z, cells);
    int provider = stream_->findTile({tx, tz, 0});
    if (provider >= 0 && stream_->slot(provider).state == TerrainStreamer::State::Resident &&
        !compact(provider)) {
        out.canonical = {uint32_t(heightOffset(provider) / 8), layout_.grid.cells + 1,
                         vegetationEroded_ ? 1u : 0u, 1};
        out.canonicalGrid = {float(-(key.x - tx * cells) * 4), float(-(key.z - tz * cells) * 4),
                             layout_.grid.spacing, 0};
        if (retain)
            stream_->slot(provider).lastUse = std::max(stream_->slot(provider).lastUse, serial_ + 1);
    }
    out.signature = signature ? signature : 1;
    return true;
}
} // namespace ow

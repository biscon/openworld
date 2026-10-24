#include "terrain_stream.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace ow {
TerrainPoolCapacity terrainPoolCapacity(uint64_t available, uint32_t maximumSlots, bool hasDetail,
                                        const std::function<uint64_t(uint32_t)> &coverageBytes,
                                        const std::function<uint64_t(uint32_t)> &detailBytes) {
    const uint64_t minimumDetail = hasDetail ? detailBytes(1) : 0;
    for (int radius = 8; radius >= 0; --radius) {
        const uint32_t side = uint32_t(2 * (radius + 1) + 1);
        const uint32_t coverage = side * side + 4;
        if (coverage > maximumSlots)
            continue;
        const uint64_t bytes = coverageBytes(coverage);
        if (bytes > available / 2 || minimumDetail > available - bytes)
            continue;
        uint32_t low = hasDetail ? 1 : 0, high = hasDetail ? coverage : 0;
        while (low < high) {
            const uint32_t mid = low + (high - low + 1) / 2;
            if (detailBytes(mid) <= available - bytes)
                low = mid;
            else
                high = mid - 1;
        }
        return {coverage, low, radius};
    }
    throw std::invalid_argument("Terrain GPU pool budget (" + std::to_string(available) +
                                " bytes) or device layer limit cannot fit "
                                "coverage and replacement slots" +
                                (hasDetail ? " plus one detail slot" : ""));
}
std::filesystem::path terrainDefaultCacheDirectory() {
#ifdef _WIN32
    if (const char *base = std::getenv("LOCALAPPDATA"); base && *base)
        return std::filesystem::path(base) / "OpenWorld/terrain-v1";
#elif defined(__APPLE__)
    if (const char *base = std::getenv("HOME"); base && *base)
        return std::filesystem::path(base) / "Library/Caches/OpenWorld/terrain-v1";
#else
    if (const char *base = std::getenv("XDG_CACHE_HOME"); base && *base)
        return std::filesystem::path(base) / "openworld/terrain-v1";
    if (const char *base = std::getenv("HOME"); base && *base)
        return std::filesystem::path(base) / ".cache/openworld/terrain-v1";
#endif
    return {};
}

TerrainPayloadLayout::TerrainPayloadLayout(TerrainGrid g, uint32_t r, uint32_t p, bool filtered)
    : grid(g), subdivisions(r), patchCells(p), levels(1) {
    validateLodGrid(g, p);
    if (r < 1 || r > 2)
        throw std::invalid_argument("Normal subdivisions must be 1 or 2");
    for (auto n = g.cells / p; n > 1; n >>= 1)
        ++levels;
    filteredHeights = filtered;
    heightsBytes = uint64_t(g.cells + 1) * (g.cells + 1) * 8;
    if (filtered) {
        heightsBytes = 0;
        for (uint32_t l = 0; l <= levels; ++l) {
            heightOffsets[l] = heightsBytes;
            const uint64_t width = (g.cells >> l) + 1;
            heightsBytes += width * width * 8;
        }
    }
    boundsOffset = heightsBytes;
    leafBoundsBytes = uint64_t(g.cells / p) * (g.cells / p) * 8;
    boundsBytes = leafBoundsBytes;
    errorOffset = boundsBytes;
    if (filtered)
        for (uint32_t l = 0; l < levels; ++l) {
            errorOffsets[l] = boundsBytes;
            const uint64_t width = (g.cells / p) >> l;
            boundsBytes += width * width * sizeof(TerrainPatchMetadata);
        }
    coarseOffset = boundsOffset + boundsBytes;
    for (uint32_t l = 0; l < levels; ++l) {
        halos[l] = (1u << (levels - 1 - l)) - 1;
        widths[l] = (g.cells * r >> l) + 1 + 2 * halos[l];
        bytes[l] = uint64_t(widths[l]) * widths[l] * 16;
        offsets[l] = detailBytes;
        if (l + 1 < levels)
            detailBytes += bytes[l];
    }
    coverageBytes = coarseOffset + bytes[levels - 1];
}
bool TerrainPayloadLayout::valid(TerrainProduct product, const uint8_t *data, size_t sizeBytes) const {
    if (sizeBytes != size(product))
        return false;
    if (product == TerrainProduct::Coverage) {
        for (uint64_t i = 0; i < coarseOffset; i += 4) {
            float f;
            std::memcpy(&f, data + i, 4);
            if (!std::isfinite(f))
                return false;
        }
        for (uint64_t i = boundsOffset; i < boundsOffset + leafBoundsBytes; i += 8) {
            float a, b;
            std::memcpy(&a, data + i, 4);
            std::memcpy(&b, data + i + 4, 4);
            if (a > b)
                return false;
        }
        if (filteredHeights) {
            for (uint64_t i = boundsOffset + errorOffset; i < coarseOffset;
                 i += sizeof(TerrainPatchMetadata)) {
                float v[8];
                std::memcpy(v, data + i, sizeof(v));
                if (v[0] > v[1] || std::any_of(v + 2, v + 8, [](float x) { return x < 0; }))
                    return false;
            }
        }
        // Bounds must cover every vertex used by each leaf, including shared edges.
        const auto w = grid.cells + 1, leaves = grid.cells / patchCells;
        for (uint32_t z = 0; z < leaves; ++z)
            for (uint32_t x = 0; x < leaves; ++x) {
                float lo, hi;
                auto i = boundsOffset + (uint64_t(z) * leaves + x) * 8;
                std::memcpy(&lo, data + i, 4);
                std::memcpy(&hi, data + i + 4, 4);
                for (uint32_t v = 0; v <= patchCells; ++v)
                    for (uint32_t u = 0; u <= patchCells; ++u) {
                        float h[2];
                        std::memcpy(h, data + (uint64_t(z * patchCells + v) * w + x * patchCells + u) * 8, 8);
                        if (h[0] < lo || h[0] > hi || h[1] < lo || h[1] > hi)
                            return false;
                    }
            }
    }
    const uint64_t begin = product == TerrainProduct::Coverage ? coarseOffset : 0;
    // Reject infinities/NaNs in half-float normal texels, including padding.
    for (uint64_t i = begin; i < sizeBytes; i += 2) {
        uint16_t h;
        std::memcpy(&h, data + i, 2);
        if ((h & 0x7c00) == 0x7c00)
            return false;
    }
    return true;
}
namespace {
int64_t distance(ChunkCoord a, ChunkCoord b) {
    return std::max(std::llabs(a.x - b.x), std::llabs(a.z - b.z));
}
} // namespace
TerrainStreamer::TerrainStreamer(uint32_t capacity, int maximumRadius, TerrainGrid grid, bool hierarchical,
                                 uint32_t shadowSlots, uint32_t horizonSlots, int horizonRadius)
    : slots_(capacity), cameraCapacity_(capacity - shadowSlots),
      baseCapacity_(capacity - shadowSlots - horizonSlots), horizonSlots_(horizonSlots),
      maximumRadius_(maximumRadius), grid_(grid), hierarchical_(hierarchical) {
    if (horizonSlots > capacity || shadowSlots > capacity - horizonSlots ||
        (horizonSlots &&
         (!hierarchical || (horizonRadius == 0 ? horizonSlots != 48 : horizonSlots % 48 != 0))))
        throw std::invalid_argument("Invalid horizon residency reservation");
    if (shadowSlots > capacity || shadowSlots > 64 || (!hierarchical && shadowSlots))
        throw std::invalid_argument("Invalid shadow residency reservation");
    spatial_.fill(-1);
    if (hierarchical_) {
        if (maximumRadius < 0 || maximumRadius > terrainMaximumRadius(grid) ||
            baseCapacity_ < terrainHierarchyCapacity(maximumRadius))
            throw std::invalid_argument("Invalid hierarchy capacity");
        detailedLevel_ = topLevel_ = terrainHierarchyLevel(maximumRadius);
        if (horizonRadius > maximumRadius && horizonSlots) {
            const auto level = terrainHierarchyLevel(horizonRadius);
            if (level >= 30 || horizonSlots < 48 * (level - topLevel_))
                throw std::invalid_argument("Insufficient distant hierarchy slots");
            topLevel_ = level;
            horizonRadius_ = horizonRadius;
        }
        wanted_.reserve(capacity);
        size_t size = 1;
        while (size < capacity * 4)
            size *= 2;
        table_.resize(size, -1);
        return;
    }
    if (maximumRadius < 0 || maximumRadius > 8 ||
        capacity < uint32_t((2 * maximumRadius + 3) * (2 * maximumRadius + 3) + 4))
        throw std::invalid_argument("Invalid streaming capacity");
}
int TerrainStreamer::find(ChunkCoord c) const {
    if (hierarchical_)
        return findTile({c.x, c.z, 0});
    if (c.x < centre_.x - 9 || c.x > centre_.x + 9 || c.z < centre_.z - 9 || c.z > centre_.z + 9)
        return -1;
    const auto x = c.x - centre_.x, z = c.z - centre_.z;
    return spatial_[size_t(z + 9) * 19 + size_t(x + 9)];
}
bool TerrainStreamer::valid(TerrainHandle h) const {
    return h.index < capacity() && slots_[h.index].generation == h.generation &&
           slots_[h.index].state != State::Empty && slots_[h.index].state != State::Retiring;
}
void TerrainStreamer::shadowDemand(const TerrainTileKey *keys, uint32_t count) {
    if (count > shadowWanted_.size() || (count && (!keys || !hierarchical_)))
        throw std::invalid_argument("Shadow terrain demand capacity");
    for (uint32_t i = 0; i < count; ++i)
        if (keys[i].level != detailedLevel_)
            throw std::invalid_argument("Shadow residency requires the detailed coverage level");
    shadowCount_ = count;
    if (count)
        std::copy_n(keys, count, shadowWanted_.begin());
}
void TerrainStreamer::canonicalDemand(float metres) {
    if (!std::isfinite(metres) || metres < 0 || metres > 216)
        throw std::invalid_argument("Invalid canonical terrain demand radius");
    canonicalRadius_ = metres;
}
void TerrainStreamer::demand(const Camera &camera, int radius, uint64_t completed, uint64_t submitted,
                             uint64_t epoch) {
    if (camera.chunk.x < -1000000000000ll || camera.chunk.x > 1000000000000ll ||
        camera.chunk.z < -1000000000000ll || camera.chunk.z > 1000000000000ll)
        throw std::invalid_argument("Streaming camera coordinate out of range");
    if (hierarchical_) {
        demandHierarchy(camera, radius, completed, submitted, epoch);
        return;
    }
    centre_ = camera.chunk;
    radius_ = std::clamp(radius, 0, maximumRadius_);
    outerRadius_ = horizonRadius_ ? horizonRadius_ : horizonSlots_ ? radius_ * 2 : radius_;
    for (auto &s : slots_) {
        if (s.state == State::Retiring && s.retireAfter <= completed) {
            const auto g = s.generation;
            s = {};
            s.generation = g;
        }
        if (s.state == State::Empty || s.state == State::Retiring)
            continue;
        auto d = distance(s.chunk, centre_);
        s.wanted = d <= radius_ + 1 && s.epoch == epoch;
        s.visible = d <= radius_ && s.epoch == epoch;
        s.detailWanted = s.visible;
        if (!s.wanted) {
            s.state = State::Retiring;
            s.retireAfter = std::max(s.lastUse, submitted);
            ++s.generation;
            s.visible = false;
        }
    }
    spatial_.fill(-1);
    for (uint32_t i = 0; i < capacity(); ++i) {
        const auto &s = slots_[i];
        if (s.state != State::Empty && s.state != State::Retiring && s.wanted)
            spatial_[size_t(s.chunk.z - centre_.z + 9) * 19 + size_t(s.chunk.x - centre_.x + 9)] = int(i);
    }
    // Rings are stable, bounded and nearest-first; no frame-time
    // sorting/allocation.
    for (int ring = 0; ring <= radius_ + 1; ++ring)
        for (int z = -ring; z <= ring; ++z)
            for (int x = -ring; x <= ring; ++x) {
                if (std::max(std::abs(x), std::abs(z)) != ring)
                    continue;
                ChunkCoord c{centre_.x + x, centre_.z + z};
                if (find(c) >= 0)
                    continue;
                for (uint32_t i = 0; i < cameraCapacity_; ++i)
                    if (slots_[i].state == State::Empty) {
                        auto &s = slots_[i];
                        spatial_[size_t(z + 9) * 19 + size_t(x + 9)] = int(i);
                        s.chunk = c;
                        s.epoch = epoch;
                        s.state = State::Requested;
                        s.wanted = true;
                        s.visible = ring <= radius_;
                        s.detailWanted = s.visible;
                        break;
                    }
            }
}
TerrainHandle TerrainStreamer::next(State state) const {
    uint32_t best = invalidTerrainSlot;
    int64_t d = INT64_MAX;
    for (uint32_t i = 0; i < capacity(); ++i)
        if (slots_[i].state == state && slots_[i].wanted) {
            const auto &s = slots_[i];
            // Coarse ancestors first; within a level prefer nearby visible data.
            const auto value = hierarchical_ ? int64_t((s.shadowOnly                       ? 100000000
                                                        : s.horizonOnly && !horizonRadius_ ? 50000000
                                                                                           : 0) +
                                                       (topLevel_ - s.level) * 1000000 +
                                                       (s.refine ? 0 : 500000) + tileDistance(s.tile()) * 100)
                                             : distance(s.chunk, centre_);
            if (value < d) {
                d = value;
                best = i;
            }
        }
    return best == invalidTerrainSlot ? TerrainHandle{} : TerrainHandle{best, slots_[best].generation};
}
void TerrainStreamer::weights(uint32_t index, float out[16]) const {
    const auto &s = slots_[index];
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x) {
            float f = s.fade;
            const int dx = x == 0 ? -1 : x == 3 ? 1 : 0, dz = z == 0 ? -1 : z == 3 ? 1 : 0;
            for (int oz = std::min(0, dz); oz <= std::max(0, dz); ++oz)
                for (int ox = std::min(0, dx); ox <= std::max(0, dx); ++ox) {
                    const int n = hierarchical_ ? findTile({s.chunk.x + ox, s.chunk.z + oz, s.level})
                                                : find({s.chunk.x + ox, s.chunk.z + oz});
                    if (n >= 0 && slots_[n].state == State::Resident)
                        f = std::min(f, slots_[n].fade);
                    else
                        f = 0;
                }
            out[z * 4 + x] = f;
        }
}

int64_t terrainFloorDivide(int64_t value, int64_t divisor) {
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}
TerrainTileKey terrainParent(TerrainTileKey t) {
    return {terrainFloorDivide(t.x, 2), terrainFloorDivide(t.z, 2), t.level + 1};
}
uint32_t terrainHierarchyLevel(int radius) {
    uint32_t level = 0;
    while ((1u << level) < uint32_t(std::max(1, radius)))
        ++level;
    return level;
}
uint32_t terrainHierarchyCapacity(int radius) { return 48 * (terrainHierarchyLevel(radius) + 1) + 16; }
int terrainMaximumRadius(TerrainGrid grid) { return std::max(1, int(16384.0 / grid.extent())); }
TerrainPoolCapacity terrainHierarchyPoolCapacity(uint64_t available, uint32_t maximumSlots, int radius,
                                                 bool hasDetail,
                                                 const std::function<uint64_t(uint32_t)> &coverageBytes,
                                                 const std::function<uint64_t(uint32_t)> &detailBytes) {
    const uint32_t minimumDetail = hasDetail ? 4 : 0;
    for (int r = radius;; r /= 2) {
        const auto capacity = terrainHierarchyCapacity(r);
        if (capacity <= maximumSlots) {
            const auto coverage = coverageBytes(capacity);
            const auto detail = minimumDetail ? detailBytes(minimumDetail) : 0;
            if (coverage <= available && detail <= available - coverage) {
                uint32_t lo = minimumDetail, hi = hasDetail ? capacity : 0;
                while (lo < hi) {
                    const auto mid = lo + (hi - lo + 1) / 2;
                    if (detailBytes(mid) <= available - coverage)
                        lo = mid;
                    else
                        hi = mid - 1;
                }
                return {capacity, lo, r};
            }
        }
        if (!r)
            break;
    }
    throw std::invalid_argument("Terrain budget/device limit cannot fit "
                                "hierarchy coverage and near detail");
}
namespace {
uint64_t tileHash(TerrainTileKey t) {
    auto mix = [](uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    };
    return mix(uint64_t(t.x)) ^ mix(uint64_t(t.z) + 0x9e3779b97f4a7c15ull) ^ mix(t.level + 17);
}
} // namespace
int TerrainStreamer::findTile(TerrainTileKey key) const {
    if (!hierarchical_)
        return key.level ? -1 : find({key.x, key.z});
    size_t at = tileHash(key) & (table_.size() - 1);
    while (table_[at] >= 0) {
        const auto i = table_[at];
        if (slots_[i].tile() == key)
            return i;
        at = (at + 1) & (table_.size() - 1);
    }
    return -1;
}
void TerrainStreamer::insert(uint32_t i) {
    size_t at = tileHash(slots_[i].tile()) & (table_.size() - 1);
    while (table_[at] >= 0)
        at = (at + 1) & (table_.size() - 1);
    table_[at] = int(i);
}
void TerrainStreamer::rebuildTable() {
    std::fill(table_.begin(), table_.end(), -1);
    for (uint32_t i = 0; i < capacity(); ++i)
        if (slots_[i].state != State::Empty && slots_[i].state != State::Retiring)
            insert(i);
}
double TerrainStreamer::tileDistance(TerrainTileKey t) const {
    const int64_t scale = int64_t(1) << t.level;
    // Relative integer subtraction precedes conversion, preserving distant
    // coordinates.
    const double x = double(t.x * scale - centre_.x) - .5;
    const double z = double(t.z * scale - centre_.z) - .5;
    return std::max({0.0, x, z, -x - double(scale), -z - double(scale)}) / double(scale);
}
void TerrainStreamer::requestTree(TerrainTileKey t) {
    const int64_t scale = int64_t(1) << t.level;
    const int64_t x = t.x * scale - centre_.x, z = t.z * scale - centre_.z;
    // Clip to the requested square plus one base chunk. Descendant rings are
    // bounded to six tiles per axis, independent of the number of base chunks.
    const int clip =
        (t.level == topLevel_ || (horizonRadius_ && t.level > detailedLevel_)) ? outerRadius_ : radius_;
    if (x > clip + 1 || z > clip + 1 || x + scale < -clip || z + scale < -clip)
        return;
    const double d = tileDistance(t);
    if (t.level < topLevel_ && d > 2)
        return;
    if (wanted_.size() == wanted_.capacity()) {
        ++pressureEvents_;
        return;
    }
    wanted_.push_back({t, t.level == topLevel_ || d <= 1});
    if (t.level)
        for (int q = 0; q < 4; ++q)
            requestTree({t.x * 2 + (q & 1), t.z * 2 + (q >> 1), t.level - 1});
}
void TerrainStreamer::demandHierarchy(const Camera &camera, int radius, uint64_t completed,
                                      uint64_t submitted, uint64_t epoch) {
    centre_ = camera.chunk;
    radius_ = std::clamp(radius, 0, maximumRadius_);
    outerRadius_ = horizonRadius_ ? horizonRadius_ : horizonSlots_ ? radius_ * 2 : radius_;
    wanted_.clear();
    const int64_t scale = int64_t(1) << topLevel_;
    const auto loX = terrainFloorDivide(centre_.x - outerRadius_ - 1, scale);
    const auto hiX = terrainFloorDivide(centre_.x + outerRadius_ + 1, scale);
    const auto loZ = terrainFloorDivide(centre_.z - outerRadius_ - 1, scale);
    const auto hiZ = terrainFloorDivide(centre_.z + outerRadius_ + 1, scale);
    for (auto z = loZ; z <= hiZ; ++z)
        for (auto x = loX; x <= hiX; ++x)
            requestTree({x, z, topLevel_});
    // Close refinement dependencies before activation. A child needs the
    // parents of its whole neighbour stencil fully refined, including parents
    // that would otherwise be prefetch-only or outside the visible square.
    // Retaining only active children's parents cannot bootstrap those tiles.
    for (uint32_t level = 0; level < topLevel_; ++level)
        for (size_t i = 0; i < wanted_.size(); ++i) {
            const auto child = wanted_[i];
            if (child.key.level != level || !child.refine)
                continue;
            const auto lo = terrainParent({child.key.x - 1, child.key.z - 1, level});
            const auto hi = terrainParent({child.key.x + 1, child.key.z + 1, level});
            for (auto z = lo.z; z <= hi.z; ++z)
                for (auto x = lo.x; x <= hi.x; ++x) {
                    const TerrainTileKey parent{x, z, level + 1};
                    const auto found = std::find_if(wanted_.begin(), wanted_.end(),
                                                    [&](const auto &w) { return w.key == parent; });
                    if (found != wanted_.end())
                        found->refine = true;
                    else if (wanted_.size() < wanted_.capacity())
                        wanted_.push_back({parent, true});
                    else
                        ++pressureEvents_;
                }
        }
    if (canonicalRadius_ > 0) {
        // Request missing providers near-first after closing rendered dependencies.
        // These are data readers, not new rendering patches or an unbounded fine
        // ring.
        const double extent = grid_.extent();
        const int reach = int(std::ceil(canonicalRadius_ / extent)) + 1;
        bool full = false;
        for (int ring = 0; ring <= reach && !full; ++ring)
            for (int z = -ring; z <= ring && !full; ++z)
                for (int x = -ring; x <= ring; ++x) {
                    if (std::max(std::abs(x), std::abs(z)) != ring)
                        continue;
                    const double loX = x * extent - camera.local.x, loZ = z * extent - camera.local.z;
                    const double dx = std::max({0., loX, -loX - extent});
                    const double dz = std::max({0., loZ, -loZ - extent});
                    if (dx * dx + dz * dz > canonicalRadius_ * canonicalRadius_)
                        continue;
                    const TerrainTileKey key{centre_.x + x, centre_.z + z, 0};
                    if (std::any_of(wanted_.begin(), wanted_.end(),
                                    [&](const auto &w) { return w.key == key; }))
                        continue;
                    if (wanted_.size() == wanted_.capacity()) {
                        ++pressureEvents_;
                        full = true;
                        break;
                    }
                    wanted_.push_back({key, false});
                }
    }
    for (auto &s : slots_) {
        if (s.state == State::Retiring && s.retireAfter <= completed) {
            auto generation = s.generation;
            s = {};
            s.generation = generation;
        }
        s.wanted = s.refine = false;
        if (s.state != State::Empty && s.state != State::Retiring && s.epoch != epoch) {
            s.state = State::Retiring;
            s.retireAfter = std::max(submitted, s.lastUse);
            ++s.generation;
            s.active = s.visible = false;
            s.heightFade = 0;
        }
    }
    rebuildTable();
    // Ancestors first so pressure cannot fill the pool with unusable descendants.
    for (int level = int(topLevel_); level >= 0; --level)
        for (const auto &w : wanted_) {
            if (w.key.level != uint32_t(level))
                continue;
            const int64_t span = int64_t(1) << w.key.level;
            const int64_t x = w.key.x * span - centre_.x, z = w.key.z * span - centre_.z;
            const bool horizon = horizonRadius_
                                     ? w.key.level > detailedLevel_
                                     : horizonSlots_ && (x > radius_ + 1 || z > radius_ + 1 ||
                                                         x + span < -radius_ || z + span < -radius_);
            int at = findTile(w.key);
            if (at < 0) {
                for (uint32_t i = horizon ? baseCapacity_ : 0;
                     i < (horizon ? cameraCapacity_ : baseCapacity_); ++i)
                    if (slots_[i].state == State::Empty) {
                        auto &s = slots_[i];
                        s.chunk = {w.key.x, w.key.z};
                        s.level = w.key.level;
                        s.epoch = epoch;
                        s.state = State::Requested;
                        insert(i);
                        at = int(i);
                        break;
                    }
            }
            if (at < 0) {
                ++pressureEvents_;
                continue;
            }
            auto &s = slots_[at];
            s.wanted = true;
            s.shadowOnly = false;
            s.horizonOnly = horizon;
            s.refine = w.refine;
            s.visible = w.refine;
        }
    for (uint32_t n = 0; n < shadowCount_; ++n) {
        const auto key = shadowWanted_[n];
        int at = findTile(key);
        // Offscreen demand must not retain camera slots after camera demand has
        // left them. Let normal retirement complete, then request an extra slot.
        if (at >= 0 && uint32_t(at) < cameraCapacity_ && !slots_[at].wanted)
            continue;
        if (at >= 0 && !slots_[at].wanted)
            slots_[at].shadowOnly = true;
        if (at < 0)
            for (uint32_t i = cameraCapacity_; i < capacity(); ++i)
                if (slots_[i].state == State::Empty) {
                    auto &s = slots_[i];
                    s.chunk = {key.x, key.z};
                    s.level = key.level;
                    s.epoch = epoch;
                    s.state = State::Requested;
                    s.shadowOnly = true;
                    insert(i);
                    at = int(i);
                    break;
                }
        if (at >= 0)
            slots_[at].wanted = true;
        else
            ++pressureEvents_;
    }
    // Retain the complete parent neighbourhood used by shared edge/corner
    // transitions, including children fading out after movement.
    for (uint32_t level = 0; level < topLevel_; ++level)
        for (const auto &s : slots_)
            if (s.level == level && (s.wanted || s.active || s.heightFade > 0))
                for (int z = -1; z <= 1; ++z)
                    for (int x = -1; x <= 1; ++x) {
                        const auto parent = findTile(terrainParent({s.chunk.x + x, s.chunk.z + z, s.level}));
                        if (parent >= 0)
                            slots_[parent].wanted = true;
                    }
    for (auto &s : slots_) {
        if (s.state == State::Empty || s.state == State::Retiring || s.wanted || s.active || s.heightFade > 0)
            continue;
        s.state = State::Retiring;
        s.retireAfter = std::max(s.lastUse, submitted);
        ++s.generation;
        s.visible = false;
    }
    rebuildTable();
}
bool TerrainStreamer::parentReady(TerrainTileKey t) const {
    if (t.level == topLevel_)
        return true;
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x) {
            const auto p = terrainParent({t.x + x, t.z + z, t.level});
            const int i = findTile(p);
            if (i < 0 || slots_[i].state != State::Resident || !slots_[i].active || slots_[i].heightFade < 1)
                return false;
        }
    return true;
}
void TerrainStreamer::advance(float dt) {
    if (!hierarchical_)
        return;
    // An active child's entire parent neighbourhood must stay unmorphed until
    // that child has fully collapsed. Pinning only its own parent lets an
    // adjacent parent start collapsing underneath the shared boundary.
    for (auto &s : slots_)
        s.support = false;
    for (const auto &s : slots_)
        if (s.active && s.level < topLevel_)
            for (int z = -1; z <= 1; ++z)
                for (int x = -1; x <= 1; ++x) {
                    const auto parent = findTile(terrainParent({s.chunk.x + x, s.chunk.z + z, s.level}));
                    if (parent >= 0)
                        slots_[parent].support = true;
                }
    // Parents settle before children enter; children leave before parents do.
    for (int level = int(topLevel_); level >= 0; --level)
        for (auto &s : slots_) {
            if (s.level != uint32_t(level) || s.state != State::Resident)
                continue;
            if (s.shadowOnly) {
                s.active = s.visible = s.refine = false;
                s.heightFade = 0;
                continue;
            }
            bool children = false;
            if (s.level)
                for (int q = 0; q < 4; ++q) {
                    int c = findTile({s.chunk.x * 2 + (q & 1), s.chunk.z * 2 + (q >> 1), s.level - 1});
                    children |= c >= 0 && slots_[c].active;
                }
            bool target = (s.refine && parentReady(s.tile())) || children || s.support;
            s.heightFade = std::clamp(s.heightFade + (target ? dt : -dt) * 4.f, 0.f, 1.f);
            if (s.level == topLevel_ && s.wanted)
                s.heightFade = 1;
            // Readiness is a latch: cancellation before readiness cannot make a
            // child visible, and re-entering demand must not hide an active child.
            s.active = s.heightFade == 1 || (s.active && s.heightFade > 0);
            s.drawMask = 15;
            s.visible = s.active;
        }
    for (const auto &s : slots_)
        if (s.active && s.level < topLevel_) {
            const int p = findTile(terrainParent(s.tile()));
            if (p >= 0) {
                const auto q = uint32_t(s.chunk.x - terrainFloorDivide(s.chunk.x, 2) * 2) +
                               uint32_t(s.chunk.z - terrainFloorDivide(s.chunk.z, 2) * 2) * 2;
                slots_[p].drawMask &= ~(1u << q);
            }
        }
}
void TerrainStreamer::heightWeights(uint32_t index, float out[16]) const {
    const auto &s = slots_[index];
    for (int z = 0; z < 4; ++z)
        for (int x = 0; x < 4; ++x) {
            float f = s.heightFade;
            const int dx = x == 0 ? -1 : x == 3 ? 1 : 0, dz = z == 0 ? -1 : z == 3 ? 1 : 0;
            for (int oz = std::min(0, dz); oz <= std::max(0, dz); ++oz)
                for (int ox = std::min(0, dx); ox <= std::max(0, dx); ++ox) {
                    const int n = findTile({s.chunk.x + ox, s.chunk.z + oz, s.level});
                    f = std::min(f, n >= 0 && slots_[n].active ? slots_[n].heightFade : 0.f);
                }
            out[z * 4 + x] = s.level == topLevel_ ? 1.f : f;
        }
}
glm::vec3 TerrainStreamer::offset(uint32_t i, const Camera &camera) const {
    const auto &s = slots_[i];
    const auto scale = int64_t(1) << s.level;
    return camera.terrainOffset({s.chunk.x * scale, s.chunk.z * scale}, grid_);
}
} // namespace ow

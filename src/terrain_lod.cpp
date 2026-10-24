#include "terrain_lod.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ow {
float terrainHierarchyRange(float scale) {
    return terrainHierarchyFirstRange * std::clamp(scale, 1.f, terrainLodMaximumScale);
}
float terrainDetailRangeScale(float committed, bool quality, bool frozen, float seconds) {
    if (!quality)
        return 1.f;
    const float ahead = frozen ? 0.f : std::clamp(seconds, 0.f, .1f) * terrainLodScaleSlew;
    return std::clamp(committed + ahead, 1.f, terrainLodMaximumScale);
}

namespace {
bool powerOfTwo(uint32_t n) { return n && !(n & (n - 1)); }
uint32_t bits(uint32_t n) {
    uint32_t c = 0;
    for (; n; n >>= 1)
        c += n & 1;
    return c;
}
float distanceToBox(glm::vec3 p, glm::vec3 lo, glm::vec3 hi) {
    return glm::length(glm::max(glm::max(lo - p, p - hi), glm::vec3(0)));
}
} // namespace
void validateLodGrid(TerrainGrid grid, uint32_t patchCells) {
    validateGrid(grid);
    if (!powerOfTwo(grid.cells) || !powerOfTwo(patchCells) || patchCells < 8 || patchCells > grid.cells)
        throw std::invalid_argument("LOD requires power-of-two cells and patches, with 8 <= patch <= grid");
}
TerrainPatchTopology terrainPatchTopology(uint32_t cells) {
    validateLodGrid({cells, 0.5f}, cells);
    TerrainPatchTopology t;
    t.indices.reserve(size_t(cells) * cells * 6 * 8);
    for (uint32_t mask = 0; mask < 16; ++mask) {
        auto &r = t.ranges[mask];
        r.first = uint32_t(t.indices.size());
        for (uint32_t z = 0; z < cells; ++z)
            for (uint32_t x = 0; x < cells; ++x) {
                const uint32_t q = (x >= cells / 2 ? 1 : 0) + (z >= cells / 2 ? 2 : 0);
                if (!(mask & (1u << q)))
                    continue;
                const uint32_t a = z * (cells + 1) + x;
                // Parent-triangle morphing and endpoint coalescing use this diagonal.
                for (auto i : {a, a + cells + 1, a + cells + 2, a, a + cells + 2, a + 1})
                    t.indices.push_back(i);
            }
        r.count = uint32_t(t.indices.size()) - r.first;
    }
    const uint32_t half = cells / 2;
    for (uint32_t mask = 0; mask < 16; ++mask) {
        auto &r = t.stitched[mask];
        r.first = uint32_t(t.indices.size());
        const auto vertex = [&](uint32_t x, uint32_t z) {
            if ((x == 0 && (mask & 1)) || (x == half && (mask & 2)))
                z &= ~1u;
            if ((z == 0 && (mask & 4)) || (z == half && (mask & 8)))
                x &= ~1u;
            return z * (half + 1) + x;
        };
        for (uint32_t z = 0; z < half; ++z)
            for (uint32_t x = 0; x < half; ++x) {
                const uint32_t a = vertex(x, z), b = vertex(x + 1, z), c = vertex(x, z + 1),
                               d = vertex(x + 1, z + 1);
                for (auto tri : {std::array<uint32_t, 3>{a, c, d}, std::array<uint32_t, 3>{a, d, b}})
                    if (tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2])
                        t.indices.insert(t.indices.end(), tri.begin(), tri.end());
            }
        r.count = uint32_t(t.indices.size()) - r.first;
    }
    return t;
}
float terrainMorph(float distance, float range) {
    return std::clamp((distance / range - 0.75f) * 4.0f, 0.0f, 1.0f);
}
TerrainLod::TerrainLod(TerrainGrid grid, uint32_t capacity, TerrainLodConfig config, uint32_t chunkCapacity)
    : grid_(grid), config_(config), capacity_(capacity), chunkCapacity_(chunkCapacity),
      requiredRanges_(chunkCapacity) {
    config_.patchCells = std::min(config_.patchCells, capacity);
    validateLodGrid({capacity, grid.spacing}, patchCells());
    if (!std::isfinite(config_.firstRange) || config_.firstRange <= 0)
        throw std::invalid_argument("Invalid LOD range");
    const size_t leaves = size_t(capacity / patchCells()) * (capacity / patchCells());
    nodes_.reserve((leaves * 4 - 1) / 3);
    bounds_.reserve(nodes_.capacity() * chunkCapacity_);
    errors_.reserve(nodes_.capacity() * chunkCapacity_);
    metadata_.reserve(nodes_.capacity() * chunkCapacity_);
    draws_.reserve(nodes_.capacity() * chunkCapacity_);
    drawBounds_.reserve(draws_.capacity());
    casterDraws_.reserve(draws_.capacity() * 2);
    casterBounds_.reserve(casterDraws_.capacity());
    setGrid(grid);
}
uint32_t TerrainLod::build(uint32_t x, uint32_t z, uint32_t step, uint32_t level) {
    uint32_t i = uint32_t(nodes_.size());
    nodes_.push_back({x, z, step, level, {}});
    if (level)
        for (uint32_t q = 0; q < 4; ++q) {
            const auto half = step * patchCells() / 2;
            const uint32_t child = build(x + (q & 1) * half, z + (q >> 1) * half, step / 2, level - 1);
            nodes_[i].children[q] = child;
        }
    return i;
}
void TerrainLod::setGrid(TerrainGrid grid) {
    validateLodGrid(grid, patchCells());
    if (grid.cells > capacity_)
        throw std::invalid_argument("LOD grid exceeds capacity");
    grid_ = grid;
    nodes_.clear();
    uint32_t step = grid.cells / patchCells(), level = 0;
    for (auto n = step; n > 1; n >>= 1)
        ++level;
    build(0, 0, step, level);
    bounds_.resize(nodes_.size() * chunkCapacity_);
    errors_.resize(nodes_.size() * chunkCapacity_);
    metadata_.resize(nodes_.size() * chunkCapacity_);
    std::fill(requiredRanges_.begin(), requiredRanges_.end(), config_.firstRange);
}
void TerrainLod::setBounds(uint32_t slot, const glm::vec2 *leaf) {
    if (slot >= chunkCapacity_)
        throw std::out_of_range("LOD bounds slot");
    requiredRanges_[slot] = config_.firstRange;
    const uint32_t width = grid_.cells / patchCells();
    for (size_t i = nodes_.size(); i--;) {
        auto &b = bounds_[i * chunkCapacity_ + slot];
        const auto &n = nodes_[i];
        if (!n.level)
            b = leaf[(n.z / patchCells()) * width + n.x / patchCells()];
        else {
            b = {INFINITY, -INFINITY};
            for (auto c : n.children) {
                b.x = std::min(b.x, bounds_[c * chunkCapacity_ + slot].x);
                b.y = std::max(b.y, bounds_[c * chunkCapacity_ + slot].y);
            }
        }
        const float side = n.step * patchCells() * grid_.spacing;
        requiredRanges_[slot] = std::max(
            requiredRanges_[slot], 2.01f * glm::length(glm::vec3(side, b.y - b.x, side)) / float(n.step));
    }
}
void TerrainLod::setQualityMetadata(uint32_t slot, const void *levels) {
    for (size_t i = nodes_.size(); i--;) {
        const auto &n = nodes_[i];
        uint32_t offset = 0, width = grid_.cells / patchCells();
        for (uint32_t l = 0; l < n.level; ++l) {
            offset += width * width;
            width >>= 1;
        }
        TerrainPatchMetadata v;
        const auto index = offset + (n.z / (patchCells() * n.step)) * width + n.x / (patchCells() * n.step);
        std::memcpy(&v, static_cast<const uint8_t *>(levels) + index * sizeof(v), sizeof(v));
        auto &b = bounds_[i * chunkCapacity_ + slot];
        b = {std::min(b.x, v.heights.x), std::max(b.y, v.heights.y)};
        metadata_[i * chunkCapacity_ + slot] = v;
        auto &e = errors_[i * chunkCapacity_ + slot];
        e = v.error;
        if (n.level)
            for (auto child : n.children) {
                e = glm::max(e, errors_[child * chunkCapacity_ + slot]);
                const auto cb = bounds_[child * chunkCapacity_ + slot];
                b = {std::min(b.x, cb.x), std::max(b.y, cb.y)};
            }
    }
}
TerrainPatchMetadata TerrainLod::metadata(uint32_t slot, uint32_t x, uint32_t z, uint32_t level) const {
    uint32_t i = 0;
    while (nodes_[i].level > level) {
        const auto &n = nodes_[i];
        const auto half = n.step * patchCells() / 2;
        i = n.children[(x >= n.x + half ? 1u : 0u) | (z >= n.z + half ? 2u : 0u)];
    }
    return metadata_[i * chunkCapacity_ + slot];
}
float TerrainLod::projectedError(const TerrainPatchDraw &p, ChunkCoord origin, uint32_t level,
                                 const Camera &observer, float focalPixels, bool eroded) const {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const auto &n = nodes_[i];
        if (n.x != p.x || n.z != p.z || n.level != p.level)
            continue;
        const auto b = bounds_[i * chunkCapacity_ + p.slot];
        const float spacing = grid_.spacing * float(1u << level);
        const auto offset = observer.terrainOffset(origin, grid_);
        glm::vec3 lo = offset + glm::vec3(n.x * spacing, b.x, n.z * spacing);
        glm::vec3 hi = offset + glm::vec3((n.x + patchCells() * n.step) * spacing, b.y,
                                          (n.z + patchCells() * n.step) * spacing);
        curveBounds(lo, hi, observer.curvature);
        const auto forward = observer.forward();
        const glm::vec3 nearCorner{forward.x >= 0 ? lo.x : hi.x, forward.y >= 0 ? lo.y : hi.y,
                                   forward.z >= 0 ? lo.z : hi.z};
        return errors_[i * chunkCapacity_ + p.slot][eroded ? 1 : 0] * focalPixels /
               std::max(.5f, glm::dot(nearCorner, forward));
    }
    return 0;
}
void TerrainLod::box(uint32_t i, uint32_t slot, glm::vec3 offset, glm::vec3 &lo, glm::vec3 &hi) const {
    const auto &n = nodes_[i];
    auto b = bounds_[i * chunkCapacity_ + slot];
    // Metadata encloses this patch and its parent-target samples. Whole-tile
    // envelopes unnecessarily prevent ridge occlusion and frustum rejection.
    lo = offset + glm::vec3(n.x * grid_.spacing, b.x, n.z * grid_.spacing);
    hi = offset + glm::vec3((n.x + n.step * patchCells()) * grid_.spacing, b.y,
                            (n.z + n.step * patchCells()) * grid_.spacing);
}
bool TerrainLod::visible(glm::vec3 lo, glm::vec3 hi) const {
    curveBounds(lo, hi, hierarchyCamera_.curvature);
    for (auto p : planes_) {
        const glm::vec3 v(p.x >= 0 ? hi.x : lo.x, p.y >= 0 ? hi.y : lo.y, p.z >= 0 ? hi.z : lo.z);
        if (glm::dot(glm::vec3(p), v) + p.w < -0.01f)
            return false;
    }
    return true;
}
void TerrainLod::visit(uint32_t i, uint32_t slot, glm::vec3 offset, glm::vec3 observer, bool lod, bool cull,
                       uint32_t allowed) {
    const auto &n = nodes_[i];
    glm::vec3 lo, hi;
    box(i, slot, offset, lo, hi);
    if (hierarchical_ && !exterior_ && (hi.x < clip_.x || lo.x > clip_.z || hi.z < clip_.y || lo.z > clip_.w))
        return;
    const bool inView = !exterior_ && (!cull || visible(lo, hi));
    if (!inView && !collectCasters_) {
        ++stats_.culled;
        return;
    }
    uint32_t mask = allowed, viewMask = inView ? allowed : 0;
    if (n.level > minimumLevel_)
        for (uint32_t q = 0; q < 4; ++q) {
            if (!(allowed & (1u << q)))
                continue;
            const auto child = n.children[q];
            box(child, slot, offset, lo, hi);
            auto selectionLo = lo, selectionHi = hi;
            if (hierarchical_)
                selectionLo.y = selectionHi.y = observer.y;
            if (!lod ||
                distanceToBox(observer, selectionLo, selectionHi) <= stats_.firstRange * float(n.step / 2)) {
                visit(child, slot, offset, observer, lod, cull);
                mask &= ~(1u << q);
                viewMask &= ~(1u << q);
            } else if (cull && !visible(lo, hi)) {
                viewMask &= ~(1u << q);
                if (!exterior_)
                    ++stats_.culled;
            }
        }
    box(i, slot, offset, lo, hi);
    curveBounds(lo, hi, hierarchyCamera_.curvature);
    const VisibilityBounds bounds{lo - glm::vec3(.01f), hi + glm::vec3(.01f), stats_.boundsReady};
    if (mask && collectCasters_) {
        casterDraws_.push_back({slot, n.x, n.z, n.step, n.level, mask, exterior_});
        casterBounds_.push_back(bounds);
    }
    if (viewMask) {
        draws_.push_back({slot, n.x, n.z, n.step, n.level, viewMask});
        drawBounds_.push_back(bounds);
        ++stats_.selected;
        ++stats_.perLevel[std::min(n.level + tileLevel_, maxTerrainLodLevels - 1)];
        const auto triangles = uint64_t(patchCells()) * patchCells() / 2 * bits(viewMask);
        stats_.triangles += triangles;
        stats_.trianglesPerLevel[std::min(n.level + tileLevel_, maxTerrainLodLevels - 1)] += triangles;
    }
}
void TerrainLod::selectExteriorTile(uint32_t slot, ChunkCoord origin, uint32_t level) {
    if (!collectCasters_)
        return;
    exterior_ = true;
    selectTile(slot, origin, level, 15, true, false);
    exterior_ = false;
}

void TerrainLod::select(const std::array<ChunkCoord, maxTerrainChunks> &chunks, uint32_t count,
                        const Camera &camera, const Camera &observer, const glm::mat4 &vp, bool enabled,
                        bool culling, bool ready) {
    select(chunks.data(), nullptr, count, camera, observer, vp, enabled, culling, ready);
}
void TerrainLod::select(const ChunkCoord *chunks, const uint32_t *slots, uint32_t count, const Camera &camera,
                        const Camera &observer, const glm::mat4 &vp, bool enabled, bool culling, bool ready,
                        float rangeOverride) {
    hierarchical_ = false;
    hierarchyCamera_ = camera;
    tileLevel_ = minimumLevel_ = 0;
    if (count > chunkCapacity_)
        throw std::invalid_argument("LOD chunk count");
    draws_.clear();
    casterDraws_.clear();
    casterBounds_.clear();
    drawBounds_.clear();
    stats_ = {};
    stats_.boundsReady = ready;
    stats_.firstRange = std::max(config_.firstRange, rangeOverride);
    if (ready)
        for (uint32_t s = 0; s < count; ++s)
            stats_.firstRange = std::max(stats_.firstRange, requiredRanges_[slots ? slots[s] : s]);
    const auto m = glm::transpose(vp);
    planes_ = {m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2]};
    const glm::vec3 eye = camera.terrainOffset(observer.chunk, grid_) + glm::vec3(observer.local);
    for (uint32_t s = 0; s < count; ++s)
        visit(0, slots ? slots[s] : s, camera.terrainOffset(chunks[s], grid_), eye, enabled && ready,
              culling && ready);
}

void TerrainLod::beginHierarchy(const Camera &camera, const Camera &observer, const glm::mat4 &vp, int radius,
                                float range) {
    hierarchical_ = true;
    hierarchyCamera_ = camera;
    hierarchyEye_ = camera.terrainOffset(observer.chunk, grid_) + glm::vec3(observer.local);
    draws_.clear();
    casterDraws_.clear();
    casterBounds_.clear();
    drawBounds_.clear();
    stats_ = {};
    stats_.boundsReady = true;
    hierarchyRange_ = range;
    stats_.firstRange = hierarchyRange_;
    const auto m = glm::transpose(vp);
    planes_ = {m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2]};
    const float e = float(grid_.extent());
    clip_ = {-radius * e - float(camera.local.x), -radius * e - float(camera.local.z),
             (radius + 1) * e - float(camera.local.x), (radius + 1) * e - float(camera.local.z)};
}
void TerrainLod::selectTile(uint32_t slot, ChunkCoord origin, uint32_t level, uint32_t mask, bool enabled,
                            bool culling, uint32_t minimumLevel) {
    if (!mask)
        return;
    const auto offset = hierarchyCamera_.terrainOffset(origin, grid_);
    const float spacing = grid_.spacing;
    grid_.spacing *= float(1u << level);
    stats_.firstRange = hierarchyRange_ * float(1u << level);
    tileLevel_ = level;
    minimumLevel_ = minimumLevel;
    visit(0, slot, offset, hierarchyEye_, enabled, culling, mask);
    grid_.spacing = spacing;
    stats_.firstRange = hierarchyRange_;
}
} // namespace ow

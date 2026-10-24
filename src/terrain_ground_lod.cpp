#include "terrain_ground_lod.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>
namespace ow {
namespace {
uint64_t hash(TerrainPatchKey k) {
    auto mix = [](uint64_t v) {
        v ^= v >> 30;
        v *= 0xbf58476d1ce4e5b9ULL;
        v ^= v >> 27;
        v *= 0x94d049bb133111ebULL;
        return v ^ (v >> 31);
    };
    return mix(uint64_t(k.x)) ^ mix(uint64_t(k.z) + 0x9e3779b97f4a7c15ULL) ^ mix(k.level);
}
int64_t floorDiv(int64_t n, int64_t d) { return n / d - (n % d < 0); }
float boxDistance(glm::vec3 lo, glm::vec3 hi) {
    return glm::length(glm::max(glm::max(lo, -hi), glm::vec3(0)));
}
} // namespace
void TerrainGroundLod::Table::reset() {
    maximumLevel = 0;
    if (++generation == 0) {
        for (auto &e : entries)
            e.generation = 0;
        generation = 1;
    }
}
void TerrainGroundLod::Table::put(TerrainPatchKey key, uint32_t index) {
    maximumLevel = std::max(maximumLevel, key.level);
    size_t p = hash(key) & (entries.size() - 1);
    while (entries[p].generation == generation && !(entries[p].key == key))
        p = (p + 1) & (entries.size() - 1);
    entries[p] = {key, index, generation};
}
uint32_t TerrainGroundLod::Table::get(TerrainPatchKey key) const {
    size_t p = hash(key) & (entries.size() - 1);
    while (entries[p].generation == generation) {
        if (entries[p].key == key)
            return entries[p].index;
        p = (p + 1) & (entries.size() - 1);
    }
    return ~0u;
}
int64_t TerrainGroundLod::side(uint32_t cells, uint32_t level) { return int64_t(cells) << level; }
TerrainGroundLod::TerrainGroundLod(TerrainGrid grid, uint32_t patch, uint32_t tiles, uint32_t draws,
                                   uint64_t budget)
    : grid_(grid), cells_(patch / 2), rootLevel_(0), budget_(budget) {
    validateLodGrid(grid, patch);
    for (auto n = grid.cells / patch; n > 1; n >>= 1)
        ++rootLevel_;
    const uint64_t triangles = uint64_t(cells_) * cells_ * 2;
    if (budget < triangles * 16 || budget > 33554432)
        throw std::invalid_argument("Terrain triangle budget outside supported range");
    // Coverage is mandatory even if a deliberately tiny budget cannot hold all roots.
    const uint64_t fullLeaves = uint64_t(tiles) * (grid.cells / cells_) * (grid.cells / cells_);
    leafCapacity_ =
        std::min<uint64_t>(draws, std::max<uint64_t>(tiles * 4, std::min(budget / triangles, fullLeaves)));
    if (leafCapacity_ < tiles * 4)
        throw std::invalid_argument("Visibility capacity cannot hold terrain coverage roots");
    // Every split adds four nodes and three leaves. Include removed parents in
    // the table's bound, keeping its worst-case load below one half.
    const auto capacity = (leafCapacity_ * 4) / 3 + 1;
    nodes_.reserve(capacity);
    heap_.reserve(capacity);
    closure_.reserve(capacity);
    previousKeys_.reserve(leafCapacity_);
    draws_.reserve(leafCapacity_);
    bounds_.reserve(leafCapacity_);
    casters_.reserve(leafCapacity_);
    casterBounds_.reserve(leafCapacity_);
    size_t table = 1;
    while (table < capacity * 2)
        table *= 2;
    current_.entries.resize(table);
    previous_.entries.resize(table);
}
void TerrainGroundLod::invalidate() {
    previousKeys_.clear();
    previous_.reset();
    restored_ = false;
}
void TerrainGroundLod::begin(const Camera &camera, const Camera &observer, const glm::mat4 &vp, float focal,
                             bool quality, float target, bool freeze, bool cull, bool eroded, bool enabled) {
    camera_ = camera;
    observer_ = observer;
    focal_ = focal;
    quality_ = quality;
    target_ = target;
    freeze_ = freeze;
    culling_ = cull;
    eroded_ = eroded;
    enabled_ = enabled;
    nodes_.clear();
    heap_.clear();
    closure_.clear();
    draws_.clear();
    bounds_.clear();
    casters_.clear();
    casterBounds_.clear();
    current_.reset();
    leafCount_ = 0;
    stats_ = {};
    stats_.boundsReady = true;
    stats_.firstRange = 64;
    stats_.qualityScale = 1;
    stats_.triangleBudget = budget_;
    const auto m = glm::transpose(vp);
    const float aspect = glm::length(glm::vec3(m[1])) / glm::length(glm::vec3(m[0]));
    projectionScale_ = focal_ * (1 + (1 + aspect * aspect) / 3.f) * 1.01f;
    planes_ = {m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2]};
}
void TerrainGroundLod::tile(uint32_t slot, ChunkCoord origin, uint32_t level, uint32_t mask,
                            uint32_t minimum) {
    for (uint32_t q = 0; q < 4; ++q)
        if (mask & (1u << q)) {
            Node n;
            n.slot = slot;
            n.x = (q & 1) * grid_.cells / 2;
            n.z = (q >> 1) * grid_.cells / 2;
            n.level = rootLevel_;
            n.minimum = minimum;
            n.key = {origin.x * int64_t(grid_.cells) + (int64_t(n.x) << level),
                     origin.z * int64_t(grid_.cells) + (int64_t(n.z) << level), level + rootLevel_};
            current_.put(n.key, uint32_t(nodes_.size()));
            nodes_.push_back(n);
            ++leafCount_;
        }
}
uint32_t TerrainGroundLod::covering(const Table &table, int64_t x, int64_t z, uint32_t hint) const {
    const auto at = [&](uint32_t level) {
        auto s = side(cells_, level);
        // Power-of-two cell sizes: bit alignment also floors negative world
        // coordinates, avoiding integer division in the neighbour lookup loop.
        return table.get({x & -s, z & -s, level});
    };
    hint = std::min(hint, table.maximumLevel);
    if (const auto i = at(hint); i != ~0u)
        return i;
    // Most neighbours and prior leaves have the same or an adjacent level.
    // Keep the complete search for missing/unbalanced residency and camera cuts.
    const auto extent = std::max(hint, table.maximumLevel - hint);
    for (uint32_t distance = 1; distance <= extent; ++distance) {
        if (distance <= hint)
            if (const auto i = at(hint - distance); i != ~0u)
                return i;
        if (hint + distance <= table.maximumLevel)
            if (const auto i = at(hint + distance); i != ~0u)
                return i;
    }
    return ~0u;
}
void TerrainGroundLod::neighbours(const Node &n, std::array<uint32_t, 8> &out) const {
    const auto s = side(cells_, n.key.level), x = n.key.x, z = n.key.z;
    const auto at = [&](int64_t px, int64_t pz) { return covering(current_, px, pz, n.key.level); };
    out = {at(x - 1, z + s / 2), at(x + s, z + s / 2), at(x + s / 2, z - 1), at(x + s / 2, z + s),
           at(x - 1, z - 1), at(x + s, z - 1), at(x - 1, z + s), at(x + s, z + s)};
}
void TerrainGroundLod::measure(uint32_t index, const TerrainLod &source) {
    auto &n = nodes_[index];
    const auto m = source.metadata(n.slot, n.x, n.z, n.level);
    const double x =
        double(n.key.x - camera_.chunk.x * int64_t(grid_.cells)) * grid_.spacing - camera_.local.x;
    const double z =
        double(n.key.z - camera_.chunk.z * int64_t(grid_.cells)) * grid_.spacing - camera_.local.z;
    const float extent = float(side(cells_, n.key.level) * grid_.spacing);
    n.lo = {float(x), m.heights.x - float(camera_.local.y), float(z)};
    n.hi = n.lo + glm::vec3(extent, m.heights.y - m.heights.x, extent);
    const auto delta = camera_.terrainOffset(observer_.chunk, grid_) + glm::vec3(observer_.local);
    auto lo = n.lo - delta, hi = n.hi - delta;
    auto horizontalLo = lo, horizontalHi = hi;
    horizontalLo.y = horizontalHi.y = 0;
    n.distance = boxDistance(horizontalLo, horizontalHi);
    n.protectedGround = n.distance < 64;
    // Euclidean distance makes mandatory transition safety independent of yaw.
    // Perspective Jacobian bound over the full view cone, including its corners.
    const float projection = projectionScale_ / std::max(.5f, boxDistance(lo, hi));
    const float step = grid_.spacing * float(uint64_t(1) << n.key.level);
    const float curvatureError = camera_.curvature.z != 0 ? step * step / (4 * camera_.curvature.x) : 0;
    n.error =
        (std::max(m.error[eroded_ ? 1 : 0], m.parentError[eroded_ ? 1 : 0]) + curvatureError) * projection;
    n.transition = 0;
    if (n.level > n.minimum) {
        const auto child = source.metadata(n.slot, n.x, n.z, n.level - 1);
        n.transition = (child.displacement[eroded_ ? 1 : 0] + curvatureError) * projection;
    }
    const auto p =
        covering(previous_, n.key.x + side(cells_, n.key.level) / 2,
                 n.key.z + side(cells_, n.key.level) / 2, n.key.level);
    const bool refined = p != ~0u && previousKeys_[p].level < n.key.level;
    const bool transition = n.transition > (refined ? .5f : 1.f);
    const bool baseline = !enabled_ || n.protectedGround ||
                          n.distance < 64 * float(uint64_t(1) << (n.key.level ? n.key.level - 1 : 0));
    bool want = freeze_ && !previousKeys_.empty()
                    ? refined
                    : baseline || transition ||
                          (quality_ && n.error > target_ * (refined ? .5f : 1.f));
    n.priority = 0;
    if (want)
        // Strict classes prevent optional approximation quality from spending
        // the allowance needed to keep replacement changes small.
        n.priority = transition ? 1e8f + std::min(n.transition * 1024, 1e7f)
                     : baseline ? 1e6f + 1e5f / (1 + n.distance)
                                : std::clamp(n.error / target_, .001f, 1e5f);
    if (n.protectedGround)
        n.priority = 1e12f - n.distance;
    if (n.level <= n.minimum)
        n.priority = 0;
    curveBounds(n.lo, n.hi, camera_.curvature);
}
void TerrainGroundLod::enqueue(uint32_t i) {
    if (nodes_[i].priority <= 0)
        return;
    const auto less = [&](uint32_t a, uint32_t b) {
        return nodes_[a].priority != nodes_[b].priority ? nodes_[a].priority < nodes_[b].priority : a > b;
    };
    heap_.push_back(i);
    std::push_heap(heap_.begin(), heap_.end(), less);
}
bool TerrainGroundLod::gather(uint32_t index) {
    auto &n = nodes_[index];
    if (n.mark == mark_)
        return true;
    n.mark = mark_;
    if (n.child != ~0u)
        return true;
    if (n.level <= n.minimum)
        return false;
    std::array<uint32_t, 8> neighbours_;
    neighbours(n, neighbours_);
    for (auto next : neighbours_)
        if (next != ~0u && nodes_[next].key.level > n.key.level)
            if (!gather(next))
                return false;
    closure_.push_back(index);
    return true;
}
bool TerrainGroundLod::split(uint32_t index, const TerrainLod &source) {
    // Once even an unbalanced single split cannot fit, do not walk neighbour
    // closures for every remaining candidate merely to reject them afterwards.
    const uint64_t minimumLeaves = uint64_t(leafCount_) + 3;
    if (minimumLeaves > leafCapacity_ || nodes_.size() + 4 > nodes_.capacity() ||
        minimumLeaves * cells_ * cells_ * 2 > budget_) {
        ++stats_.budgetLimited;
        return false;
    }
    closure_.clear();
    if (++mark_ == 0) {
        for (auto &n : nodes_)
            n.mark = 0;
        mark_ = 1;
    }
    if (!gather(index)) {
        ++stats_.residencyLimited;
        return false;
    }
    const uint64_t next = uint64_t(leafCount_) + closure_.size() * 3;
    const uint64_t triangles = next * cells_ * cells_ * 2;
    if (next > leafCapacity_ || nodes_.size() + closure_.size() * 4 > nodes_.capacity() ||
        triangles > budget_) {
        ++stats_.budgetLimited;
        return false;
    }
    for (auto i : closure_) {
        const Node parent = nodes_[i];
        nodes_[i].child = uint32_t(nodes_.size());
        current_.put(parent.key, ~0u);
        for (uint32_t q = 0; q < 4; ++q) {
            Node n = parent;
            n.child = ~0u;
            n.mark = 0;
            --n.level;
            --n.key.level;
            const auto span = side(cells_, n.key.level);
            n.key.x += (q & 1) * span;
            n.key.z += (q >> 1) * span;
            n.x += (q & 1) * (cells_ << n.level);
            n.z += (q >> 1) * (cells_ << n.level);
            const auto j = uint32_t(nodes_.size());
            nodes_.push_back(n);
            current_.put(n.key, j);
            measure(j, source);
            enqueue(j);
        }
        leafCount_ += 3;
        ++stats_.refined;
    }
    return true;
}
bool TerrainGroundLod::visible(const Node &n) const {
    if (!culling_)
        return true;
    for (auto p : planes_) {
        const glm::vec3 v(p.x >= 0 ? n.hi.x : n.lo.x, p.y >= 0 ? n.hi.y : n.lo.y, p.z >= 0 ? n.hi.z : n.lo.z);
        if (glm::dot(glm::vec3(p), v) + p.w < -.01f)
            return false;
    }
    return true;
}
void TerrainGroundLod::finish(const TerrainLod &source, const TerrainPatchTopology &topology) {
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        measure(i, source);
        enqueue(i);
    }
    stats_.baselineTriangles = uint64_t(leafCount_) * cells_ * cells_ * 2;
    const auto less = [&](uint32_t a, uint32_t b) {
        return nodes_[a].priority != nodes_[b].priority ? nodes_[a].priority < nodes_[b].priority : a > b;
    };
    while (!heap_.empty()) {
        std::pop_heap(heap_.begin(), heap_.end(), less);
        auto i = heap_.back();
        heap_.pop_back();
        if (nodes_[i].child == ~0u)
            split(i, source);
    }
    bool retainRestore = restored_ && freeze_;
    if (retainRestore) {
        retainRestore = false;
        for (auto k : previousKeys_)
            if (current_.get(k) == ~0u) {
                retainRestore = true;
                break;
            }
    }
    if (!retainRestore) {
        previous_.reset();
        previousKeys_.clear();
        restored_ = false;
    }
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        const auto &n = nodes_[i];
        if (n.child != ~0u)
            continue;
        std::array<uint32_t, 8> adjacent;
        neighbours(n, adjacent);
        uint32_t edges = 0, corners = 0;
        for (uint32_t e = 0; e < 4; ++e)
            if (adjacent[e] != ~0u && nodes_[adjacent[e]].key.level > n.key.level)
                edges |= 1u << e;
        for (uint32_t c = 0; c < 4; ++c) {
            const auto a = adjacent[4 + c];
            if ((a != ~0u && nodes_[a].key.level > n.key.level) || (edges & (1u << (c & 1))) ||
                (edges & (4u << (c >> 1))))
                corners |= 1u << c;
        }
        // Record the partition before culling; turning the camera cannot move seams.
        if (!retainRestore) {
            previous_.put(n.key, uint32_t(previousKeys_.size()));
            previousKeys_.push_back(n.key);
        }
        TerrainPatchDraw p{n.slot, n.x, n.z, 1u << n.level, n.level, 15, false, cells_, edges, corners};
        VisibilityBounds b{n.lo - glm::vec3(.01f), n.hi + glm::vec3(.01f), true};
        casters_.push_back(p);
        casterBounds_.push_back(b);
        if (n.protectedGround) {
            ++stats_.protectedPatches;
            if (n.key.level)
                ++stats_.protectedMissing;
        }
        if (!visible(n)) {
            ++stats_.culled;
            continue;
        }
        draws_.push_back(p);
        bounds_.push_back(b);
        ++stats_.selected;
        const auto level = std::min(n.key.level, maxTerrainLodLevels - 1),
                   triangles = topology.stitched[edges].count / 3;
        ++stats_.perLevel[level];
        stats_.trianglesPerLevel[level] += triangles;
        stats_.triangles += triangles;
        stats_.maximumErrorPixels = std::max(stats_.maximumErrorPixels, n.error);
        stats_.maximumTransitionPixels = std::max(stats_.maximumTransitionPixels, n.transition);
        stats_.overTargetPatches += n.error > target_;
        if (n.key.level > n.level && (n.protectedGround || n.error > target_))
            ++stats_.residencyLimited;
    }
    stats_.qualityLimited = stats_.budgetLimited || stats_.residencyLimited || stats_.protectedMissing;
}
std::vector<TerrainPatchKey> TerrainGroundLod::snapshot() const { return previousKeys_; }
void TerrainGroundLod::restore(const std::vector<TerrainPatchKey> &keys) {
    if (keys.size() > leafCapacity_)
        throw std::invalid_argument("Review partition exceeds terrain capacity");
    // Explicit review loading may allocate. Validate transactionally before changing history.
    std::set<std::tuple<int64_t, int64_t, uint32_t>> unique;
    for (auto k : keys) {
        if (k.level >= 32 || std::abs(double(k.x)) >= 1e15 || std::abs(double(k.z)) >= 1e15 ||
            k.x % side(cells_, k.level) || k.z % side(cells_, k.level) ||
            !unique.emplace(k.x, k.z, k.level).second)
            throw std::invalid_argument("Invalid review partition key");
    }
    for (auto k : keys)
        for (uint32_t l = k.level + 1; l < 32; ++l) {
            const auto s = side(cells_, l);
            if (unique.count({floorDiv(k.x, s) * s, floorDiv(k.z, s) * s, l}))
                throw std::invalid_argument("Overlapping review partition keys");
        }
    previousKeys_ = keys;
    previous_.reset();
    restored_ = !keys.empty();
    for (uint32_t i = 0; i < keys.size(); ++i)
        previous_.put(keys[i], i);
}
uint64_t TerrainGroundLod::allocatedBytes() const {
    return nodes_.capacity() * sizeof(Node) + (heap_.capacity() + closure_.capacity()) * 4 +
           previousKeys_.capacity() * sizeof(TerrainPatchKey) +
           (current_.entries.capacity() + previous_.entries.capacity()) * sizeof(Entry) +
           (draws_.capacity() + casters_.capacity()) * sizeof(TerrainPatchDraw) +
           (bounds_.capacity() + casterBounds_.capacity()) * sizeof(VisibilityBounds);
}
} // namespace ow

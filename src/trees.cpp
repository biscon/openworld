#include "trees.hpp"
#include <cmath>
#include <limits>
namespace ow {
uint32_t treeHash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}
uint32_t treeCandidateHash(TreeCellKey k, uint32_t seed, uint32_t candidate) {
    uint32_t h = treeHash(seed ^ 0x54524545u);
    for (uint32_t w : {uint32_t(k.x), uint32_t(uint64_t(k.x) >> 32), uint32_t(k.z),
                       uint32_t(uint64_t(k.z) >> 32), candidate})
        h = treeHash(h ^ w);
    return h;
}
glm::vec2 treeCandidatePosition(TreeCellKey key, uint32_t seed, uint32_t candidate) {
    const uint32_t h = treeCandidateHash(key, seed, candidate);
    return glm::min((glm::vec2(candidate % 8, (candidate / 8) % 8) +
                     glm::vec2(float(treeHash(h + 1) >> 8), float(treeHash(h + 2) >> 8)) / 16777216.f) *
                        8.f,
                    glm::vec2(63.999996f));
}
namespace {
int64_t floorDiv(int64_t x, int64_t n) { return x / n - (x % n < 0); }
uint32_t keyHash(TreeCellKey k) { return treeCandidateHash(k, 0, 0); }
} // namespace
WorldPosition treeCellOrigin(TreeCellKey k) {
    return {floorDiv(k.x, 16),
            floorDiv(k.z, 16),
            {double((k.x % 16 + 16) % 16) * 64, 0, double((k.z % 16 + 16) % 16) * 64}};
}
TreeCellKey treeCellAt(WorldPosition p) {
    validate(p);
    constexpr int64_t limit = INT64_MAX / 64;
    if (p.x < -limit || p.x > limit || p.z < -limit || p.z > limit || std::abs(p.local.x) > 1048576 ||
        std::abs(p.local.z) > 1048576)
        throw std::invalid_argument("Tree world coordinate outside supported range");
    return {p.x * 16 + int64_t(std::floor(p.local.x / 64)), p.z * 16 + int64_t(std::floor(p.local.z / 64))};
}
void validate(const TreeSettings &s) {
    if (!std::isfinite(s.distance) || s.distance < 64 || s.distance > 8000 || !std::isfinite(s.density) ||
        s.density < 0 || s.density > 1 || !std::isfinite(s.coverage) || s.coverage < 0 || s.coverage > 1 ||
        !std::isfinite(s.wind) || s.wind < 0 || s.wind > 2 || !std::isfinite(s.time) || s.time < 0 ||
        s.time > 1000000 || !std::isfinite(s.treeline) || s.treeline <= 0 ||
        !std::isfinite(s.windDirection.x) || !std::isfinite(s.windDirection.y) || s.forceLod < -1 ||
        s.forceLod > 3)
        throw std::invalid_argument("Invalid tree settings");
}
void validate(const TreeStreamConfig &c) {
    if (!c.cellCapacity || c.cellCapacity > 65536 || !c.updatesPerFrame || c.updatesPerFrame > 32 ||
        !std::isfinite(c.maximumDistance) || c.maximumDistance < 64 || c.maximumDistance > 8000 ||
        c.budgetBytes < (1ull << 20) || c.budgetBytes > (2ull << 30))
        throw std::invalid_argument("Invalid tree streaming configuration");
}
TreeResidency::TreeResidency(TreeStreamConfig c) : config_(c) {
    validate(c);
    slots_.resize(c.cellCapacity);
    uint32_t size = 1;
    while (size < c.cellCapacity * 2)
        size *= 2;
    table_.resize(size, -1);
    const auto side = uint32_t(std::ceil((c.maximumDistance + 1024) / 64)) * 2 + 3;
    wanted_.reserve(side * side);
    ordered_.reserve(c.cellCapacity);
}
int TreeResidency::find(TreeCellKey key) const {
    uint32_t pos = keyHash(key) & (table_.size() - 1);
    while (table_[pos] >= 0) {
        if (slots_[table_[pos]].key == key)
            return table_[pos];
        pos = (pos + 1) & (table_.size() - 1);
    }
    return -1;
}
void TreeResidency::insert(uint32_t i) {
    uint32_t pos = keyHash(slots_[i].key) & (table_.size() - 1);
    while (table_[pos] >= 0)
        pos = (pos + 1) & (table_.size() - 1);
    table_[pos] = int32_t(i);
}
void TreeResidency::demand(WorldPosition camera, uint64_t revision, uint64_t completed, bool enabled,
                           float requestedRadius) {
    if (!std::isfinite(requestedRadius) || requestedRadius < 0 ||
        requestedRadius > (config_.maximumDistance + 1024))
        throw std::invalid_argument("Invalid live tree demand radius");
    const float demandRadius = requestedRadius > 0 ? requestedRadius : (config_.maximumDistance + 1024);
    const float retentionRadius = demandRadius + 128;
    if (initialized_ && !retiring_ && previousRevision_ == revision && previousEnabled_ == enabled &&
        previousRadius_ == demandRadius && camera.x == previousCamera_.x && camera.z == previousCamera_.z &&
        treeCellAt(camera) == treeCellAt(previousCamera_))
        return;
    previousCamera_ = camera;
    previousRadius_ = demandRadius;
    previousRevision_ = revision;
    previousEnabled_ = enabled;
    initialized_ = true;
    const auto centre = treeCellAt(camera);
    const auto local = relativePosition(camera, treeCellOrigin(centre));
    deferred_ = retiring_ = 0;
    wanted_.clear();
    ordered_.clear();
    std::fill(table_.begin(), table_.end(), -1);
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        auto &s = slots_[i];
        s.wanted = false;
        if (s.state == State::Active) {
            double dx = double(s.key.x - centre.x) * 64 + 32 - local.x;
            double dz = double(s.key.z - centre.z) * 64 + 32 - local.z;
            s.distanceSquared = float(dx * dx + dz * dz);
            if (!enabled || s.revision != revision || s.distanceSquared > retentionRadius * retentionRadius)
                s.state = State::Retiring;
        }
        if (s.state == State::Retiring && s.lastUse <= completed) {
            s.state = State::Empty;
            s.generated = false;
            if (++s.generation == 0)
                ++s.generation;
        }
        if (s.state == State::Active)
            insert(i);
        if (s.state == State::Retiring)
            ++retiring_;
    }
    if (!enabled)
        return;
    int radius = int(std::ceil(demandRadius / 64)) + 1;
    for (int z = -radius; z <= radius; ++z)
        for (int x = -radius; x <= radius; ++x) {
            double dx = std::max(0.0, std::abs(x * 64 + 32 - local.x) - 32);
            double dz = std::max(0.0, std::abs(z * 64 + 32 - local.z) - 32);
            if (dx * dx + dz * dz > demandRadius * demandRadius)
                continue;
            double cx = x * 64 + 32 - local.x, cz = z * 64 + 32 - local.z;
            wanted_.push_back({{centre.x + x, centre.z + z}, float(cx * cx + cz * cz)});
        }
    std::sort(wanted_.begin(), wanted_.end(), [](const Wanted &a, const Wanted &b) {
        return a.distance != b.distance ? a.distance < b.distance
               : a.key.z != b.key.z     ? a.key.z < b.key.z
                                        : a.key.x < b.key.x;
    });
    const uint32_t selected = uint32_t(std::min(wanted_.size(), slots_.size()));
    uint32_t missing = 0, available = 0;
    for (uint32_t j = 0; j < selected; ++j) {
        int i = find(wanted_[j].key);
        if (i >= 0)
            slots_[i].wanted = true;
        else
            ++missing;
    }
    for (const auto &s : slots_)
        available += s.state == State::Empty;
    if (missing > available) {
        // Drop retained/far demand before denying nearer cells. Submitted slots
        // remain retired until completed, even when the new demand is urgent.
        for (auto &s : slots_)
            if (s.state == State::Active && !s.wanted) {
                s.state = State::Retiring;
                ++retiring_;
                if (s.lastUse <= completed) {
                    s.state = State::Empty;
                    s.generated = false;
                    ++available;
                    --retiring_;
                    if (++s.generation == 0)
                        ++s.generation;
                }
            }
        std::fill(table_.begin(), table_.end(), -1);
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::Active)
                insert(i);
    }
    deferred_ = uint32_t(wanted_.size()) - selected;
    uint32_t free = 0;
    for (uint32_t j = 0; j < selected; ++j) {
        auto w = wanted_[j];
        int index = find(w.key);
        if (index < 0) {
            while (free < slots_.size() && slots_[free].state != State::Empty)
                ++free;
            if (free == slots_.size()) {
                ++deferred_;
                continue;
            }
            index = int(free++);
            auto &s = slots_[index];
            s.key = w.key;
            s.revision = revision;
            s.state = State::Active;
            s.generated = false;
            s.lastUse = 0;
            insert(uint32_t(index));
        }
        auto &s = slots_[index];
        s.wanted = true;
        s.distanceSquared = w.distance;
        ordered_.push_back(uint32_t(index));
    }
}
bool TreeResidency::valid(TreeCellHandle h) const {
    return h.index < slots_.size() && slots_[h.index].generation == h.generation &&
           slots_[h.index].state == State::Active;
}
void TreeResidency::used(uint32_t i, uint64_t serial) {
    slots_[i].lastUse = std::max(slots_[i].lastUse, serial);
}
} // namespace ow

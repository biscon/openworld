#include "vegetation_stream.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace ow {
namespace {
uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}
int64_t floorDiv(int64_t x, int64_t n) { return x / n - (x % n < 0); }
uint32_t keyHash(VegetationCellKey k) { return vegetationCandidateHash(k, 0, 0, 0); }
} // namespace
uint32_t populationUpdateAllowance(uint32_t pending, uint32_t required, double remaining, double cellCost) {
    if (!std::isfinite(remaining) || !std::isfinite(cellCost) || cellCost <= 0)
        throw std::invalid_argument("Invalid population budget");
    uint32_t timed = uint32_t(std::min(double(pending), std::max(0.0, remaining) / cellCost));
    if (remaining > 0 && pending && timed == 0)
        timed = 1;
    return std::min(pending, std::max(required, timed));
}
uint32_t vegetationCandidateHash(VegetationCellKey k, uint32_t seed, uint32_t species, uint32_t candidate) {
    uint32_t h = hash(seed ^ 0x56330001u);
    for (uint32_t w : {uint32_t(k.x), uint32_t(uint64_t(k.x) >> 32), uint32_t(k.z),
                       uint32_t(uint64_t(k.z) >> 32), species, candidate})
        h = hash(h ^ w);
    return h;
}
WorldPosition vegetationCellOrigin(VegetationCellKey k) {
    WorldPosition p;
    p.x = floorDiv(k.x, 256);
    p.z = floorDiv(k.z, 256);
    p.local = {double(k.x % 256 + (k.x % 256 < 0 ? 256 : 0)) * 4, 0,
               double(k.z % 256 + (k.z % 256 < 0 ? 256 : 0)) * 4};
    return p;
}
VegetationCellKey vegetationCellAt(WorldPosition p) {
    constexpr auto limit = std::numeric_limits<int64_t>::max() / 512;
    if (p.x < -limit || p.x > limit || p.z < -limit || p.z > limit || !std::isfinite(p.local.x) ||
        !std::isfinite(p.local.z) || std::abs(p.local.x) > 1048576 || std::abs(p.local.z) > 1048576)
        throw std::invalid_argument("Vegetation world position outside supported range");
    return {p.x * 256 + int64_t(std::floor(p.local.x / 4)), p.z * 256 + int64_t(std::floor(p.local.z / 4))};
}
VegetationHabitatInput vegetationHabitat(VegetationHabitat preset) {
    if (uint32_t(preset) > 2)
        throw std::invalid_argument("Invalid vegetation habitat");
    VegetationHabitatInput out;
    out.weights = glm::vec3(0);
    out.weights[uint32_t(preset)] = 1;
    out.revision = uint32_t(preset) + 1;
    return out;
}
VegetationResidency::VegetationResidency(VegetationStreamConfig c) : config_(c) {
    if (!c.cellCapacity || c.cellCapacity > vegetationCellLimit || !c.updatesPerFrame ||
        c.updatesPerFrame > 32 || !std::isfinite(c.demandRadius) || !std::isfinite(c.retentionRadius) ||
        c.demandRadius <= 0 || c.demandRadius > 216 || c.retentionRadius < c.demandRadius ||
        c.retentionRadius > 232)
        throw std::invalid_argument("Invalid vegetation streaming capacity/radius");
    slots_.resize(c.cellCapacity);
    uint32_t size = 1;
    while (size < c.cellCapacity * 2)
        size *= 2;
    table_.resize(size, -1);
    const auto side = uint32_t(std::ceil(c.demandRadius / 4)) * 2 + 3;
    wanted_.reserve(side * side);
    ordered_.reserve(c.cellCapacity);
}
int VegetationResidency::find(VegetationCellKey key) const {
    uint32_t pos = keyHash(key) & (table_.size() - 1);
    while (table_[pos] >= 0) {
        if (slots_[table_[pos]].key == key)
            return table_[pos];
        pos = (pos + 1) & (table_.size() - 1);
    }
    return -1;
}
void VegetationResidency::insert(uint32_t i) {
    uint32_t pos = keyHash(slots_[i].key) & (table_.size() - 1);
    while (table_[pos] >= 0)
        pos = (pos + 1) & (table_.size() - 1);
    table_[pos] = int32_t(i);
}
void VegetationResidency::demand(WorldPosition camera, uint64_t revision, uint64_t completed, bool enabled,
                                 float requestedRadius) {
    if (!std::isfinite(requestedRadius) || requestedRadius < 0 || requestedRadius > config_.demandRadius)
        throw std::invalid_argument("Invalid live vegetation demand radius");
    const float demandRadius = requestedRadius > 0 ? requestedRadius : config_.demandRadius;
    const float retentionRadius = demandRadius + config_.retentionRadius - config_.demandRadius;
    if (initialized_ && !retiring_ && previousRevision_ == revision && previousEnabled_ == enabled &&
        previousRadius_ == demandRadius && camera.x == previousCamera_.x && camera.z == previousCamera_.z &&
        camera.local.x == previousCamera_.local.x && camera.local.z == previousCamera_.local.z)
        return;
    previousCamera_ = camera;
    previousRadius_ = demandRadius;
    previousRevision_ = revision;
    previousEnabled_ = enabled;
    initialized_ = true;
    const auto centre = vegetationCellAt(camera);
    const auto local = relativePosition(camera, vegetationCellOrigin(centre));
    deferred_ = retiring_ = 0;
    wanted_.clear();
    ordered_.clear();
    std::fill(table_.begin(), table_.end(), -1);
    for (uint32_t i = 0; i < slots_.size(); ++i) {
        auto &s = slots_[i];
        s.wanted = false;
        if (s.state == State::Active) {
            double dx = double(s.key.x - centre.x) * 4 + 2 - local.x;
            double dz = double(s.key.z - centre.z) * 4 + 2 - local.z;
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
    int radius = int(std::ceil(demandRadius / 4)) + 1;
    for (int z = -radius; z <= radius; ++z)
        for (int x = -radius; x <= radius; ++x) {
            double dx = std::max(0.0, std::abs(x * 4 + 2 - local.x) - 2);
            double dz = std::max(0.0, std::abs(z * 4 + 2 - local.z) - 2);
            if (dx * dx + dz * dz > demandRadius * demandRadius)
                continue;
            double cx = x * 4 + 2 - local.x, cz = z * 4 + 2 - local.z;
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
bool VegetationResidency::valid(VegetationCellHandle h) const {
    return h.index < slots_.size() && slots_[h.index].generation == h.generation &&
           slots_[h.index].state == State::Active;
}
void VegetationResidency::used(uint32_t i, uint64_t serial) {
    slots_[i].lastUse = std::max(slots_[i].lastUse, serial);
}
} // namespace ow

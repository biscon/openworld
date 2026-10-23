#include "terrain_detail_policy.hpp"
#include <algorithm>
#include <cmath>
namespace ow {
void TerrainDetailPolicy::update(const Camera &camera, double extent, double seconds) {
    const glm::dvec2 step{double(camera.chunk.x - camera_.chunk.x) * extent + camera.local.x - camera_.local.x,
                          double(camera.chunk.z - camera_.chunk.z) * extent + camera.local.z - camera_.local.z};
    if (!initialized_ || seconds <= 0 || seconds > .5 || glm::length(step) > 4 * extent)
        velocity_ = {};
    else
        velocity_ += (step / seconds - velocity_) * (1 - std::exp(-seconds / .15));
    prediction_ = velocity_ * .5;
    const auto length = glm::length(prediction_);
    if (length > 2 * extent)
        prediction_ *= 2 * extent / length;
    camera_ = camera;
    initialized_ = true;
}
TerrainDetailRank TerrainDetailPolicy::rank(TerrainTileKey tile, double extent, bool retained,
                                            double maximumDistance) const {
    const int64_t scale = int64_t(1) << tile.level;
    const glm::dvec2 lo{double(tile.x * scale - camera_.chunk.x) * extent - camera_.local.x,
                        double(tile.z * scale - camera_.chunk.z) * extent - camera_.local.z};
    const auto distance = [&](glm::dvec2 offset) {
        const auto a = lo - offset, b = a + double(scale) * extent;
        return glm::length(glm::max(glm::max(a, -b), glm::dvec2(0))) / extent;
    };
    TerrainDetailRank r;
    r.current = distance({});
    r.distance = std::min(r.current, distance(prediction_) + .25);
    r.protectedNear = tile.level == 0 && r.current <= 1;
    r.eligible = r.protectedNear || r.distance <= maximumDistance / extent;
    r.priority = r.distance + double(scale - 1) - (retained ? double(scale) : 0);
    return r;
}
bool TerrainDetailPolicy::before(const TerrainDetailRank &a, const TerrainDetailRank &b) {
    if (a.protectedNear != b.protectedNear)
        return a.protectedNear;
    return a.protectedNear ? a.current < b.current : a.priority < b.priority;
}
bool TerrainDetailPolicy::preempt(const TerrainDetailRank &active, const TerrainDetailRank &candidate, double age,
                                  double progress) {
    if (!candidate.eligible || age < .1 || progress >= .75 || !before(candidate, active))
        return false;
    if (candidate.protectedNear && !active.protectedNear)
        return true;
    // A protected job must not be replaced by an unprotected prediction.
    if (active.protectedNear && !candidate.protectedNear)
        return false;
    return candidate.priority * 2 <= active.priority && active.distance - candidate.distance >= .5;
}
} // namespace ow

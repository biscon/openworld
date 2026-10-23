#pragma once
#include "terrain_stream.hpp"

namespace ow {
struct TerrainDetailRank {
    bool eligible = false, protectedNear = false;
    double current = 0, distance = 0, priority = 0; // Base-chunk units.
};
// Allocation-free camera demand policy; elapsed time is supplied by the caller.
class TerrainDetailPolicy {
  public:
    void update(const Camera &, double extent, double seconds);
    TerrainDetailRank rank(TerrainTileKey, double extent, bool residentDetail, double maximumDistance) const;
    glm::dvec2 prediction() const { return prediction_; }
    static bool before(const TerrainDetailRank &, const TerrainDetailRank &);
    static bool preempt(const TerrainDetailRank &active, const TerrainDetailRank &candidate, double ageSeconds,
                        double progress);
    static bool cooling(double now, double restartAfter, bool betterAvailable) {
        return betterAvailable && now < restartAfter;
    }

  private:
    Camera camera_{};
    glm::dvec2 velocity_{}, prediction_{};
    bool initialized_ = false;
};
} // namespace ow

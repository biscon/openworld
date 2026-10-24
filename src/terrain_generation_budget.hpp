#pragma once
#include <array>
#include <cstdint>

namespace ow {
enum class TerrainComputeKernel : uint32_t {
    Height,
    Normal,
    Filter,
    Bounds,
    Pack,
    TileHeight,
    TileNormal,
    TileFilteredNormal,
    TileFilteredHeight,
    QualityBounds,
    CompactExpand,
    CompactBounds,
    Count
};
// Workgroup cost, not dispatch count: dispatch rectangles can change shape.
// Observations belong to completed submissions, never the current generation.
class TerrainGenerationBudget {
  public:
    static constexpr uint32_t maximumGroups = 4096, maximumDispatches = 128, maximumSegments = 32;
    static constexpr double softTargetMilliseconds = 4.0;
    static constexpr double targetMilliseconds = softTargetMilliseconds * .9;
    uint32_t groups(TerrainComputeKernel kernel) const { return uint32_t(costs_[uint32_t(kernel)].allowance); }
    double groupMilliseconds(TerrainComputeKernel) const;
    void observe(TerrainComputeKernel, uint32_t actualGroups, double milliseconds);

  private:
    struct Cost {
        double milliseconds = 0;
        // Preserve fractional capacity so integer group rounding does not
        // discard time that another, cheaper stage can use.
        double allowance = 4;
    };
    std::array<Cost, uint32_t(TerrainComputeKernel::Count)> costs_{};
};
enum class TerrainGenerationStop { Idle, TimeBudget, GroupLimit, DispatchLimit, SegmentLimit, JobComplete };
const char *terrainGenerationStopName(TerrainGenerationStop);
// Shared across all stages recorded for one frame. Only the first group may
// exceed the estimate, so an expensive kernel cannot permanently stop progress.
struct TerrainGenerationFrameBudget {
    uint32_t groups = 0, dispatches = 0, segments = 0;
    double milliseconds = 0;
    TerrainGenerationStop limit() const;
    uint32_t allowance(double groupMilliseconds) const;
    void consume(uint32_t count, double groupMilliseconds);
};
struct TerrainComputeBatch {
    uint32_t x{}, y{}, width{}, height{}; // In workgroups.
    uint32_t groups() const { return width * height; }
};
TerrainComputeBatch terrainComputeBatch(uint32_t rowGroups, uint32_t cursor, uint32_t allowance);
} // namespace ow

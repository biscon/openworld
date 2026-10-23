#include "terrain_generation_budget.hpp"
#include <algorithm>
#include <cmath>

namespace ow {
double TerrainGenerationBudget::groupMilliseconds(TerrainComputeKernel kernel) const {
    const auto &cost = costs_[uint32_t(kernel)];
    return std::max(cost.milliseconds, targetMilliseconds / cost.allowance);
}
TerrainGenerationStop TerrainGenerationFrameBudget::limit() const {
    if (groups >= TerrainGenerationBudget::maximumGroups) return TerrainGenerationStop::GroupLimit;
    if (dispatches >= TerrainGenerationBudget::maximumDispatches) return TerrainGenerationStop::DispatchLimit;
    if (segments >= TerrainGenerationBudget::maximumSegments) return TerrainGenerationStop::SegmentLimit;
    return TerrainGenerationStop::TimeBudget;
}
uint32_t TerrainGenerationFrameBudget::allowance(double cost) const {
    if (limit() != TerrainGenerationStop::TimeBudget || !std::isfinite(cost) || cost <= 0) return 0;
    const auto remaining = TerrainGenerationBudget::maximumGroups - groups;
    const double count = std::clamp((TerrainGenerationBudget::targetMilliseconds - milliseconds) / cost + 1e-9,
                                  0.0, double(remaining));
    return std::max(groups == 0 ? 1u : 0u, uint32_t(count));
}
void TerrainGenerationFrameBudget::consume(uint32_t count, double cost) {
    groups += count;
    ++dispatches;
    milliseconds += count * cost;
}
const char *terrainGenerationStopName(TerrainGenerationStop stop) {
    switch (stop) {
    case TerrainGenerationStop::Idle: return "idle / publication";
    case TerrainGenerationStop::TimeBudget: return "time budget";
    case TerrainGenerationStop::GroupLimit: return "group limit";
    case TerrainGenerationStop::DispatchLimit: return "dispatch limit";
    case TerrainGenerationStop::SegmentLimit: return "stage limit";
    case TerrainGenerationStop::JobComplete: return "job complete";
    }
    return "unknown";
}
void TerrainGenerationBudget::observe(TerrainComputeKernel kernel, uint32_t actualGroups, double milliseconds) {
    if (!actualGroups || !std::isfinite(milliseconds) || milliseconds <= 0)
        return;
    auto &cost = costs_[uint32_t(kernel)];
    const double sample = milliseconds / actualGroups;
    // React to overruns immediately; recover cautiously from noisy/stale samples.
    cost.milliseconds = std::max(sample, cost.milliseconds * .75 + sample * .25);
    // Keep 10% headroom under the soft target for timing variation.
    const double target = std::clamp(targetMilliseconds / cost.milliseconds, 1.0, double(maximumGroups));
    cost.allowance = std::min(target, double(uint32_t(cost.allowance) * 2));
}
TerrainComputeBatch terrainComputeBatch(uint32_t rowGroups, uint32_t cursor, uint32_t allowance) {
    if (!rowGroups || cursor >= uint64_t(rowGroups) * rowGroups || !allowance)
        return {};
    allowance = std::min(allowance, 256u);
    TerrainComputeBatch batch;
    batch.x = cursor % rowGroups;
    batch.y = cursor / rowGroups;
    batch.width = std::min(rowGroups - batch.x, allowance);
    batch.height = batch.width == rowGroups ? std::min(rowGroups - batch.y, allowance / rowGroups) : 1;
    return batch;
}
} // namespace ow

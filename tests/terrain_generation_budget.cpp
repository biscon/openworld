#include "terrain_generation_budget.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>
using namespace ow;
namespace {
void frameCarryover() {
    constexpr double target = TerrainGenerationBudget::targetMilliseconds;
    TerrainGenerationBudget budget;
    const auto height = TerrainComputeKernel::TileHeight, normal = TerrainComputeKernel::TileNormal;
    TerrainGenerationFrameBudget frame;
    const auto startup = budget.groupMilliseconds(height);
    require(frame.allowance(startup) == 4, "missing timestamps retain conservative startup");
    frame.consume(1, startup);
    ++frame.segments;
    require(frame.allowance(budget.groupMilliseconds(normal)) == 3,
            "short height tail leaves time for the next kernel");
    frame.consume(3, budget.groupMilliseconds(normal));
    require(frame.allowance(startup) == 0, "next stage cannot reset the frame time budget");

    budget.observe(normal, 4, 4 * target * .6);
    frame = {};
    require(frame.allowance(budget.groupMilliseconds(normal)) == 1, "only one group costing 60% of the target fits");
    frame.consume(1, budget.groupMilliseconds(normal));
    require(frame.allowance(startup) == 1, "integer group rounding does not discard time for a cheaper stage");

    budget.observe(height, 4, .04);
    budget.observe(normal, 4, 5 * target);
    frame = {};
    frame.consume(1, budget.groupMilliseconds(height));
    require(frame.allowance(budget.groupMilliseconds(normal)) == 0,
            "expensive next stage waits instead of repeating minimum progress");
    frame = {};
    require(frame.allowance(budget.groupMilliseconds(normal)) == 1, "expensive first stage makes progress");
    frame.consume(1, budget.groupMilliseconds(normal));
    require(frame.allowance(budget.groupMilliseconds(height)) == 0, "minimum progress exhausts shared budget");

    frame = {};
    frame.consume(100, target / 300);
    ++frame.segments;
    require(frame.allowance(target / 150) == 100, "mixed kernel costs spend remaining milliseconds, not groups");
    frame.consume(100, target / 150);
    require(frame.allowance(.001) == 0, "mixed costs obey unchanged total target");
    frame = {};
    frame.reservedGroups = 32;
    frame.reservedDispatches = 1;
    frame.milliseconds = .5;
    require(frame.allowance(.000001) == TerrainGenerationBudget::maximumGroups - 32,
            "vegetation shares hard group allowance");
    frame.dispatches = TerrainGenerationBudget::maximumDispatches - 1;
    require(frame.allowance(.000001) == 0, "vegetation dispatch shares hard dispatch allowance");
    frame = {};
    require(frame.allowance(.000001) == TerrainGenerationBudget::maximumGroups, "group hard limit caps fast kernels");
    frame.consume(TerrainGenerationBudget::maximumGroups, .000001);
    require(!frame.allowance(.000001) && frame.limit() == TerrainGenerationStop::GroupLimit, "group cap crosses stages");
    frame = {};
    for (uint32_t i = 0; i < TerrainGenerationBudget::maximumDispatches; ++i) frame.consume(1, .000001);
    require(!frame.allowance(.000001) && frame.limit() == TerrainGenerationStop::DispatchLimit, "dispatch cap crosses stages");
    frame = {};
    for (uint32_t i = 0; i < TerrainGenerationBudget::maximumSegments; ++i) {
        require(frame.allowance(.000001) > 0, "segment storage available");
        frame.consume(1, .000001);
        ++frame.segments;
    }
    require(!frame.allowance(.000001) && frame.limit() == TerrainGenerationStop::SegmentLimit, "segment metadata stays bounded");
}
void adaptation() {
    TerrainGenerationBudget budget;
    constexpr auto normal = TerrainComputeKernel::TileNormal;
    constexpr auto filtered = TerrainComputeKernel::TileFilteredNormal;
    // Simulate a fast GPU completing a default tile's three normal levels.
    // Timing cost is synthetic; this checks scheduling, not hardware throughput.
    int frames = 0;
    uint32_t largest = 0;
    for (uint32_t side : {66u, 33u, 17u}) {
        const auto kernel = side == 66 ? normal : filtered;
        uint32_t remaining = side * side;
        while (remaining) {
            const auto groups = std::min(remaining, budget.groups(kernel));
            largest = std::max(largest, groups);
            budget.observe(kernel, groups, groups * .0005);
            remaining -= groups;
            require(++frames < 40, "fast generation escapes the old 183-frame floor");
        }
    }
    require(largest > 32, "fast hardware uses more work than eight 16x16 dispatches");
    const auto before = budget.groups(normal);
    for (int i = 0; i < 8; ++i)
        budget.observe(normal, 1, .0005); // Short tails must be normalized by actual work.
    require(budget.groups(normal) == before, "partial tails preserve measured work cost");
    budget.observe(TerrainComputeKernel::Pack, 4, .000001);
    require(budget.groups(normal) == before, "cheap kernels do not change normal allowance");
    budget.observe(normal, 4, 8);
    require(budget.groups(normal) == 1, "cost increase immediately cuts work to minimum");
    budget.observe(normal, 1, 8);
    require(budget.groups(normal) == 1, "one expensive group still makes progress");
    const auto slow = budget.groups(normal);
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()})
        budget.observe(normal, 1, invalid);
    budget.observe(normal, 0, .001);
    require(budget.groups(normal) == slow, "missing/invalid timing never expands work");
    for (int i = 0; i < 200; ++i) {
        auto old = budget.groups(normal);
        budget.observe(normal, old, .000001);
        require(budget.groups(normal) <= old * 2, "recovery growth stays bounded");
    }
    require(budget.groups(normal) == TerrainGenerationBudget::maximumGroups, "CPU recording work stays bounded");
}
void rectangles() {
    for (uint32_t side : {1u, 2u, 5u, 17u, 33u, 66u, 131u}) {
        std::vector<uint8_t> visited(side * side);
        uint32_t cursor = 0, frame = 0;
        while (cursor < visited.size()) {
            const uint32_t allowances[] = {1, 5, 13, 4096};
            uint32_t remaining = allowances[frame++ % 4];
            for (uint32_t dispatch = 0; remaining && cursor < visited.size() &&
                                        dispatch < TerrainGenerationBudget::maximumDispatches; ++dispatch) {
                auto batch = terrainComputeBatch(side, cursor, remaining);
                require(batch.groups() && batch.groups() <= remaining && batch.groups() <= 256,
                        "nonempty rectangle obeys work allowance");
                require(batch.x + batch.width <= side && batch.y + batch.height <= side,
                        "dispatch remains inside output groups");
                for (uint32_t z = batch.y; z < batch.y + batch.height; ++z)
                    for (uint32_t x = batch.x; x < batch.x + batch.width; ++x)
                        require(visited[z * side + x]++ == 0, "adaptive rectangles never overwrite earlier groups");
                cursor += batch.groups();
                remaining -= batch.groups();
            }
        }
        for (auto count : visited)
            require(count == 1, "adaptive rectangles cover every group exactly once");
        require(terrainComputeBatch(side, cursor, 100).groups() == 0, "completed grid emits no work");
    }
    require(terrainComputeBatch(0, 0, 100).groups() == 0 && terrainComputeBatch(10, 0, 0).groups() == 0,
            "empty grid or exhausted budget emits no work");
}
} // namespace
int main() {
    try {
        adaptation();
        frameCarryover();
        rectangles();
        std::cout << "Terrain generation budget tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

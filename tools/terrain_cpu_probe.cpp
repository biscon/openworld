#include "temporal.hpp"
#include "terrain_ground_lod.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>

// Owned CPU workload, deliberately separate from correctness tests and GPU
// timing.
using namespace ow;
using Clock = std::chrono::steady_clock;
namespace {
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void fold(uint64_t &hash, uint64_t word) { hash = (hash ^ word) * 1099511628211ull; }
void report(const char *stage, uint64_t workload, std::vector<double> &times, uint64_t selected,
            uint64_t partition, uint64_t fingerprint) {
    std::sort(times.begin(), times.end());
    std::cout << stage << ',' << workload << ',' << times[times.size() / 2] << ','
              << times[std::min(times.size() - 1, times.size() * 95 / 100)] << ',' << selected << ','
              << partition << ',' << fingerprint << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        const uint32_t frames = argc > 1 ? uint32_t(std::stoul(argv[1])) : 120;
        if (argc > 2 || frames < 10 || frames > 10000)
            throw std::invalid_argument("Usage: terrain_cpu_probe [frames: 10..10000]");
        const TerrainGrid grid{256, .5f};
        TerrainLod metadata(grid, 256, {true, 32, 64}, 180);
        const auto topology = terrainPatchTopology(32);
        std::vector<glm::vec2> bounds(64, glm::vec2(1000, 1300));
        std::vector<TerrainPatchMetadata> values;
        for (uint32_t l = 0; l < 4; ++l)
            for (uint32_t i = 0; i < (8u >> l) * (8u >> l); ++i)
                values.push_back({{1000, 1300},
                                  glm::vec2(.5f * (1u << l)),
                                  glm::vec2(1.f * (1u << l)),
                                  glm::vec2(.25f * (1u << l))});
        for (uint32_t i = 0; i < 180; ++i) {
            metadata.setBounds(i, bounds.data());
            metadata.setQualityMetadata(i, values.data());
        }
        std::cout << std::fixed << std::setprecision(6)
                  << "stage,budget_or_records,median_ms,p95_ms,visible_patches,"
                     "partition_patches,fingerprint\n";
        std::vector<double> times;
        times.reserve(frames);
        for (uint64_t budget : {524288, 2097152}) {
            TerrainGroundLod selector(grid, 32, 180, 65536, budget);
            Camera c;
            c.local = {47, 1104, 57};
            c.yaw = 3.06f;
            c.pitch = -.185f;
            c.curvature = {6360000, 1104, 1, 8192};
            uint64_t fingerprint = 14695981039346656037ull;
            times.clear();
            for (uint32_t frame = 0; frame < frames + 16; ++frame) {
                c.local.x += .016;
                const auto start = Clock::now();
                selector.begin(c, c, c.viewProjection(16.f / 9), 935, true, 1.2f, false, true, true, true);
                uint32_t slot = 0;
                for (uint32_t l = 0; l < 14; ++l)
                    for (int z = -2; z < 2; ++z)
                        for (int x = -2; x < 2; ++x)
                            if (l == 0 || x == -2 || x == 1 || z == -2 || z == 1)
                                selector.tile(slot++, {x * (1ll << l), z * (1ll << l)}, l, 15, 0);
                selector.finish(metadata, topology);
                const auto ms = elapsed(start);
                if (frame < 16)
                    continue;
                times.push_back(ms);
                // Includes all leaves, visible draws and seam contracts, outside
                // timing.
                for (const auto *draws : {&selector.casters(), &selector.draws()})
                    for (const auto &p : *draws)
                        for (auto word : {p.slot, p.x, p.z, p.step, p.level, p.cells, p.edges, p.corners})
                            fold(fingerprint, word);
            }
            report("selection", budget, times, selector.stats().selected, selector.casters().size(),
                   fingerprint);
        }
        for (uint32_t count : {1024, 4095}) {
            TerrainHistory history(count);
            std::vector<TemporalTerrainKey> keys(count);
            for (uint32_t i = 0; i < count; ++i)
                keys[i].words = {i / 32, i % 32, 1, 1, 1, 1, 1, 1};
            std::mt19937 random(1);
            std::shuffle(keys.begin(), keys.end(), random);
            TemporalTerrainRecord record;
            record.valid.x = 1;
            times.clear();
            uint64_t matches = 0;
            for (uint32_t frame = 0; frame < frames + 16; ++frame) {
                const auto start = Clock::now();
                history.begin(true);
                for (uint32_t i = 0; i < count; ++i)
                    matches += history.append(keys[(i + frame) % count], record).valid.x > 0;
                history.submitted();
                const auto ms = elapsed(start);
                if (frame >= 16)
                    times.push_back(ms);
            }
            if (matches != uint64_t(frames + 15) * count)
                throw std::runtime_error("Temporal correspondence changed during benchmark");
            report("history", count, times, count, count, matches);
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

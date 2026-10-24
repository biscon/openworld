#include "terrain_lod.hpp"
#include "terrain_ground_lod.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <tuple>
using namespace ow;
namespace {
bool tracking = false;
size_t allocations = 0;
} // namespace
void *operator new(size_t n) {
    if (tracking)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
namespace {
void topology() {
    const auto t = terrainPatchTopology(8);
    for (uint32_t mask = 0; mask < 16; ++mask) {
        std::array<uint32_t, 64> coverage{};
        const auto r = t.ranges[mask];
        for (uint32_t i = r.first; i < r.first + r.count; i += 3) {
            glm::vec3 p[3];
            for (uint32_t j = 0; j < 3; ++j) {
                const auto v = t.indices[i + j];
                require(v < 81, "Patch index within grid");
                p[j] = {float(v % 9), 0, float(v / 9)};
            }
            require(glm::cross(p[1] - p[0], p[2] - p[0]).y > 0, "Upward patch winding");
            auto c = (p[0] + p[1] + p[2]) / 3.0f;
            ++coverage[uint32_t(c.z) * 8 + uint32_t(c.x)];
        }
        for (uint32_t z = 0; z < 8; ++z)
            for (uint32_t x = 0; x < 8; ++x)
                require(coverage[z * 8 + x] == ((mask & (1u << ((x / 4) + 2 * (z / 4)))) ? 2u : 0u),
                        "Exact quadrant mask coverage");
    }
    rejects([] { validateLodGrid({33, .5f}, 8); }, "LOD rejects non-power-of-two grids");
    rejects([] { validateLodGrid({32, .5f}, 64); }, "LOD rejects oversized patches");
}
float height(float x, float z, int fixture) {
    if (fixture == 0)
        return 0;
    if (fixture == 1)
        return .7f * x + .3f * z;
    return 6 * std::sin(x * .15f) * std::cos(z * .13f);
}
using Point = std::tuple<int64_t, int64_t, int64_t>;
Point key(glm::vec3 p) {
    return {std::llround(p.x * 10000), std::llround(p.y * 1000), std::llround(p.z * 10000)};
}
void selectionAndSeams() {
    const TerrainGrid grid{64, .5f};
    const auto topology = terrainPatchTopology(8);
    TerrainLod lod(grid, 64, {true, 8, 16});
    for (int fixture = 0; fixture < 3; ++fixture)
        for (auto origin : {ChunkCoord{0, 0}, ChunkCoord{-4, -9}, ChunkCoord{1000000, -1000000}}) {
            auto batch = terrainNeighbourhood({origin, {}});
            for (uint32_t slot = 0; slot < 9; ++slot) {
                std::array<glm::vec2, 64> bounds;
                const float ox = float(batch.chunks[slot].x - origin.x) * 32,
                            oz = float(batch.chunks[slot].z - origin.z) * 32;
                for (uint32_t z = 0; z < 8; ++z)
                    for (uint32_t x = 0; x < 8; ++x) {
                        auto &b = bounds[z * 8 + x];
                        b = {INFINITY, -INFINITY};
                        for (uint32_t v = 0; v <= 8; ++v)
                            for (uint32_t u = 0; u <= 8; ++u) {
                                const float h =
                                    height(ox + (x * 8 + u) * .5f, oz + (z * 8 + v) * .5f, fixture);
                                b.x = std::min(b.x, h);
                                b.y = std::max(b.y, h);
                            }
                    }
                lod.setBounds(slot, bounds.data());
            }
            for (float movement : {0.f, 7.3f, 19.1f, 43.7f}) {
                Camera camera;
                camera.chunk = origin;
                camera.local = {movement, 8, 10};
                lod.select(batch.chunks, 9, camera, camera, camera.viewProjection(1), true, false, true);
                require(lod.stats().triangles < 9 * 64 * 64 * 2, "Distance selection reduces triangles");
                std::array<std::array<unsigned char, 4096>, 9> coverage{};
                std::map<std::pair<Point, Point>, uint32_t> edges;
                std::map<Point, float> levelAtVertex;
                bool mixed = false;
                for (const auto &patch : lod.draws()) {
                    mixed |= patch.level > 0;
                    const uint32_t size = patch.step * 8, half = size / 2;
                    for (uint32_t q = 0; q < 4; ++q)
                        if (patch.mask & (1u << q))
                            for (uint32_t z = 0; z < half; ++z)
                                for (uint32_t x = 0; x < half; ++x)
                                    ++coverage[patch.slot][(patch.z + (q / 2) * half + z) * 64 + patch.x +
                                                           (q % 2) * half + x];
                    const float ox = float(batch.chunks[patch.slot].x - origin.x) * 32,
                                oz = float(batch.chunks[patch.slot].z - origin.z) * 32;
                    const auto r = topology.ranges[patch.mask];
                    for (uint32_t i = r.first; i < r.first + r.count; i += 3) {
                        glm::vec3 p[3];
                        float levels[3];
                        for (uint32_t j = 0; j < 3; ++j) {
                            const uint32_t v = topology.indices[i + j];
                            const glm::uvec2 local(v % 9, v / 9);
                            glm::vec2 g = glm::vec2(patch.x, patch.z) + glm::vec2(local) * float(patch.step);
                            glm::vec3 original(ox + g.x * .5f, 0, oz + g.y * .5f);
                            original.y = height(original.x, original.z, fixture);
                            const float distance = glm::length(original - glm::vec3(camera.local));
                            const float m = patch.level + 1 < lod.levels()
                                                ? terrainMorph(distance, lod.stats().firstRange * patch.step)
                                                : 0;
                            glm::vec3 anchor = original;
                            anchor.x -= float(local.x & 1) * patch.step * .5f;
                            anchor.z -= float(local.y & 1) * patch.step * .5f;
                            anchor.y = height(anchor.x, anchor.z, fixture);
                            const bool endpoint = m == 1 && glm::length(anchor - glm::vec3(camera.local)) >=
                                                                lod.stats().firstRange * patch.step;
                            g = glm::vec2(patch.x, patch.z) +
                                (glm::vec2(local) -
                                 glm::vec2(local & glm::uvec2(1)) * (endpoint ? 1.f : 0.f)) *
                                    float(patch.step);
                            p[j] = {ox + g.x * .5f, 0, oz + g.y * .5f};
                            if (m < 1)
                                require(g == glm::vec2(patch.x, patch.z) +
                                                 glm::vec2(local) * float(patch.step),
                                        "Intermediate morph never stretches horizontal topology");
                            const float step = float(patch.step);
                            const float ax = std::floor((original.x - ox) / step) * step + ox;
                            const float az = std::floor((original.z - oz) / step) * step + oz;
                            const float fx = (original.x - ax) / step, fz = (original.z - az) / step;
                            const float a = height(ax, az, fixture),
                                        d = height(ax + step, az + step, fixture);
                            const float target =
                                fx >= fz ? a * (1 - fx) + height(ax + step, az, fixture) * (fx - fz) + d * fz
                                         : a * (1 - fz) + d * fx + height(ax, az + step, fixture) * (fz - fx);
                            p[j].y =
                                endpoint ? height(p[j].x, p[j].z, fixture) : glm::mix(original.y, target, m);
                            levels[j] = patch.level + m;
                        }
                        const float area = glm::cross(p[1] - p[0], p[2] - p[0]).y;
                        require(area >= -1e-5, "Morph cannot invert terrain triangles");
                        if (area == 0)
                            continue;
                        for (uint32_t j = 0; j < 3; ++j) {
                            const auto a = key(p[j]), b = key(p[(j + 1) % 3]);
                            if (a != b)
                                ++edges[std::minmax(a, b)];
                            const auto found = levelAtVertex.emplace(a, levels[j]);
                            if (!found.second)
                                near(found.first->second, levels[j], .0001,
                                     "Shared vertices use identical normal blend levels");
                        }
                    }
                }
                require(mixed, "Fixture exercises coarser nodes");
                for (const auto &chunk : coverage)
                    for (auto c : chunk)
                        require(c == 1, "No gaps or overlap in selected quadrants");
                for (const auto &e : edges) {
                    require(e.second == 1 || e.second == 2, "Surface is a manifold");
                    if (e.second == 2)
                        continue;
                    const auto &a = e.first.first;
                    const auto &b = e.first.second;
                    const bool outer = (std::get<0>(a) == std::get<0>(b) &&
                                        (std::get<0>(a) == -320000 || std::get<0>(a) == 640000)) ||
                                       (std::get<2>(a) == std::get<2>(b) &&
                                        (std::get<2>(a) == -320000 || std::get<2>(a) == 640000));
                    require(outer, "No unmatched interior edge at mixed levels or corners");
                }
            }
        }
}
void cullingAndAllocation() {
    TerrainLod lod({64, .5f}, 64, {true, 8, 16});
    std::array<glm::vec2, 64> b{};
    lod.setBounds(0, b.data());
    std::array<ChunkCoord, 9> chunks{};
    Camera c;
    c.local = {16, 10, 16};
    c.pitch = 1.5f;
    auto select = [&](bool enabled, bool cull, bool ready) {
        lod.select(chunks, 1, c, c, c.viewProjection(1), enabled, cull, ready);
    };
    select(true, true, true);
    require(lod.draws().empty() && lod.stats().culled, "Terrain behind camera culled");
    select(true, true, false);
    require(lod.stats().triangles == 64 * 64 * 2, "Missing bounds disable culling and LOD");
    c.pitch = -1.5f;
    select(false, false, true);
    require(lod.stats().triangles == 64 * 64 * 2, "Full detail comparison");
    c.local.y = 1000;
    select(true, false, true);
    require(lod.draws().size() == 1 && lod.draws()[0].level == 3,
            "Far field uses root, without distance holes");
    const auto before = allocations;
    tracking = true;
    for (uint32_t i = 0; i < 1000; ++i) {
        c.local.x = i * .1;
        select(true, true, true);
    }
    tracking = false;
    require(allocations == before, "LOD selection performs no allocations");
    lod.setGrid({32, .5f});
    rejects([&] { lod.setGrid({128, .5f}); }, "Capacity failure is explicit");
    require(lod.levels() == 3, "Failed reconfiguration preserves valid hierarchy");
}
void sampledErrorProjection() {
    TerrainLod lod({16, .5f}, 16, {true, 8, 64}, 1);
    const std::array<glm::vec2, 4> bounds = {glm::vec2(-1, 1), glm::vec2(-1, 1), glm::vec2(-1, 1),
                                             glm::vec2(-1, 1)};
    lod.setBounds(0, bounds.data());
    const std::array<TerrainPatchMetadata, 5> metadata = {{{{-1, 1}, {1, 2}, {}, {}},
                                                           {{-1, 1}, {1, 2}, {}, {}},
                                                           {{-1, 1}, {1, 2}, {}, {}},
                                                           {{-1, 1}, {1, 2}, {}, {}},
                                                           {{-2, 2}, {3, 4}, {}, {}}}};
    lod.setQualityMetadata(0, metadata.data());
    Camera c;
    c.local = {4, 3, 100};
    TerrainPatchDraw patch{0, 0, 0, 2, 1, 15};
    const float a = lod.projectedError(patch, {}, 0, c, 1000, false);
    near(lod.projectedError(patch, {}, 0, c, 2000, false), 2 * a, 1e-5,
         "pixel error tracks output resolution");
    near(lod.projectedError(patch, {}, 0, c, 1000, true), a * 4 / 3, 1e-5,
         "base and erosion errors remain distinct");
    c.local.z = 200;
    require(lod.projectedError(patch, {}, 0, c, 1000, false) < a, "distant error projects smaller");
    {
        TerrainLod broad({256, .5f}, 256, {true, 32, 64}, 1);
        std::array<glm::vec2, 64> flatBounds;
        flatBounds.fill({-1, 1});
        broad.setBounds(0, flatBounds.data());
        Camera view;
        view.local = {64, 100, 64};
        broad.beginHierarchy(view, view, view.viewProjection(1), 8, 64);
        broad.selectTile(0, {}, 0, 15, true, false);
        const auto baseline = broad.stats().triangles;
        broad.beginHierarchy(view, view, view.viewProjection(1), 8, 128);
        broad.selectTile(0, {}, 0, 15, true, false);
        require(broad.stats().triangles > baseline,
                "larger shared error ranges retain finer resident geometry");
    }
    const auto before = allocations;
    tracking = true;
    for (int i = 0; i < 1000; ++i) {
        lod.beginHierarchy(c, c, c.viewProjection(1), 32, 80);
        lod.selectTile(0, {}, 0, 15, true, false);
        for (const auto &p : lod.draws())
            (void)lod.projectedError(p, {}, 0, c, 1000, true);
    }
    tracking = false;
    require(before == allocations, "quality metadata selection is allocation free");
}

void stationaryTopology() {
    const auto topology = terrainPatchTopology(32);
    constexpr int cells = 16, width = 17;
    for (uint32_t mask = 0; mask < 16; ++mask) {
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        double area = 0;
        const auto range = topology.stitched[mask];
        for (uint32_t i = range.first; i < range.first + range.count; i += 3) {
            glm::dvec3 p[3];
            uint32_t ids[3];
            for (int j = 0; j < 3; ++j) {
                const auto id = ids[j] = topology.indices[i + j];
                const auto x = id % width, z = id / width;
                require(x <= cells && z <= cells, "Stitched index inside patch");
                require(!(((x == 0 && (mask & 1)) || (x == cells && (mask & 2))) && (z & 1)),
                        "Coarse X edge omits odd vertices");
                require(!(((z == 0 && (mask & 4)) || (z == cells && (mask & 8))) && (x & 1)),
                        "Coarse Z edge omits odd vertices");
                p[j] = {double(x), 0, double(z)};
            }
            const auto a = glm::cross(p[1] - p[0], p[2] - p[0]).y * .5;
            require(a > 0, "Every stitched triangle has positive area");
            area += a;
            for (int j = 0; j < 3; ++j)
                ++edges[std::minmax(ids[j], ids[(j + 1) % 3])];
        }
        near(area, cells * cells, 0, "Stitched triangles cover full rectangle");
        for (const auto &e : edges)
            if (e.second != 2) {
                const auto a = e.first.first, b = e.first.second;
                require(e.second == 1 &&
                            ((a % width == b % width && (a % width == 0 || a % width == cells)) ||
                             (a / width == b / width && (a / width == 0 || a / width == cells))),
                        "Only outer stitched edges are unmatched");
            }
    }
}
void stationarySelection() {
    const TerrainGrid grid{64, 2};
    const auto topology = terrainPatchTopology(8);
    TerrainLod metadata(grid, 64, {true, 8, 64}, 9);
    std::array<glm::vec2, 64> bounds;
    bounds.fill({-4, 4});
    std::vector<TerrainPatchMetadata> samples;
    for (uint32_t l = 0; l < 4; ++l)
        for (uint32_t i = 0; i < (8u >> l) * (8u >> l); ++i)
            samples.push_back({{-4, 4},
                               glm::vec2(.005f * (1u << l)),
                               glm::vec2(.02f * (1u << l)),
                               glm::vec2(.01f * (1u << l))});
    for (uint32_t i = 0; i < 9; ++i) {
        metadata.setBounds(i, bounds.data());
        metadata.setQualityMetadata(i, samples.data());
    }
    TerrainGroundLod selector(grid, 8, 9, 8192, 131072);
    Camera c;
    c.local = {64, 6, 64};
    auto select = [&](bool quality, bool freeze) {
        selector.begin(c, c, c.viewProjection(1), 300, quality, 2, freeze, false, true, true);
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                selector.tile((z + 1) * 3 + x + 1, {x, z}, 0, 15, 0);
        selector.finish(metadata, topology);
    };
    for (bool quality : {false, true}) {
        selector.invalidate();
        select(quality, false);
        auto old = selector.snapshot();
        std::array<int, 48 * 48> cover;
        cover.fill(-1);
        const auto &patches = selector.casters();
        for (uint32_t i = 0; i < patches.size(); ++i) {
            const auto &p = patches[i];
            int tx = int(p.slot % 3) - 1, tz = int(p.slot / 3) - 1;
            int x = (tx * 64 + int(p.x) + 64) / 4, z = (tz * 64 + int(p.z) + 64) / 4;
            for (uint32_t v = 0; v < p.step; ++v)
                for (uint32_t u = 0; u < p.step; ++u) {
                    auto &value = cover[(z + v) * 48 + x + u];
                    require(value == -1, "Stationary selection never overlaps");
                    value = int(i);
                }
        }
        for (int z = 0; z < 48; ++z)
            for (int x = 0; x < 48; ++x) {
                int i = cover[z * 48 + x];
                require(i >= 0, "Stationary selection covers every cell");
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (x + dx >= 0 && x + dx < 48 && z + dz >= 0 && z + dz < 48)
                            require(std::abs(int(patches[i].level) -
                                             int(patches[cover[(z + dz) * 48 + x + dx]].level)) <= 1,
                                    "Edges and corners stay 2:1 balanced");
            }
        require(!selector.stats().protectedMissing, "Protected ground retains finest available geometry");
        require(selector.stats().triangles <= selector.stats().triangleBudget,
                "Selection obeys fixed triangle allowance");
        const auto before = allocations;
        tracking = true;
        for (int i = 0; i < 40; ++i) {
            c.yaw += .1f;
            select(quality, true);
        }
        tracking = false;
        require(before == allocations, "Stationary partition frame work allocates nothing");
        require(selector.snapshot() == old, "Frozen selection survives turns");
        selector.invalidate();
        selector.restore(old);
        // An empty startup frame must not discard a restored partition before tiles arrive.
        selector.begin(c, c, c.viewProjection(1), 300, quality, 2, true, false, true, true);
        selector.finish(metadata, topology);
        require(selector.snapshot() == old, "Cold replay retains selection while waiting for residency");
        select(quality, true);
        require(selector.snapshot() == old, "Cold replay restores the geographic partition");
        for (auto bad : {std::vector<TerrainPatchKey>{{0, 0, 32}},
                         {{1, 0, 0}},
                         {{0, 0, 0}, {0, 0, 1}},
                         {{0, 0, 0}, {0, 0, 0}}}) {
            rejects([&] { selector.restore(bad); }, "Invalid replay selection rejected");
            require(selector.snapshot() == old, "Invalid replay preserves selection");
        }
    }
    TerrainGroundLod constrained(grid, 8, 9, 1024, 2048);
    constrained.begin(c, c, c.viewProjection(1), 300, true, 2, false, false, true, true);
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
            constrained.tile((z + 1) * 3 + x + 1, {x, z}, 0, 15, 0);
    constrained.finish(metadata, topology);
    require(constrained.stats().budgetLimited > 0 && constrained.stats().protectedMissing > 0,
            "Impossible budget reports degraded ground explicitly");
    // Under pressure, a high approximation error with zero replacement motion
    // must not starve another tile's mandatory transition refinement.
    auto qualityOnly = samples, movingSurface = samples;
    for (auto &m : qualityOnly) {
        m.error = m.parentError = glm::vec2(10000);
        m.displacement = {};
    }
    for (auto &m : movingSurface) {
        m.error = m.parentError = glm::vec2(1);
        m.displacement = glm::vec2(10);
    }
    metadata.setQualityMetadata(0, qualityOnly.data());
    metadata.setQualityMetadata(1, movingSurface.data());
    c.local.y = 2000;
    TerrainGroundLod priority(grid, 8, 2, 1024, 1024);
    priority.begin(c, c, c.viewProjection(1), 300, true, 2, false, false, true, true);
    priority.tile(0, {5, 0}, 0, 15, 0);
    priority.tile(1, {-5, 0}, 0, 15, 0);
    priority.finish(metadata, topology);
    require(std::count_if(priority.casters().begin(), priority.casters().end(),
                          [](const auto &p) { return p.slot == 0; }) == 4,
            "Optional quality cannot consume transition-safety budget");
    require(priority.stats().refined > 0 && priority.stats().budgetLimited > 0,
            "Priority fixture exercises refinement under pressure");
}
} // namespace
int main() {
    try {
        topology();
        stationaryTopology();
        stationarySelection();
        sampledErrorProjection();
        selectionAndSeams();
        cullingAndAllocation();
        std::cout << "Terrain LOD CPU tests passed\n";
    } catch (const std::exception &e) {
        tracking = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}

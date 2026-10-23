#include "terrain_lod.hpp"
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
float sampledHeight(float x, float z, int fixture) {
    const float a = std::floor(x * 2) * .5f, b = std::floor(z * 2) * .5f, fx = (x - a) * 2, fz = (z - b) * 2;
    return glm::mix(glm::mix(height(a, b, fixture), height(a + .5f, b, fixture), fx),
                    glm::mix(height(a, b + .5f, fixture), height(a + .5f, b + .5f, fixture), fx), fz);
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
                            glm::vec3 anchor = original;
                            anchor.x -= float(local.x & 1) * patch.step * .5f;
                            anchor.z -= float(local.y & 1) * patch.step * .5f;
                            anchor.y = height(anchor.x, anchor.z, fixture);
                            const float distance = glm::length(anchor - glm::vec3(camera.local));
                            const float m = patch.level + 1 < lod.levels()
                                                ? terrainMorph(distance, lod.stats().firstRange * patch.step)
                                                : 0;
                            g = glm::vec2(patch.x, patch.z) + terrainMorphPosition(local, patch.step, m);
                            p[j] = {ox + g.x * .5f, 0, oz + g.y * .5f};
                            p[j].y = sampledHeight(p[j].x, p[j].z, fixture);
                            levels[j] =
                                patch.level + (patch.level + 1 < lod.levels()
                                                   ? terrainMorph(glm::length(p[j] - glm::vec3(camera.local)),
                                                                  lod.stats().firstRange * patch.step)
                                                   : 0);
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
    const std::array<glm::vec4, 5> metadata = {glm::vec4(-1, 1, 1, 2), glm::vec4(-1, 1, 1, 2),
                                               glm::vec4(-1, 1, 1, 2), glm::vec4(-1, 1, 1, 2),
                                               glm::vec4(-2, 2, 3, 4)};
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
        broad.selectTile(0, {}, 0, 15, true, false, {-1, 1});
        const auto baseline = broad.stats().triangles;
        broad.beginHierarchy(view, view, view.viewProjection(1), 8, 128);
        broad.selectTile(0, {}, 0, 15, true, false, {-1, 1});
        require(broad.stats().triangles > baseline, "larger shared error ranges retain finer resident geometry");
    }
    const auto before = allocations;
    tracking = true;
    for (int i = 0; i < 1000; ++i) {
        lod.beginHierarchy(c, c, c.viewProjection(1), 32, 80);
        lod.selectTile(0, {}, 0, 15, true, false, {-2, 2});
        for (const auto &p : lod.draws())
            (void)lod.projectedError(p, {}, 0, c, 1000, true);
    }
    tracking = false;
    require(before == allocations, "quality metadata selection is allocation free");
}
} // namespace
int main() {
    try {
        topology();
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

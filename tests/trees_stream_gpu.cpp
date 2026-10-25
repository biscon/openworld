#include "renderer.hpp"
#include "test_support.hpp"
#include "tree_generation.hpp"
#include <chrono>
#include <iostream>
#include <map>
#include <tuple>
using namespace ow;
namespace {
void checkGround(Renderer &r, const TreeSnapshot &snapshot) {
    using Key = std::tuple<int64_t, int64_t, uint32_t, uint32_t>;
    std::map<Key, std::vector<glm::vec2>> cache;
    uint32_t checked = 0, sloping = 0;
    for (const auto &cell : snapshot.cells) {
        std::vector<glm::vec3> triangles;
        for (uint32_t p = 0; p < cell.surface.count; ++p) {
            const auto &patch = cell.surface.patches[p];
            uint32_t tileLevel = uint32_t(std::lround(std::log2(patch.rectangle.z / .5f)));
            int64_t x = int64_t(std::llround((double(cell.key.x) * 64 + patch.rectangle.x) /
                                             (16.0 * double(1u << tileLevel))));
            int64_t z = int64_t(std::llround((double(cell.key.z) * 64 + patch.rectangle.y) /
                                             (16.0 * double(1u << tileLevel))));
            auto heights = [&](uint32_t level) -> const std::vector<glm::vec2> & {
                Key k{x, z, tileLevel, level};
                auto it = cache.find(k);
                if (it == cache.end())
                    it = cache.emplace(k, r.readStreamedTileHeights({x, z, tileLevel}, level)).first;
                return it->second;
            };
            auto at = [&](uint32_t gx, uint32_t gz, uint32_t level) {
                uint32_t width = (32 >> level) + 1;
                return heights(level).at((gz >> level) * width + (gx >> level))[patch.topology.w];
            };
            auto vertex = [&](uint32_t id) {
                uint32_t side = patch.geometry.w, ix = id % (side + 1), iz = id / (side + 1);
                uint32_t gx = patch.geometry.x + ix * patch.geometry.z,
                         gz = patch.geometry.y + iz * patch.geometry.z;
                bool edge = (ix == 0 && (patch.topology.x & 1)) || (ix == side && (patch.topology.x & 2)) ||
                            (iz == 0 && (patch.topology.x & 4)) || (iz == side && (patch.topology.x & 8));
                if ((ix == 0 || ix == side) && (iz == 0 || iz == side))
                    edge |= (patch.topology.y & (1u << ((ix != 0 ? 1 : 0) + (iz != 0 ? 2 : 0)))) != 0;
                float h = at(gx, gz, patch.topology.z);
                if (edge) {
                    uint32_t level = patch.topology.z + 1, step = 1u << level;
                    uint32_t ax = std::min(gx / step, 32 / step - 1) * step,
                             az = std::min(gz / step, 32 / step - 1) * step;
                    float u = float(gx - ax) / step, v = float(gz - az) / step;
                    float a = at(ax, az, level), b = at(ax + step, az, level), c = at(ax, az + step, level),
                          d = at(ax + step, az + step, level);
                    h = u >= v ? a + (b - a) * u + (d - b) * v : a + (d - c) * u + (c - a) * v;
                }
                return glm::vec3(patch.rectangle.x + gx * patch.rectangle.z, h,
                                 patch.rectangle.y + gz * patch.rectangle.z);
            };
            auto topology = terrainPatchTopology(patch.geometry.w * 2);
            auto range = topology.stitched[patch.topology.x];
            for (uint32_t i = 0; i < range.count; ++i)
                triangles.push_back(vertex(topology.indices[range.first + i]));
        }
        for (uint32_t i = 0; i < 128; ++i) {
            const auto &p = cell.population[i];
            const auto &attachment = cell.attachments[i];
            require(p.identity == treeCandidateHash(cell.key, cell.surface.geography.dimensions.y, i),
                    "CPU/GPU candidate identity agreement");
            if (attachment.w == 0)
                continue;
            bool found = false;
            for (size_t t = 0; t < triangles.size(); t += 3) {
                auto a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
                glm::vec2 e(b.x - a.x, b.z - a.z), f(c.x - a.x, c.z - a.z),
                    q(p.rootScale.x - a.x, p.rootScale.z - a.z);
                float det = e.x * f.y - e.y * f.x;
                if (std::abs(det) < 1e-8)
                    continue;
                float u = (q.x * f.y - q.y * f.x) / det, v = (e.x * q.y - e.y * q.x) / det;
                if (u < -.00001 || v < -.00001 || u + v > 1.00001)
                    continue;
                near(attachment.x, a.y + u * (b.y - a.y) + v * (c.y - a.y), .0002,
                     "attached root on actual indexed terrain triangle");
                auto n = glm::cross(b - a, c - a);
                sloping += glm::length(glm::vec2(n.x, n.z)) > .0001;
                found = true;
                break;
            }
            require(found, "root has supporting terrain triangle");
            ++checked;
        }
    }
    require(checked > 8 && sloping > 4, "nontrivial sloping attachment fixture");
}
TreeSnapshot settle(Renderer &r, Camera &c, SceneSettings &s) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    uint32_t stable = 0;
    for (int i = 0; i < 400 && std::chrono::steady_clock::now() < deadline; ++i) {
        r.render(c, s);
        auto &t = r.treeStats();
        require(!t.overflow && t.updates <= 4 && t.bytes <= 32ull * 1024 * 1024, "bounded streaming trees");
        auto &terrain = r.terrainStreamingStats();
        stable = t.resident == 4 && t.updates == 0 ? stable + 1 : 0;
        if (stable >= 6)
            return r.readTreeSnapshot();
    }
    const auto &t = r.treeStats();
    std::cerr << "resident " << t.resident << " pending " << t.pending << " updates " << t.updates
              << " misses " << t.surfaceMisses << '\n';
    throw std::runtime_error("Tree settle deadline");
}
void same(const TreeSnapshot &a, const TreeSnapshot &b) {
    uint32_t count = 0;
    for (auto &x : a.cells)
        for (auto &y : b.cells)
            if (x.key == y.key)
                for (uint32_t i = 0; i < 128; ++i) {
                    auto &p = x.population[i];
                    auto &q = y.population[i];
                    require(p.identity == q.identity && p.rootScale == q.rootScale && p.yaw == q.yaw &&
                                (p.assetCell & 31) == (q.assetCell & 31),
                            "population independent of rebase and residency");
                    ++count;
                }
    require(count == 512, "all returned cells compare");
}
} // namespace
int main() {
    try {
        RendererOptions o;
        o.ui = false;
        o.requireValidation = true;
        o.width = 128;
        o.height = 96;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.terrainGrid = {32, .5f};
        o.terrainCapacity = 32;
        o.lod = {true, 8, 128};
        o.streaming.enabled = o.streaming.hierarchical = true;
        o.streaming.diskCache = false;
        o.streaming.maximumDistance = 128;
        o.streaming.shadowCasterBytes = 0;
        o.streaming.gpuBytes = 40 * terrainMiB;
        o.streaming.hostBytes = 64 * terrainMiB;
        TreeRecipe recipe;
        recipe.height = 5;
        recipe.crownRadius = 1.5;
        recipe.attractors = 32;
        auto library = std::make_shared<TreeLibrary>();
        for (uint32_t species = 0; species < 4; ++species)
            for (uint32_t variant = 0; variant < 6; ++variant) {
                recipe.species = TreeSpecies(species);
                recipe.variant = variant;
                library->assets.push_back(generateTree(recipe));
            }
        o.trees.library = library;
        o.trees.streaming.cellCapacity = 4;
        o.trees.streaming.maximumDistance = 64;
        o.trees.streaming.budgetBytes = 32ull << 20;
        o.vegetation.library = std::make_shared<VegetationLibrary>(buildVegetationLibrary());
        o.vegetation.streaming.enabled = true;
        o.vegetation.streaming.cellCapacity = 64;
        o.vegetation.streaming.demandRadius = 8;
        o.vegetation.streaming.retentionRadius = 12;
        Renderer r(o);
        TerrainRequest terrain;
        terrain.settings.heightScale = 30;
        terrain.settings.erosionStrength = 0;
        r.requestTerrain(terrain);
        Camera c;
        c.local = {0, 25, 0};
        c.pitch = -.8;
        SceneSettings s;
        s.viewRadius = 1;
        s.frustumCulling = false;
        s.shadows.mode = ShadowMode::Off;
        s.trees.distance = 64;
        s.trees.coverage = 1;
        s.trees.wind = 0;
        auto first = settle(r, c, s);
        checkGround(r, first);
        uint32_t neighbors = 0, matching = 0;
        for (auto &cell : first.cells)
            for (uint32_t i = 0; i < 128; ++i)
                for (uint32_t j = 0; j < i; ++j) {
                    auto &a = cell.population[i];
                    auto &b = cell.population[j];
                    if (a.rootScale.w > 0 && b.rootScale.w > 0 &&
                        glm::length(glm::vec2(a.rootScale.x - b.rootScale.x, a.rootScale.z - b.rootScale.z)) <
                            12) {
                        ++neighbors;
                        matching += ((a.assetCell & 31) / 6) == ((b.assetCell & 31) / 6);
                    }
                }
        require(neighbors > 20 && matching > neighbors * .65,
                "coherent species stands rather than independent random trees");

        std::vector<glm::dvec2> roots;
        for (auto &cell : first.cells)
            for (auto &p : cell.population)
                if (p.rootScale.w > 0)
                    roots.push_back({cell.key.x * 64 + p.rootScale.x, cell.key.z * 64 + p.rootScale.z});
        for (size_t i = 0; i < roots.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                require(glm::length(roots[i] - roots[j]) >= 2.999, "cross-cell spacing competition");
        --c.chunk.x;
        c.local.x += 16;
        same(first, settle(r, c, s));
        c.chunk.x += 200;
        settle(r, c, s);
        c.chunk.x -= 200;
        auto returned = settle(r, c, s);
        same(first, returned);
        checkGround(r, returned);
        terrain.settings.heightScale = 0;
        r.requestTerrain(terrain);
        auto flat = settle(r, c, s);
        uint32_t valid = 0;
        for (auto &cell : flat.cells)
            for (auto a : cell.attachments)
                if (a.w > 0) {
                    near(a.x, 0, .0001, "terrain edit reattaches tree roots");
                    ++valid;
                }
        require(valid > 8, "flat terrain attachment fixture");
        require(r.validationErrors() == 0, "streamed tree validation clean");
        std::cout << "Tree streaming GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "renderer.hpp"
#include "test_support.hpp"
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
using namespace ow;
namespace {
VegetationSnapshot settle(Renderer &r, Camera &c, SceneSettings &s) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    uint32_t stable = 0;
    for (uint32_t i = 0; i < 1200 && std::chrono::steady_clock::now() < deadline; ++i) {
        s.vegetation.time += 1.f / 120;
        r.render(c, s);
        const auto &v = r.vegetationStats();
        require(v.updatedCells <= 32 && v.allocatedBytes <= (128ull << 20), "bounded streaming work/memory");
        require(!v.overflow, "bounded streamed visible/caster lists");
        require(r.validationErrors() == 0, "streamed vegetation Vulkan validation");
        const auto &terrain = r.terrainStreamingStats();
        stable = v.residentCells > 0 && v.pendingCells == 0 && v.updatedCells == 0 && terrain.pending == 0 &&
                         terrain.detailPending == 0 && terrain.ioJobs == 0
                     ? stable + 1
                     : 0;
        if (stable >= 4)
            return r.readVegetationSnapshot();
    }
    const auto &v = r.vegetationStats();
    std::cerr << "ready=" << v.residentCells << " pending=" << v.pendingCells
              << " missing=" << v.missingTerrainCells << " updates=" << v.updatedCells << '\n';
    throw std::runtime_error("Bounded vegetation settle deadline");
}
using Identity = std::tuple<int64_t, int64_t, uint32_t>;
std::map<Identity, FoliageInstance> population(const VegetationSnapshot &snapshot) {
    std::map<Identity, FoliageInstance> out;
    for (const auto &c : snapshot.cells)
        for (uint32_t i = 0; i < 128; ++i)
            out[{c.key.x, c.key.z, i}] = c.population[i];
    return out;
}
void samePopulation(const VegetationSnapshot &a, const VegetationSnapshot &b) {
    auto first = population(a), second = population(b);
    uint32_t compared = 0;
    for (const auto &[key, p] : first)
        if (auto it = second.find(key); it != second.end()) {
            const auto &q = it->second;
            require(p.identity == q.identity, "stable GPU candidate identity");
            near(glm::length(p.rootScale - q.rootScale), 0, 0, "stable accepted population");
            near(p.yaw, q.yaw, 0, "stable orientation");
            ++compared;
        }
    require(compared > 128, "overlapping cells actually compared");
}
void checkGround(Renderer &r, const VegetationSnapshot &snapshot) {
    using Key = std::tuple<int64_t, int64_t, uint32_t, uint32_t>;
    std::map<Key, std::vector<glm::vec2>> cache;
    uint32_t checked = 0, sloping = 0;
    for (const auto &cell : snapshot.cells) {
        std::vector<glm::vec3> triangles;
        for (uint32_t p = 0; p < cell.surface.count; ++p) {
            const auto &patch = cell.surface.patches[p];
            uint32_t tileLevel = uint32_t(std::lround(std::log2(patch.rectangle.z / .5f)));
            int64_t x = int64_t(std::llround((double(cell.key.x) * 4 + patch.rectangle.x) /
                                             (16.0 * double(1u << tileLevel))));
            int64_t z = int64_t(std::llround((double(cell.key.z) * 4 + patch.rectangle.y) /
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
            const auto &attachment = cell.attachment[i];
            require(p.identity == vegetationCandidateHash(cell.key, cell.surface.seed, i < 126 ? 0 : i - 125,
                                                          i < 126 ? i : 0),
                    "CPU/GPU integer identity agreement");
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
                near(attachment.y, -n.x / n.y, .0002, "attachment slope X");
                near(attachment.z, -n.z / n.y, .0002, "attachment slope Z");
                sloping += glm::length(glm::vec2(attachment.y, attachment.z)) > .0001;
                found = true;
                break;
            }
            require(found, "root has supporting terrain triangle");
            ++checked;
        }
    }
    require(checked > 128 && sloping > 64, "nontrivial sloping attachment fixture");
}
} // namespace
int main() {
    try {
        for (bool fixed : {false, true}) {
            RendererOptions o;
            o.ui = false;
            o.width = 128;
            o.height = 96;
            o.requireValidation = true;
            o.shaderDirectory = OPENWORLD_SHADER_DIR;
            o.forceFixedMaterials = fixed;
            o.terrainGrid = {32, .5f};
            o.terrainCapacity = 32;
            o.lod = {true, 8, 64};
            o.streaming.enabled = o.streaming.hierarchical = true;
            o.streaming.diskCache = false;
            o.streaming.maximumDistance = 64;
            o.streaming.shadowCasterBytes = 0;
            o.streaming.gpuBytes = 40 * terrainMiB;
            o.streaming.hostBytes = 64 * terrainMiB;
            o.vegetation.library = std::make_shared<VegetationLibrary>(buildVegetationLibrary());
            o.vegetation.streaming.enabled = o.vegetation.streaming.diagnostics = true;
            o.vegetation.streaming.cellCapacity = 64;
            o.vegetation.streaming.demandRadius = 8;
            o.vegetation.streaming.retentionRadius = 12;
            o.shadowCapacity.resolution = 128;
            Renderer r(o);
            TerrainRequest terrain;
            terrain.settings.heightScale = 8;
            terrain.settings.wavelength = 64;
            terrain.settings.regionWavelength = 4096;
            terrain.settings.erosionWavelength = 64;
            r.requestTerrain(terrain);
            Camera c;
            c.local = {1, 7, 10};
            c.pitch = -.5f;
            SceneSettings s;
            s.viewRadius = 1;
            s.frustumCulling = false;
            s.taa.enabled = true;
            s.vegetation.wind = .5f;
            auto first = settle(r, c, s);
            checkGround(r, first);
            require(first.stats.visible > 0, "streamed plants rendered");
            const auto fineReview = r.terrainLodReviewState();
            for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
                auto review = fineReview;
                std::set<std::tuple<int64_t, int64_t, uint32_t>> leaves;
                for (auto key : review.patches) {
                    if (key.level == 0 && (key.x >= 0) == bool(quadrant & 1) &&
                        (key.z >= 16) == bool(quadrant & 2)) {
                        key.x = terrainFloorDivide(key.x, 8) * 8;
                        key.z = terrainFloorDivide(key.z, 8) * 8;
                        key.level = 1;
                    }
                    leaves.emplace(key.x, key.z, key.level);
                }
                review.patches.clear();
                for (auto [x, z, level] : leaves)
                    review.patches.push_back({x, z, level});
                review.observer.local.x +=
                    128; // move the protected-ground demand away from the owned partition
                r.setTerrainLodReviewState(review);
                s.freezeLod = true;
                r.render(c, s);
                auto transition = r.readVegetationSnapshot();
                bool rejected = false;
                for (const auto &cell : transition.cells)
                    rejected |= !cell.historyValid;
                require(rejected, "changed attachments reject temporal correspondence locally");
                auto stitched = settle(r, c, s);
                samePopulation(first, stitched);
                checkGround(r, stitched);
                bool edge = false;
                for (const auto &cell : stitched.cells)
                    for (uint32_t j = 0; j < cell.surface.count; ++j)
                        edge |= cell.surface.patches[j].topology.x != 0;
                if (!edge) {
                    std::array<uint32_t, 16> levels{};
                    for (const auto &cell : stitched.cells)
                        for (uint32_t j = 0; j < cell.surface.count; ++j)
                            ++levels[cell.surface.patches[j].topology.z];
                    std::cerr << "Stitch fixture: " << stitched.cells.size() << " cells, level0 " << levels[0]
                              << ", level1 " << levels[1] << ", requested leaves " << review.patches.size()
                              << '\n';
                }
                require(edge, "fixture exercised real stitched boundaries");
            }
            s.freezeLod = false;
            first = settle(r, c, s);
            checkGround(r, first);

            s.vegetation.density = .5f;
            r.render(c, s);
            auto sparse = r.readVegetationSnapshot();
            samePopulation(first, sparse);
            require(sparse.stats.visible < first.stats.visible, "density affects visibility only");
            s.vegetation.density = 1;
            c.yaw = 1.7f;
            auto turned = settle(r, c, s);
            samePopulation(first, turned);
            s.vegetation.exclusion.centre = worldPosition(c, o.terrainGrid);
            s.vegetation.exclusion.radius = 3;
            s.vegetation.exclusion.revision++;
            auto cleared = settle(r, c, s);
            for (const auto &cell : cleared.cells)
                for (const auto &p : cell.population) {
                    auto offset =
                        relativePosition(vegetationCellOrigin(cell.key), s.vegetation.exclusion.centre);
                    if (glm::length(glm::vec2(offset.x + p.rootScale.x, offset.z + p.rootScale.z)) < 3)
                        require(p.rootScale.w == 0, "exclusion survives density/culling");
                }
            s.vegetation.exclusion.radius = 0;
            s.vegetation.exclusion.revision++;
            settle(r, c, s);
            c.chunk = {20, -20};
            r.render(c, s);
            auto teleport = r.readVegetationSnapshot();
            require(teleport.cells.empty(),
                    "teleport never displays stale roots while providers are missing");
            c.chunk = {};
            auto returned = settle(r, c, s);
            samePopulation(first, returned);
            checkGround(r, returned);
            // The existing terrain generator's signed noise lattice constrains
            // coordinates according to wavelength. Keep this fixture in range
            // while exercising nonzero high words in the vegetation cell key.
            terrain.settings.wavelength = 8192;
            terrain.settings.erosionWavelength = 8192;
            r.requestTerrain(terrain);
            c.chunk = {int64_t(1) << 30, -(int64_t(1) << 30)};
            auto distant = settle(r, c, s);
            for (const auto &cell : distant.cells)
                for (uint32_t i = 0; i < 128; ++i)
                    require(cell.population[i].identity ==
                                vegetationCandidateHash(cell.key, cell.surface.seed, i < 126 ? 0 : i - 125,
                                                        i < 126 ? i : 0),
                            "large integer GPU candidate identity");
            s.vegetation.enabled = false;
            r.render(c, s);
            require(!r.readVegetationSnapshot().stats.enabled, "disable stops drawing");
            s.vegetation.enabled = true;
            settle(r, c, s);
            require(r.validationErrors() == 0, "streaming synchronization validation");
        }
        std::cout << "Vegetation streamed GPU checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

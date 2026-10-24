#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <numeric>
#include <tuple>
using namespace ow;
namespace {
double ridge(double x, double z, uint32_t seed, double height) {
    double centre = 32 + .37 * z + seed * .17;
    if (seed & 8)
        centre += 3 * std::sin(z * .08);
    double d = (x - centre) / 1.5;
    return height * std::exp(-d * d);
}
double variation(const std::vector<double> &v) {
    double mean = std::accumulate(v.begin(), v.end(), 0.) / v.size(), sum = 0;
    for (auto h : v)
        sum += (h - mean) * (h - mean);
    return std::sqrt(sum / v.size());
}
double detailHeight(glm::dvec2 p, bool eroded) {
    constexpr double pi = 3.14159265358979323846;
    double h = .1 * p.x + .3 * std::sin(p.x * pi / 2) + .2 * std::cos(p.y * pi / 4);
    double d = (p.x - 8 - .37 * p.y) / 1.5;
    return h + (eroded ? 3 * std::exp(-d * d) : 0);
}
glm::dvec3 referenceNormal(glm::dvec2 p, double spacing, bool eroded, bool filtered) {
    const double delta = filtered ? spacing * .5 : spacing;
    const auto at = [&](glm::dvec2 q) {
        double dx = (detailHeight(q + glm::dvec2(delta, 0), eroded) -
                     detailHeight(q - glm::dvec2(delta, 0), eroded)) /
                    (2 * delta);
        double dz = (detailHeight(q + glm::dvec2(0, delta), eroded) -
                     detailHeight(q - glm::dvec2(0, delta), eroded)) /
                    (2 * delta);
        return glm::normalize(glm::dvec3(-dx, 1, -dz));
    };
    if (!filtered)
        return at(p);
    glm::dvec3 sum{};
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
            sum += at(p + glm::dvec2(x, z) * delta) * double((x == 0 ? 2 : 1) * (z == 0 ? 2 : 1));
    return glm::normalize(sum);
}
void shadingDetail(Renderer &r) {
    for (uint32_t tileLevel : {0u, 1u, 2u})
        for (uint32_t level : {0u, 1u, 2u, 3u}) {
            TerrainGrid grid{32, .5f * float(1u << tileLevel)};
            const double spacing = grid.spacing * .5 * double(1u << level);
            auto values = r.sampleTerrainNormalFixture(grid, level, tileLevel);
            const uint32_t width = (grid.cells * 2 >> level) + 1;
            double actualVariation = 0, referenceVariation = 0;
            for (uint32_t z = 0; z < width; ++z)
                for (uint32_t x = 0; x < width; ++x) {
                    const auto &v = values[z * width + x];
                    for (bool eroded : {false, true}) {
                        auto expected = referenceNormal(glm::dvec2(x, z) * spacing, spacing, eroded,
                                                        level + tileLevel != 0);
                        auto actual = glm::dvec3(eroded ? v.eroded : v.base);
                        near(glm::length(actual - expected), 0, .002,
                             "shading normal preserves independently filtered relief");
                        near(glm::length(actual), 1, .002, "normal remains unit length");
                        actualVariation += actual.x * actual.x + actual.z * actual.z;
                        referenceVariation += expected.x * expected.x + expected.z * expected.z;
                    }
                }
            near(actualVariation / referenceVariation, 1, .01, "normal slope energy retained, not flattened");
            std::cout << "normal tile=" << tileLevel << " level=" << level
                      << " slope energy ratio=" << actualVariation / referenceVariation << '\n';
        }
    // Same absolute normal level, independent of the data tile representation.
    auto a = r.sampleTerrainNormalFixture({32, .5f}, 2, 0);
    auto b = r.sampleTerrainNormalFixture({16, 1.f}, 1, 1);
    require(a.size() == b.size(), "matching absolute normal grids");
    for (size_t i = 0; i < a.size(); ++i) {
        near(glm::length(a[i].base - b[i].base), 0, 0, "absolute base normals agree");
        near(glm::length(a[i].eroded - b[i].eroded), 0, 0, "absolute eroded normals agree");
    }
    rejects([&] { r.sampleTerrainNormalFixture({32, .5f}, 16); }, "invalid normal level rejected");
}
void detailResidency() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = o.height = 96;
    // A 64 m tile footprint spans the protected region and exercises coarse
    // geometry. The previous 32 m fixture refined every tile to its finest level.
    o.terrainGrid = {128, .5f};
    o.terrainCapacity = 128;
    o.lod = {true, 32, 64};
    o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.vertexDiagnostics = true;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 512;
    o.streaming.diagnosticFrameSeconds = 1. / 60;
    Renderer r(o);
    Camera c;
    c.chunk = {40, 28};
    c.local = {32, 1, 32};
    c.pitch = -.65f;
    TerrainSettings terrain;
    terrain.heightScale = 30; // Owned gentle ground still contains measurable filtered-level differences.
    terrain.erosionStrength = .674f;
    r.setTerrainSettings(terrain);
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.viewRadius = 8;
    s.frustumCulling = false;
    s.terrainLodQuality = false;
    auto settle = [&] {
        int quiet = 0;
        const auto start = std::chrono::steady_clock::now();
        while (quiet < 30) {
            r.render(c, s);
            const auto &st = r.terrainStreamingStats();
            require(st.detailReady <= st.detailSelected && st.detailSelected <= st.detailEligible,
                    "detail diagnostics retain ready/selected/eligible ordering");
            require(st.detailSelected <= st.detailCapacity, "detail requests remain within pool");
            quiet = st.pending || st.detailPending || st.ioJobs ? 0 : quiet + 1;
            require(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 90,
                    "expanded detail demand converges");
        }
    };
    settle();
    const auto baseline = r.readTerrainResidency();
    const auto memory = r.terrainAllocatedBytes();
    std::cout << "baseline demand=" << r.terrainStreamingStats().detailEligible << ","
              << r.terrainStreamingStats().detailSelected << std::endl;
    s.terrainLodQuality = true;
    s.terrainLodPixelError = .5f;
    settle();
    require(r.terrainRenderStats().refined > 0, "fixture performs local geometry refinement");
    unsigned newDetails = 0;
    for (const auto &tile : r.readTerrainResidency()) {
        const auto old =
            std::find_if(baseline.begin(), baseline.end(), [&](const auto &b) { return b.key == tile.key; });
        if (old != baseline.end() && old->active && !old->detailWanted && tile.active && tile.detailWanted) {
            require(tile.detailResident, "newly eligible resident tile obtains normals");
            ++newDetails;
        }
    }
    std::cout << "expanded demand=" << r.terrainStreamingStats().detailEligible << ","
              << r.terrainStreamingStats().detailSelected << " scale=" << r.terrainRenderStats().qualityScale
              << std::endl;
    near(r.terrainStreamingStats().detailFirstRange, 64, 1e-3,
         "normal demand is independent of local geometry selection");
    require(r.terrainStreamingStats().detailReady == r.terrainStreamingStats().detailSelected,
            "all selected normals settle when the pool has capacity");
    std::cout << "new detail tiles after geometry expansion=" << newDetails << '\n';
    const float frozen = r.terrainRenderStats().qualityScale;
    s.freezeLod = true;
    for (int i = 0; i < 4; ++i)
        r.render(c, s);
    near(r.terrainRenderStats().qualityScale, frozen, 0, "stationary frozen scale is retained");
    const auto saved = r.terrainLodReviewState();
    const auto originalCamera = c;
    for (float yaw : {0.f, 1.57f, 3.14f, 4.71f}) {
        c.yaw = yaw;
        c.local.y += 15;
        r.render(c, s);
        require(r.terrainLodReviewState().patches == saved.patches,
                "turning frozen camera retains complete partition");
        require(r.terrainLodReviewState().observer.local == saved.observer.local,
                "turning frozen camera retains observer");
    }
    c = originalCamera;
    r.setTerrainLodReviewState(saved);
    r.render(c, s);
    require(r.terrainLodReviewState().patches == saved.patches, "restored review partition is exact");
    // A fixed viewing camera exposes observer-driven deformation independently
    // of texture reprojection or camera motion. Include both quality policies.
    using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    size_t compared = 0, farCompared = 0;
    for (bool quality : {false, true}) {
        s.terrainLodQuality = quality;
        std::map<Key, std::vector<glm::vec4>> previous;
        for (double movement : {0., .05, .1, .15, 0.}) {
            auto observer = saved.observer;
            observer.local.x += movement;
            r.setTerrainLodObserver(observer);
            r.render(c, s);
            const auto positions = r.readTerrainVertexPositions();
            size_t offset = 0;
            std::map<Key, std::vector<glm::vec4>> current;
            for (const auto &p : r.readTerrainPatches()) {
                const size_t count = (p.cells + 1) * (p.cells + 1);
                Key key{p.slot, p.x, p.z, p.level, p.edges, p.corners};
                auto it = previous.find(key);
                if (it != previous.end())
                    for (size_t j = 0; j < count; ++j) {
                        require(positions[offset + j] == it->second[j],
                                "Unchanged ground patches never deform when only observer moves");
                        ++compared;
                        if (p.level > 0)
                            ++farCompared;
                    }
                current.emplace(key, std::vector<glm::vec4>(positions.begin() + offset,
                                                            positions.begin() + offset + count));
                offset += count;
            }
            require(offset == positions.size(), "Diagnostic vertices follow actual patch dimensions");
            previous = std::move(current);
        }
    }
    require(compared > 1000 && farCompared > 100, "Stability fixture exercises near and coarser geometry");
    r.setTerrainLodReviewState(saved);
    r.render(c, s);
    // Inspect from above the terrain while preserving the selected LOD observer.
    const auto centreHeights = r.readStreamedHeights(c.chunk);
    c.local.y = centreHeights[64 * 129 + 64].y + 8;
    c.pitch = -1.35f;
    // Inspect close enough to resolve the wireframe at this small framebuffer.
    // Selection remains frozen, including outer patches and parent/child seams.
    for (bool wire : {false, true}) {
        s.wireframe = wire;
        s.opaqueDepthPrepass = false;
        r.render(c, s);
        const auto reference = r.readHdrSnapshot();
        const auto triangles = r.terrainRenderStats().triangles;
        s.opaqueDepthPrepass = true;
        r.render(c, s);
        const auto prepass = r.readHdrSnapshot();
        float error = 0, variation = 0;
        for (size_t i = 0; i < reference.pixels.size(); ++i) {
            error = std::max(error, glm::length(reference.pixels[i] - prepass.pixels[i]));
            variation = std::max(variation, glm::length(reference.pixels[i] - reference.pixels.front()));
        }
        require(variation > .001f, "hierarchical prepass fixture contains visible terrain");
        require(error < .003f, "hierarchical depth prepass preserves clipping and morph coverage");
        require(r.terrainRenderStats().triangles == triangles, "hierarchical depth prepass reuses selection");
    }
    s.wireframe = s.opaqueDepthPrepass = false;
    s.terrainLodQuality = false;
    settle();
    near(r.terrainRenderStats().qualityScale, 1, 0, "disabled quality restores baseline geometry");
    near(r.terrainStreamingStats().detailFirstRange, 64, 0,
         "disabled quality restores baseline normal range");
    const auto disabled = r.readTerrainResidency();
    for (const auto &tile : baseline)
        if (tile.active) {
            const auto it = std::find_if(disabled.begin(), disabled.end(),
                                         [&](const auto &t) { return t.key == tile.key; });
            require(it != disabled.end() && it->detailWanted == tile.detailWanted,
                    "disabled quality restores original demand");
        }
    require(memory == r.terrainAllocatedBytes() && !r.validationErrors(),
            "expanded detail stays bounded and validated");
}
} // namespace
int main() {
    try {
        RendererOptions o;
        o.ui = false;
        o.requireValidation = true;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.width = o.height = 32;
        o.terrainGrid = {8, .5f};
        o.terrainCapacity = 256;
        Renderer r(o);
        shadingDetail(r);
        for (uint32_t seed : {0u, 1u, 3u, 8u, 11u}) {
            TerrainRequest request;
            request.settings.seed = seed;
            request.settings.heightScale = 50;
            const auto fine = r.sampleTerrainHeightLevel({256, .5f}, request, 0, true);
            for (uint32_t level : {1u, 2u, 3u, 4u}) {
                const auto filtered = r.sampleTerrainHeightLevel({256, .5f}, request, level, true);
                const uint32_t step = 1u << level, width = 256 / step + 1;
                const double spacing = .5 * step;
                std::vector<double> oldCrest, newCrest;
                for (uint32_t z = 2; z + 2 < width; ++z) {
                    double before = 0, after = 0;
                    for (uint32_t x = 2; x + 2 < width; ++x) {
                        double expected = 0;
                        const double w[5] = {1, 4, 6, 4, 1};
                        for (int dz = -2; dz <= 2; ++dz)
                            for (int dx = -2; dx <= 2; ++dx)
                                expected +=
                                    ridge((x + dx * .5) * spacing, (z + dz * .5) * spacing, seed, 50) *
                                    w[dx + 2] * w[dz + 2] / 256;
                        near(filtered[z * width + x].x, expected, .0003,
                             "GPU filter agrees with independent analytic footprint");
                        near(filtered[z * width + x].x, filtered[z * width + x].y, 0,
                             "both terrain channels filtered identically");
                        before = std::max(before, double(fine[(z * step) * 257 + x * step].x));
                        after = std::max(after, double(filtered[z * width + x].x));
                    }
                    oldCrest.push_back(before);
                    newCrest.push_back(after);
                }
                const double oldVariation = variation(oldCrest), newVariation = variation(newCrest);
                std::cout << "seed=" << seed << " level=" << level << " crest variation " << oldVariation
                          << " -> " << newVariation << '\n';
                if (level >= 3)
                    require(newVariation < oldVariation * .2,
                            "at least 80 percent less coarse crest sawtooth variation");
            }
            for (uint32_t z = 0; z < 257; z += 17)
                for (uint32_t x = 0; x < 257; x += 17)
                    near(fine[z * 257 + x].x, ridge(x * .5, z * .5, seed, 50), .0003,
                         "level zero preserves ridge definition");
        }
        TerrainRequest request{{-20, 86}, {}};
        request.settings.erosionWavelength = 1133;
        request.settings.erosionStrength = .674;
        request.settings.creaseRounding = .270;
        const auto base = r.sampleTerrainHeightLevel({128, .5f}, request, 0);
        const auto reference = r.sampleTerrainGeography({128, .5f}, request);
        for (size_t i = 0; i < base.size(); ++i) {
            near(base[i].x, reference[i].base.w, 0, "procedural base unchanged at level zero");
            near(base[i].y, reference[i].detail.w, 0, "procedural erosion unchanged at level zero");
        }
        request.settings.heightScale = 0;
        for (const auto &h : r.sampleTerrainHeightLevel({128, .5f}, request, 4))
            require(h == glm::vec2(0), "filter preserves flat terrain");
        rejects([&] { r.sampleTerrainHeightLevel({128, .5f}, request, 15); }, "invalid probe level rejected");
        require(!r.validationErrors(), "height quality probe Vulkan validation");
        detailResidency();
        std::cout << "Terrain LOD quality GPU checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

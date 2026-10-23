#include "renderer.hpp"
#include "terrain_generation_budget.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
using namespace ow;
namespace {
bool observedMultipleStages = false;
struct Temp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ow-hierarchy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() {
        if (std::uncaught_exceptions()) {
            std::cerr << "Retained fixture " << path << "\n";
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
RendererOptions options(const Temp &t) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 64};
    o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.maximumDistance = 128;
    o.streaming.vertexDiagnostics = true;
    o.streaming.gpuBytes = 40 * terrainMiB;
    o.streaming.hostBytes = 64 * terrainMiB;
    o.streaming.transferBytesPerFrame = 32768;
    o.streaming.cacheDirectory = t.path / "cache";
    return o;
}
void settle(Renderer &r, Camera &c, SceneSettings &s) {
    int stable = 0;
    auto quiet = std::chrono::steady_clock::now();
    for (int i = 0; i < 12000; ++i) {
        r.render(c, s);
        const auto &a = r.terrainStreamingStats();
        require(a.transferBytes <= 32768, "hierarchy transfer budget");
        require(a.dispatches <= TerrainGenerationBudget::maximumDispatches, "hierarchy dispatch budget");
        require(a.generationGroups <= a.generationGroupBudget &&
                    a.generationGroupBudget <= TerrainGenerationBudget::maximumGroups,
                "hierarchy measured work budget");
        observedMultipleStages |= a.generationSegments > 1;
        require(a.generationSegments <= TerrainGenerationBudget::maximumSegments, "bounded stage metadata");
        require(a.generationEstimatedMilliseconds <= TerrainGenerationBudget::targetMilliseconds + 1e-8 ||
                    a.generationGroups == 1, "shared frame budget except one minimum-progress group");
        require(r.validationErrors() == 0, "hierarchy Vulkan validation");
        const auto &quality = r.terrainRenderStats();
        require(quality.triangles <= quality.baselineTriangles * 2, "quality stays within twice baseline geometry");
        require(quality.qualityScale >= 1 && quality.qualityScale <= 2 && std::isfinite(quality.maximumErrorPixels),
                "quality ranges and projected errors stay finite and bounded");
        stable = a.pending == 0 && a.detailPending == 0 && a.ioJobs == 0 ? stable + 1 : 0;
        if (!stable)
            quiet = std::chrono::steady_clock::now();
        if (stable > 100 && std::chrono::steady_clock::now() - quiet > std::chrono::milliseconds(1400))
            return;
    }
    const auto &a = r.terrainStreamingStats();
    std::cerr << "pending=" << a.pending << " detail=" << a.detailPending << " resident=" << a.coverage
              << " gen=" << a.generations << '\n';
    throw std::runtime_error("Hierarchy convergence deadline");
}
void checkSamples(Renderer &r, Camera &c) {
    const auto states = r.readTerrainResidency();
    bool compared = false;
    for (const auto &s : states) {
        if (!s.resident || s.key.level != 0 || s.key.x != c.chunk.x || s.key.z != c.chunk.z)
            continue;
        require(s.active, "near tile active");
        auto parent = terrainParent(s.key);
        const auto child = r.readStreamedTileHeights(s.key, 1), coarse = r.readStreamedTileHeights(parent);
        const auto qx = uint32_t(s.key.x - parent.x * 2) * 16, qz = uint32_t(s.key.z - parent.z * 2) * 16;
        for (uint32_t z = 0; z <= 32; z += 2)
            for (uint32_t x = 0; x <= 32; x += 2) {
                auto a = child[(z / 2) * 17 + x / 2], b = coarse[(qz + z / 2) * 33 + qx + x / 2];
                near(a.x, b.x, 0, "same absolute filtered base samples are exact");
                near(a.y, b.y, 0, "same absolute filtered erosion samples are exact");
            }
        compared = true;
        break;
    }
    require(compared, "near parent/child data available");
    bool normalsCompared = false;
    for (const auto &child : states) {
        if (!child.active || child.key.level != 0)
            continue;
        const auto parent = terrainParent(child.key);
        try {
            const auto cn = r.readStreamedTileNormals(child.key, 2);
            const auto pn = r.readStreamedTileNormals(parent, 1);
            const auto qx = uint32_t(child.key.x - parent.x * 2) * 16;
            const auto qz = uint32_t(child.key.z - parent.z * 2) * 16;
            for (uint32_t layer = 0; layer < 2; ++layer)
                for (uint32_t z = 0; z < 17; ++z)
                    for (uint32_t x = 0; x < 17; ++x)
                        near(glm::length(cn[layer * 17 * 17 + z * 17 + x] -
                                         pn[layer * 33 * 33 + (qz + z) * 33 + qx + x]),
                             0, 0, "absolute normal levels agree across data tiles");
            normalsCompared = true;
            break;
        } catch (const std::invalid_argument &) { /* Covered ancestors need no fine-normal residency. */
        }
    }
    require(normalsCompared, "mixed-level normal comparison available");
}
void sharedVertices(Renderer &r) {
    const auto positions = r.readTerrainVertexPositions();
    const auto tiles = r.readTerrainResidency();
    const auto patches = r.readTerrainPatches();
    std::map<std::pair<int64_t, int64_t>, glm::vec4> points;
    size_t offset = 0;
    for (auto patch : patches) {
        const auto tile =
            *std::find_if(tiles.begin(), tiles.end(), [&](auto t) { return t.handle.index == patch.slot; });
        const int64_t scale = int64_t(1) << tile.key.level;
        for (uint32_t z = 0; z <= 8; ++z)
            for (uint32_t x = 0; x <= 8; ++x) {
                bool used = false;
                for (uint32_t q = 0; q < 4; ++q)
                    if (patch.mask & (1u << q))
                        used |= x >= (q & 1) * 4 && x <= (q & 1) * 4 + 4 && z >= (q >> 1) * 4 &&
                                z <= (q >> 1) * 4 + 4;
                if (!used)
                    continue;
                const auto key = std::make_pair((tile.key.x * 32 + patch.x + x * patch.step) * scale,
                                                (tile.key.z * 32 + patch.z + z * patch.step) * scale);
                const auto value = positions[offset + z * 9 + x];
                auto [it, inserted] = points.emplace(key, value);
                if (!inserted)
                    near(glm::length(it->second - value), 0, 1e-5,
                         "shared GPU vertices stay connected through residency "
                         "transitions");
            }
        offset += 81;
    }
}
void noSkyHoles(Renderer &r, const std::filesystem::path &path) {
    sharedVertices(r);
    r.capture(path);
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    int width, height, max;
    f >> magic >> width >> height >> max;
    f.get();
    require(magic == "P6" && max == 255, "owned capture format");
    std::vector<unsigned char> pixels(size_t(width) * height * 3);
    f.read(reinterpret_cast<char *>(pixels.data()), std::streamsize(pixels.size()));
    require(bool(f), "complete capture");
    for (size_t i = 0; i < pixels.size(); i += 3)
        require(!(pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255),
                "no exposed sky through transition mesh");
}
void run() {
    Temp temp;
    auto o = options(temp);
    Camera c;
    c.chunk = {-4, 1000000};
    c.local = {8, 250, 8};
    c.pitch = -1.55f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.shading = ShadingMode::Legacy;
    s.viewRadius = 8;
    s.frustumCulling = true;
    std::vector<glm::vec2> first;
    uint64_t skippedExports = 0;
    // Pre-sample the movement path without waits during residency transitions.
    // The regional generator can descend much more steeply than the old noise;
    // follow the surface so this test sees seams, not the finite coverage edge.
    std::vector<TerrainGeographySample> path;
    {
        auto probeOptions = o;
        probeOptions.streaming.enabled = false;
        probeOptions.lod.enabled = false;
        probeOptions.terrainCapacity = 128;
        probeOptions.terrainGrid = {128, 1};
        Renderer probe(probeOptions);
        path = probe.sampleTerrainGeography({128, 1}, {{-1, 125000}, {}});
    }
    {
        Renderer r(o);
        auto memory = r.terrainAllocatedBytes();
        settle(r, c, s);
        require(observedMultipleStages, "dependent generation stages share a frame");
        checkSamples(r, c);
        first = r.readStreamedHeights(c.chunk);
        require(r.terrainStreamingStats().generations > 0, "cold hierarchy generated");
        require(r.terrainStreamingStats().gpuBytes <= o.streaming.gpuBytes, "GPU storage budget");
        require(r.terrainStreamingStats().hostBytes <= o.streaming.hostBytes, "host storage budget");
        float highest = 0;
        for (auto h : first)
            highest = std::max(highest, h.y);
        c.local.y = highest + 35;
        c.pitch = -1.5707f;
        s.background[0] = s.background[2] = 1;
        s.background[1] = 0;
        r.render(c, s);
        noSkyHoles(r, temp.path / "initial.ppm");
        for (int i = 0; i < 48; ++i) {
            c.local.x += 1.0;
            c.local.z += i < 24 ? .4 : -.4;
            c.update({}, 0, o.terrainGrid);
            const auto px = uint32_t(c.chunk.x * 16 + c.local.x + 128);
            const auto pz = uint32_t((c.chunk.z - 1000000) * 16 + c.local.z);
            // Keep the camera footprint inside the finite test coverage on steep
            // slopes.
            c.local.y = path[pz * 129 + px].detail.w + 5;
            r.render(c, s);
            if (i % 6 == 0) {

                noSkyHoles(r, temp.path / "transition.ppm");
            }
        }
        // A previously blocked parent neighbourhood at this position left all
        // fine products resident but inactive indefinitely (LOD 3 at the eye).
        c.chunk = {2, -6};
        c.local = {8, 250, 8};
        settle(r, c, s);
        checkSamples(r, c);
        const auto nearNormals = r.readStreamedTileNormals({2, -6, 0}, 0);
        require(!nearNormals.empty(), "moved camera obtains finest shading normals");
        const auto movedHeights = r.readStreamedHeights(c.chunk);
        highest = 0;
        for (auto h : movedHeights)
            highest = std::max(highest, h.y);
        c.local.y = highest + 10;
        r.render(c, s);
        noSkyHoles(r, temp.path / "moved-refined.ppm");
        require(r.terrainRenderStats().perLevel[0] > 0, "moved camera draws finest LOD after settling");
        auto changed = TerrainSettings{};
        changed.seed = 17;
        for (int i = 0; i < 18; ++i) {
            c.chunk.x = i % 2 ? -1000 : 1000;
            s.viewRadius = i % 3 ? 8 : 0;
            if (i % 4 == 0) {
                changed.seed++;
                r.setTerrainSettings(changed);
            }
            r.render(c, s);
        }
        r.setTerrainSettings({});
        c.chunk = {-4, 1000000};
        c.local = {8, 250, 8};
        s.viewRadius = 8;
        settle(r, c, s);
        checkSamples(r, c);
        require(r.terrainAllocatedBytes() == memory, "movement does not grow allocations");
        require(r.validationErrors() == 0, "cancel/reuse validation clean");
        skippedExports = r.terrainStreamingStats().skippedExports;
    }
    {
        Renderer r(o);
        settle(r, c, s);
        checkSamples(r, c);
        std::cerr << "warm generations=" << r.terrainStreamingStats().generations
                  << " hits=" << r.terrainStreamingStats().hits
                  << " misses=" << r.terrainStreamingStats().misses << "\n";
        require(r.terrainStreamingStats().generations <= skippedExports &&
                    r.terrainStreamingStats().hits > r.terrainStreamingStats().generations,
                "warm hierarchy reuses cache; skipped optional exports may regenerate");
        auto again = r.readStreamedHeights(c.chunk);
        require(again == first, "cache round trip preserves sampled terrain");
    }
    {
        auto baseline = o;
        baseline.streaming.enabled = false;
        Renderer r(baseline);
        r.requestTerrain({c.chunk, {}});
        for (int i = 0; i < 4; ++i)
            r.render(c, s);
        const auto samples = r.readTerrainSnapshot();
        require(samples.size() == first.size(), "baseline sample dimensions");
        for (size_t i = 0; i < first.size(); ++i) {
            near(first[i].x, samples[i].base.w, .01, "canonical base preserves established world");
            near(first[i].y, samples[i].eroded.w, .01, "canonical erosion preserves established world");
        }
    }
}
void pressure() {
    Temp temp;
    auto o = options(temp);
    o.streaming.gpuBytes = 4 * terrainMiB;
    o.streaming.maximumDistance = 64;
    o.streaming.diskCache = false;
    Renderer r(o);
    TerrainSettings flat;
    flat.heightScale = 0;
    flat.erosionStrength = 0;
    r.setTerrainSettings(flat);
    Camera c;
    c.local = {8, 25, 8};
    c.pitch = -1.5707f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.shading = ShadingMode::Legacy;
    s.viewRadius = 4;
    s.background[0] = s.background[2] = 1;
    s.background[1] = 0;
    settle(r, c, s);
    require(r.terrainStreamingStats().detailCapacity < r.terrainStreamingStats().coverage,
            "fixture forces normal residency pressure");
    const auto memory = r.terrainAllocatedBytes();
    for (int i = 0; i < 30; ++i) {
        c.local.x += 1;
        c.update({}, 0, o.terrainGrid);
        r.render(c, s);
        if (i % 10 == 0)
            noSkyHoles(r, temp.path / "pressure.ppm");
    }
    require(r.terrainAllocatedBytes() == memory && !r.validationErrors(),
            "pressure retains bounded validated storage");
}
void ridgeTransitions() {
    Temp temp;
    auto o = options(temp);
    TerrainSettings settings;
    settings.seed = 34434;
    settings.ridgeStrength = settings.baseDetail = 1; // Preserve the steep ridge movement regression.
    settings.erosionWavelength = 1572;
    settings.erosionStrength = .229f;
    std::vector<TerrainGeographySample> path;
    {
        auto p = o;
        p.streaming.enabled = false;
        p.lod.enabled = false;
        p.terrainCapacity = 128;
        p.terrainGrid = {128, 1};
        Renderer probe(p);
        path = probe.sampleTerrainGeography(p.terrainGrid, {{-51, 5}, settings});
    }
    Renderer r(o);
    r.setTerrainSettings(settings);
    Camera c;
    c.chunk = {-406, 42};
    c.local = {8, 780, 8};
    c.pitch = -1.5707f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.shading = ShadingMode::Legacy;
    s.viewRadius = 8;
    s.background[0] = s.background[2] = 1;
    s.background[1] = 0;
    settle(r, c, s);
    checkSamples(r, c);
    const auto memory = r.terrainAllocatedBytes();
    for (int frame = 0; frame < 32; ++frame) {
        c.local.x += frame < 16 ? 1 : -1;
        c.update({}, 0, o.terrainGrid);
        const auto x = uint32_t(c.chunk.x * 16 + c.local.x + 6528);
        const auto z = uint32_t(c.chunk.z * 16 + c.local.z - 640);
        c.local.y = std::max(path[z * 129 + x].base.w, path[z * 129 + x].detail.w) + 5;
        for (bool eroded : {false, true}) {
            s.eroded = eroded;
            r.render(c, s);
            if (frame % 8 == 0)
                noSkyHoles(r, temp.path / "ridge-transition.ppm");
        }
    }
    require(r.terrainAllocatedBytes() == memory && !r.validationErrors(),
            "Ridge transitions retain bounded validated storage");
}
} // namespace
int main() {
    try {
        run();
        pressure();
        ridgeTransitions();
        std::cout << "Terrain hierarchy GPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

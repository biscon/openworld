#include "renderer.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace ow;
namespace {
struct Fixture {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ow-material-gpu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(path);
        write(false);
    }
    ~Fixture() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
    }
    void write(bool invert) {
        const char *names[] = {"colour_rgba.dds", "normal_rgba.dds", "scalar_rgba.dds"};
        for (int semantic = 0; semantic < 3; ++semantic) {
            TextureData t;
            t.width = t.height = 8;
            t.levels = 4;
            t.layers = 4;
            t.format = semantic == 0 ? TextureFormat::Srgb8 : TextureFormat::Rgba8;
            for (uint32_t layer = 0; layer < 4; ++layer)
                for (uint32_t mip = 0; mip < 4; ++mip) {
                    uint32_t n = 8 >> mip;
                    for (uint32_t y = 0; y < n; ++y)
                        for (uint32_t x = 0; x < n; ++x) {
                            std::array<uint8_t, 4> p = semantic == 1
                                                           ? std::array<uint8_t, 4>{128, 128, 255, 255}
                                                           : std::array<uint8_t, 4>{200, 255, 128, 255};
                            if (semantic == 0) {
                                p = {30, 30, 30, 255};
                                p[layer % 3] = (x + y) % 2 ? 200 : 100;
                                if (invert)
                                    for (int c = 0; c < 3; ++c)
                                        p[c] = 255 - p[c];
                            }
                            t.bytes.insert(t.bytes.end(), p.begin(), p.end());
                        }
                }
            writeTexture(path / names[semantic], t);
        }
    }
    void writeMacro() {
        TextureData t;
        t.width = t.height = 8;
        t.levels = 4;
        t.layers = 4;
        t.format = TextureFormat::Srgb8;
        for (uint32_t layer = 0; layer < 4; ++layer)
            for (uint32_t mip = 0; mip < 4; ++mip)
                for (uint32_t y = 0; y < (8u >> mip); ++y)
                    for (uint32_t x = 0; x < (8u >> mip); ++x)
                        t.bytes.insert(t.bytes.end(), {uint8_t(100 + x * 12), uint8_t(210 - y * 8),
                                                       uint8_t(140 + layer * 15), uint8_t(80 + x * 10)});
        writeTexture(path / "macro_rgba.dds", t);
    }
};
float difference(const HdrSnapshot &a, const HdrSnapshot &b) {
    require(a.pixels.size() == b.pixels.size(), "snapshot size");
    float m = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i)
        for (int c = 0; c < 3; ++c) {
            require(std::isfinite(b.pixels[i][c]), "finite material shading");
            m = std::max(m, std::abs(a.pixels[i][c] - b.pixels[i][c]));
        }
    return m;
}
void checkDepthPrepass(Renderer &r, const Camera &camera, SceneSettings s) {
    for (bool wire : {false, true}) {
        s.wireframe = wire;
        s.opaqueDepthPrepass = false;
        r.render(camera, s);
        const auto reference = r.readHdrSnapshot();
        size_t varied = 0;
        for (const auto &pixel : reference.pixels)
            varied += glm::length(pixel - reference.pixels.front()) > .001f;
        require(varied > 100, "prepass fixture contains visible terrain");
        const auto selected = r.terrainRenderStats().selected;
        const auto generations = r.terrainGenerationCount();
        s.opaqueDepthPrepass = true;
        r.render(camera, s);
        require(difference(reference, r.readHdrSnapshot()) < .003,
                "depth prepass preserves textured and wireframe output");
        require(r.terrainRenderStats().selected == selected && r.terrainGenerationCount() == generations,
                "depth prepass preserves selection and generation");
        s.opaqueDepthPrepass = false;
        r.render(camera, s);
        require(difference(reference, r.readHdrSnapshot()) < .003,
                "depth prepass can be disabled on the next frame");
    }
}
std::array<HdrSnapshot, 2> run(Fixture &fixture, bool fixed) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 128;
    o.height = 96;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.materialDirectory = fixture.path;
    o.forceRgbaMaterials = true;
    o.forceFixedMaterials = fixed;
    o.forceTrilinear = true;
    Renderer r(o);
    TerrainRequest terrain;
    terrain.settings.heightScale = 0;
    r.requestTerrain(terrain);
    Camera camera;
    camera.local = {8, 10, 18};
    camera.pitch = -.65;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.terrainMaterial.isolate = 0;
    s.terrainMaterial.filtering = 2;
    for (int i = 0; i < 3; ++i)
        require(r.render(camera, s), "initial material render");
    auto original = r.readHdrSnapshot();
    auto generations = r.terrainGenerationCount();
    const auto residentBytes = r.materialStats().imageBytes;
    require(r.materialStats().loaded && r.materialStats().effectiveAnisotropy == 1,
            "loaded palette and anisotropy feature fallback");
    s.terrainMaterial.isolate = -1;
    s.terrainMaterial.weights = true;
    r.render(camera, s);
    auto originalWeights = r.readHdrSnapshot();
    auto diagnosticCapture = [&] {
        const auto path = fixture.path / "diagnostic.ppm";
        r.capture(path);
        std::ifstream in(path, std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const auto acesDiagnostic = diagnosticCapture();
    s.output.toneMapper = ToneMapper::Agx;
    s.output.ev100 = 10;
    r.render(camera, s);
    require(acesDiagnostic == diagnosticCapture(), "material diagnostics bypass tone mapping and exposure");
    s.output = {};
    camera.chunk.x = -1;
    camera.local.x += 16;
    r.render(camera, s);
    require(difference(originalWeights, r.readHdrSnapshot()) < .003, "classifier rebase continuity");
    camera.chunk.x = 0;
    camera.local.x -= 16;
    s.terrainMaterial.isolate = 0;
    s.terrainMaterial.weights = false;
    camera.chunk.x = -1;
    camera.local.x += 16;
    r.render(camera, s);
    auto rebased = r.readHdrSnapshot();
    require(difference(original, rebased) < .003, "UV rebase continuity");
    s.terrainMaterial.isolate = 1;
    r.render(camera, s);
    auto soil = r.readHdrSnapshot();
    require(difference(original, soil) > .01, "material layers differ");
    s.terrainMaterial.enabled = false;
    r.render(camera, s);
    require(difference(soil, r.readHdrSnapshot()) > .01, "constant comparison");
    require(r.terrainGenerationCount() == generations, "material edits do not regenerate terrain");
    terrain.chunk = {-1000000, 1000000};
    r.requestTerrain(terrain);
    camera.chunk = terrain.chunk;
    camera.local = {8, 10, 18};
    s.terrainMaterial.enabled = true;
    s.terrainMaterial.isolate = 0;
    r.render(camera, s);
    require(difference(original, r.readHdrSnapshot()) < .003, "distant negative texture phase");
    terrain.chunk = {};
    camera.chunk = {};
    r.requestTerrain(terrain);
    r.render(camera, s);
    s.terrainMaterial.enabled = true;
    s.terrainMaterial.isolate = 0;
    require(!r.reloadMaterials(fixture.path / "missing"), "missing load rejected");
    require(r.materialStats().loaded, "failed replacement retains old palette");
    // Submit reads in both slots before replacing the palette; no diagnostic wait
    // until after replacement.
    r.render(camera, s);
    r.render(camera, s);
    fixture.write(true);
    require(r.reloadMaterials(fixture.path), "palette replacement");
    r.render(camera, s);
    r.render(camera, s);
    r.render(camera, s);
    require(difference(original, r.readHdrSnapshot()) > .01, "new palette published");
    fixture.write(false);
    require(r.materialStats().imageBytes == residentBytes, "retired palette released after all readers");

    const auto before5C = r.terrainGenerationCount();
    fixture.writeMacro();
    require(r.reloadMaterials(fixture.path), "macro palette replacement");
    applyMaterialPreset(s.terrainMaterial, TerrainMaterialPreset::High);
    s.terrainMaterial.filtering = 0;
    r.render(camera, s);
    auto highImage = r.readHdrSnapshot();
    checkDepthPrepass(r, camera, s);
    require(r.materialStats().macroLoaded, "macro palette published");
    require(difference(original, highImage) > .005, "macro sampling affects material");
    --camera.chunk.x;
    camera.local.x += 16;
    ++camera.chunk.z;
    camera.local.z -= 16;
    r.render(camera, s);
    require(difference(highImage, r.readHdrSnapshot()) < .003, "macro rebase continuity");
    ++camera.chunk.x;
    camera.local.x -= 16;
    --camera.chunk.z;
    camera.local.z += 16;
    const auto bytesWithMacro = r.materialStats().imageBytes;
    {
        std::ofstream broken(fixture.path / "macro_rgba.dds", std::ios::binary | std::ios::trunc);
        broken << "bad";
    }
    require(!r.reloadMaterials(fixture.path), "malformed macro rejects replacement");
    r.render(camera, s);
    require(difference(highImage, r.readHdrSnapshot()) < .003 && r.materialStats().macroLoaded,
            "failed macro replacement retains published palette");
    std::filesystem::remove(fixture.path / "macro_rgba.dds");
    require(r.reloadMaterials(fixture.path), "old palette without macro still loads");
    applyMaterialPreset(s.terrainMaterial, TerrainMaterialPreset::Balanced);
    s.terrainMaterial.filtering = 0;
    for (int i = 0; i < 3; ++i)
        r.render(camera, s);
    require(!r.materialStats().macroLoaded && r.materialStats().imageBytes <= bytesWithMacro,
            "macro removed and retired with frame fences");
    require(r.terrainGenerationCount() == before5C, "5C appearance never regenerates terrain");

    // Seed 1 crosses an octave-feedback saturation contour here. Heights are
    // continuous, but classifying their unsmoothed analytic derivative used to
    // switch grass to rock within a single pixel. A flat fixture misses this.
    terrain = {{0, 341}, {}}; // 16 m tile: world (0, 5456).
    terrain.settings.ridgeStrength = terrain.settings.baseDetail = 1;
    terrain.settings.erosionStrength = 0;
    r.requestTerrain(terrain);
    camera.chunk = terrain.chunk;
    camera.local = {7.25, 651, 12.5};
    camera.pitch = -1.5707963f;
    s.terrainMaterial.isolate = -1;
    s.terrainMaterial.weights = true;
    r.render(camera, s);
    const auto mountainWeights = r.readHdrSnapshot();
    float largestStep = 0, low = 1, high = 0;
    for (uint32_t y = 24; y < 72; ++y)
        for (uint32_t x = 40; x < 88; ++x) {
            const size_t i = y * o.width + x;
            // The central footprint is entirely terrain, spans the contour,
            // and samples it at approximately 0.12 metres per pixel.
            for (int ch = 0; ch < 3; ++ch) {
                require(std::isfinite(mountainWeights.pixels[i][ch]), "finite mountain coverage");
                for (size_t neighbour : {i + 1, i + o.width})
                    largestStep = std::max(largestStep, std::abs(mountainWeights.pixels[i][ch] -
                                                                 mountainWeights.pixels[neighbour][ch]));
            }
            low = std::min(low, mountainWeights.pixels[i].g);
            high = std::max(high, mountainWeights.pixels[i].g);
        }
    std::cout << "Mountain coverage max adjacent step=" << largestStep << ", range=" << high - low << '\n';
    // HDR snapshots are half precision. 0.03 allows numerical/rasterization
    // variation but rejects the old ~0.5 colour discontinuity by a wide margin.
    require(largestStep < .03f, "mountain material coverage has no hard feedback boundary");
    require(high - low > .03f, "mountain fixture retains varying coverage");
    --camera.chunk.x;
    camera.local.x += o.terrainGrid.extent();
    r.render(camera, s);
    require(difference(mountainWeights, r.readHdrSnapshot()) < .003f,
            "non-flat classifier rebase continuity");
    require(r.validationErrors() == 0, "material Vulkan validation");
    return {original, highImage};
}
HdrSnapshot numericalFixture(Fixture &fixture, bool fixed) {
    fixture.writeMacro();
    const auto shaderDir = fixture.path / "shaders";
    std::filesystem::create_directories(shaderDir);
    for (const auto &entry : std::filesystem::directory_iterator(OPENWORLD_SHADER_DIR)) {
        if (entry.path().extension() != ".spv")
            continue;
        auto source = entry.path();
        const auto name = source.filename().string();
        if (name == "terrain.frag.spv")
            source = source.parent_path() / "material_fixture.frag.spv";
        if (name == "terrain_bindless.frag.spv")
            source = source.parent_path() / "material_fixture_bindless.frag.spv";
        std::filesystem::copy_file(source, shaderDir / name,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = shaderDir;
    o.materialDirectory = fixture.path;
    o.forceRgbaMaterials = true;
    o.forceFixedMaterials = fixed;
    o.forceTrilinear = true;
    o.width = 128;
    o.height = 96;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    Renderer r(o);
    TerrainRequest request;
    request.settings.heightScale = 0;
    r.requestTerrain(request);
    Camera c;
    c.local = {8, 5, 8};
    c.pitch = -1.5707963f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.terrainMaterial.filtering = 0;
    r.render(c, s);
    auto a = r.readHdrSnapshot();
    const auto pixel = [&](uint32_t column, uint32_t row) {
        return a.pixels[(row * 16 + 8) * 128 + column * 16 + 8];
    };
    require(glm::length(pixel(0, 0) - glm::vec4(1, 0, 0, 0)) < .002f, "zero coverage has stable fallback");
    require(glm::length(pixel(1, 0) - glm::vec4(0, 0, 1, 0)) < .002f, "height cannot activate absent layers");
    require(pixel(2, 0).y > .99f && pixel(2, 0).x < .01f, "higher micro relief wins equal coverage");
    require(glm::length(pixel(3, 0) - glm::vec4(.5, .5, 0, 0)) < .002f,
            "equal relief preserves equal coverage");
    require(glm::length(pixel(4, 0) - glm::vec4(.25, .75, 0, 0)) < .002f,
            "disabled height blend preserves coverage");
    const glm::vec3 normals[] = {{1, 0, 0},
                                 {-1, 0, 0},
                                 {0, 1, 0},
                                 {0, -1, 0},
                                 {0, 0, 1},
                                 {0, 0, -1},
                                 glm::normalize(glm::vec3(1, 1, 1)),
                                 glm::normalize(glm::vec3(-1, 1, -1))};
    for (uint32_t i = 0; i < 8; ++i)
        require(glm::length(glm::vec3(pixel(i, 1)) * 2.f - 1.f - normals[i]) < .003f,
                "neutral normal preserves signed axis and oblique surfaces");
    c.local.y += 2;
    r.render(c, s);
    const auto elevated = r.readHdrSnapshot();
    for (uint32_t x = 96; x < 128; ++x)
        for (int ch = 0; ch < 3; ++ch)
            near(elevated.pixels[40 * 128 + x][ch], a.pixels[40 * 128 + x][ch], .002,
                 "vertical camera movement preserves cliff texture phase");
    c.local.y -= 2;
    float previous = 0;
    for (uint32_t x = 0; x < 128; ++x) {
        const auto w = a.pixels[56 * 128 + x];
        near(w.x + w.y, 1, .001, "height weights normalized through dominance transition");
        require(w.y >= previous - .001 && w.y - previous < .06f, "height dominance changes continuously");
        previous = w.y;
    }
    // Replace normals with a uniform +U slope. Known directions must perturb all
    // signed faces correctly, not merely produce finite colours.
    auto t = readTexture(fixture.path / "normal_rgba.dds");
    for (size_t i = 0; i < t.bytes.size(); i += 4) {
        t.bytes[i] = 176;
        t.bytes[i + 1] = 128;
    }
    writeTexture(fixture.path / "normal_rgba.dds", t);
    require(r.reloadMaterials(fixture.path), "directional normal fixture loads");
    r.render(c, s);
    auto directional = r.readHdrSnapshot();
    const glm::vec3 tangents[] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {1, 0, 0}};
    for (uint32_t i = 0; i < 6; ++i) {
        const auto n = glm::vec3(directional.pixels[24 * 128 + i * 16 + 8]) * 2.f - 1.f;
        near(glm::length(n), 1, .003, "decoded projected normal is unit length");
        require(glm::dot(n, tangents[i]) > .2f && glm::dot(n, normals[i]) > .8f,
                "signed projection transforms normal slope into expected direction");
    }
    r.render(c, s);
    auto spatial = r.readHdrSnapshot();
    near(spatial.pixels[88 * 128 + 64].a, 1, .001, "bounded worst-case micro sampling count");
    --c.chunk.x;
    c.local.x += 16;
    r.render(c, s);
    require(difference(spatial, r.readHdrSnapshot()) < .003, "all projected normals survive rebasing");
    s.terrainMaterial.diagnostic = 2;
    r.render(c, s);
    const auto projections = r.readHdrSnapshot();
    float largestProjectionStep = 0;
    uint32_t stepPixel = 0;
    for (uint32_t x = 1; x < 128; ++x) {
        const auto a = glm::vec3(projections.pixels[48 * 128 + x]);
        const auto b = glm::vec3(projections.pixels[48 * 128 + x - 1]);
        near(a.x + a.y + a.z, 1, .002, "projection weights remain normalized");
        if (glm::length(a - b) > largestProjectionStep) {
            largestProjectionStep = glm::length(a - b);
            stepPixel = x;
        }
    }
    std::cout << "Projection step=" << largestProjectionStep << " at x=" << stepPixel << '\n';
    // The 128-pixel half-turn sweeps 1.406 degrees per sample; the smooth
    // reference peaks near 0.105. Allow half-float error, reject axis switches.
    require(largestProjectionStep < .12f, "projection weights continuous through steep and signed normals");
    s.terrainMaterial.twoAxis = true;
    r.render(c, s);
    const auto pruned = r.readHdrSnapshot();
    require(difference(projections, pruned) < .09, "weak-axis comparison has bounded deviation");
    require(r.validationErrors() == 0, "numerical material fixture validation");
    fixture.write(false);
    std::filesystem::remove(fixture.path / "macro_rgba.dds");
    return directional;
}
} // namespace
int main() {
    try {
        Fixture f;
        auto numericalBindless = numericalFixture(f, false);
        auto numericalFixed = numericalFixture(f, true);
        require(difference(numericalBindless, numericalFixed) < .002,
                "projection fixture descriptor equivalence");
        auto bindless = run(f, false), fixed = run(f, true);
        require(difference(bindless[0], fixed[0]) < .002, "bindless/fixed equivalence");
        require(difference(bindless[1], fixed[1]) < .002, "5C bindless/fixed equivalence");
        RendererOptions o;
        o.ui = false;
        o.requireValidation = true;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.terrainGrid = {8, .5f};
        o.terrainCapacity = 8;
        o.width = 32;
        o.height = 32;
        uint64_t fallbackBytes = 0;
        {
            Renderer r(o);
            Camera c;
            SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
            s.gtao.mode = ow::GtaoMode::Off;
            r.render(c, s);
            fallbackBytes = r.materialStats().imageBytes;
        }
        o.materialBudgetBytes = fallbackBytes;
        o.forceRgbaMaterials = true;
        {
            Renderer r(o);
            require(!r.reloadMaterials(f.path), "budget exhaustion rejects palette");
            require(!r.materialStats().loaded, "fallback retained on exhaustion");
            Camera c;
            SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
            s.gtao.mode = ow::GtaoMode::Off;
            r.render(c, s);
            r.waitIdle();
            require(r.materialStats().imageBytes <= fallbackBytes, "budget includes fallback");
            require(r.validationErrors() == 0, "exhaustion validation");
        }
        o.terrainGrid = {32, .5f};
        o.terrainCapacity = 32;
        o.width = 128;
        o.height = 96;
        o.materialBudgetBytes = 320ull * 1024 * 1024;
        o.materialDirectory = f.path;
        o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = o.streaming.hierarchical = true;
        o.streaming.diskCache = false;
        o.streaming.maximumDistance = 32;
        o.streaming.gpuBytes = 40 * terrainMiB;
        o.streaming.hostBytes = 64 * terrainMiB;
        o.streaming.cacheDirectory = f.path / "cache";
        o.streaming.diagnosticFrameSeconds = 1. / 60;
        o.lod = {true, 8, 64};
        {
            Renderer r(o);
            TerrainSettings t;
            t.heightScale = 0;
            r.setTerrainSettings(t);
            Camera c;
            c.local = {8, 10, 18};
            c.pitch = -.65;
            SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
            s.gtao.mode = ow::GtaoMode::Off;
            s.viewRadius = 1;
            s.terrainMaterial.isolate = 0;
            s.terrainMaterial.filtering = 0;
            bool ready = false;
            for (int i = 0; i < 2000; ++i) {
                r.render(c, s);
                const auto &st = r.terrainStreamingStats();
                if (st.coverage >= 9 && !st.pending && !st.detailPending && !st.ioJobs &&
                    r.terrainRenderStats().triangles) {
                    ready = true;
                    break;
                }
            }
            require(ready, "streamed material fixture settled");
            for (int i = 0; i < 8; ++i)
                r.render(c, s);
            const auto image = r.readHdrSnapshot();
            const size_t centre = 48 * 128 + 64;
            for (int ch = 0; ch < 3; ++ch)
                near(image.pixels[centre][ch], bindless[0].pixels[centre][ch], .004,
                     "fixed/streamed material semantics");
            f.writeMacro();
            require(r.reloadMaterials(f.path), "streamed macro palette upload");
            applyMaterialPreset(s.terrainMaterial, TerrainMaterialPreset::High);
            s.terrainMaterial.filtering = 0;
            r.render(c, s);
            const auto highImage = r.readHdrSnapshot();
            checkDepthPrepass(r, c, s);
            for (int ch = 0; ch < 3; ++ch)
                near(highImage.pixels[centre][ch], bindless[1].pixels[centre][ch], .004,
                     "5C fixed/streamed material semantics");
            s.terrainMaterial.filtering = 1;
            r.render(c, s);
            near(r.materialStats().effectiveAnisotropy, std::min(8.f, r.materialStats().maxAnisotropy), 0,
                 "8x clamped sampler");
            s.terrainMaterial.filtering = 2;
            r.render(c, s);
            near(r.materialStats().effectiveAnisotropy, std::min(16.f, r.materialStats().maxAnisotropy), 0,
                 "16x clamped sampler");
            r.waitIdle();
            require(r.validationErrors() == 0, "streamed material validation");
        }
        std::cout << "Material GPU invariants passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

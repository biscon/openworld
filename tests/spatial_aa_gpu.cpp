#include "render_fixtures.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace ow;
namespace {
RendererOptions options() {
    RendererOptions o;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.requireValidation = true;
    o.ui = false;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.visibilityCapacity = 128;
    return o;
}
SceneSettings scene() {
    SceneSettings s;
    s.terrainVisible = false;
    s.output.ev100 = 0;
    s.shadows.mode = ShadowMode::Off;
    s.background[0] = s.background[1] = s.background[2] = 0;
    return s;
}
void frame(Renderer &r, const Camera &c, const SceneSettings &s) {
    require(r.render(c, s), "Spatial fixture submitted");
    require(r.validationErrors() == 0, "Spatial AA has no validation errors");
}
std::vector<uint8_t> capture(Renderer &r, const std::filesystem::path &file) {
    r.capture(file);
    std::ifstream f(file, std::ios::binary);
    std::string magic;
    uint32_t w, h, max;
    f >> magic >> w >> h >> max;
    f.get();
    require(magic == "P6" && max == 255, "Owned capture format");
    std::vector<uint8_t> pixels(size_t(w) * h * 3);
    f.read(reinterpret_cast<char *>(pixels.data()), pixels.size());
    require(bool(f), "Complete capture");
    return pixels;
}
void edges(const std::filesystem::path &file) {
    Renderer r(options());
    auto s = scene();
    Camera c;
    c.local = {0, 0, 10};
    MeshData mesh;
    mesh.vertices = {{{-4, -3, 0}}, {{4, -2.6f, 0}}, {{-.7f, 3, 0}}};
    mesh.indices = {0, 1, 2};
    // Thin diagonal strips supplement the solid edge without production content.
    for (int n = 0; n < 8; ++n) {
        uint32_t first = uint32_t(mesh.vertices.size());
        float x = -3 + n * .6f;
        mesh.vertices.insert(
            mesh.vertices.end(),
            {{{x, -3, .1f}}, {{x + .025f, -3, .1f}}, {{x + 1, 3, .1f}}, {{x + 1.025f, 3, .1f}}});
        mesh.indices.insert(mesh.indices.end(),
                            {first, first + 1, first + 2, first + 2, first + 1, first + 3});
    }
    MeshMaterial m;
    m.unlit = m.doubleSided = true;
    m.baseColour = {.8, .8, .8, 1};
    MeshInstance i;
    i.mesh = r.uploadMesh(mesh);
    i.material = r.createMaterial(m);
    r.createInstance(i);
    frame(r, c, s);
    const auto off = capture(r, file);
    const auto inactiveTemporalBytes = r.postStats().taaBytes;
    uint8_t white = *std::max_element(off.begin(), off.end());
    require(white > 100, "Visible reference geometry");
    for (auto mode : {AntialiasingMode::Fxaa, AntialiasingMode::Smaa1x}) {
        s.antialiasing = mode;
        frame(r, c, s);
        auto first = capture(r, file);
        require(r.spatialAaStats().effective == mode && r.spatialAaStats().bytes > 0,
                "Requested spatial AA is active");
        require(!r.postStats().taa && !r.postStats().historyValid &&
                    r.postStats().taaBytes == inactiveTemporalBytes && r.postStats().thinGeometryBytes == 0 &&
                    r.postStats().sequence == 0,
                "Spatial AA allocates no temporal history");
        uint32_t intermediates = 0;
        for (size_t j = 0; j < first.size(); j += 3)
            if (first[j] > 3 && first[j] + 3 < white)
                ++intermediates;
        require(intermediates > 30, "Spatial AA reconstructs intermediate edge coverage");
        for (int n = 0; n < 4; ++n) {
            frame(r, c, s);
            require(capture(r, file) == first, "Frozen spatial output is independent of frame index");
        }
        std::cout << antialiasingName(mode) << ": intermediate edge pixels=" << intermediates << '\n';
        s.antialiasing = AntialiasingMode::Taa;
        frame(r, c, s);
        require(!r.postStats().historyValid, "Entering TAA starts fresh history");
        frame(r, c, s);
        require(r.postStats().historyValid, "TAA continues accumulating");
    }
    s.antialiasing = AntialiasingMode::Off;
    frame(r, c, s);
    require(capture(r, file) == off && r.spatialAaStats().bytes == 0,
            "Off restores original image and releases spatial images");
    for (auto mode : {AntialiasingMode::Fxaa, AntialiasingMode::Smaa1x}) {
        s.antialiasing = mode;
        s.shading = ShadingMode::Legacy;
        frame(r, c, s);
        require(r.spatialAaStats().effective == AntialiasingMode::Off &&
                    r.spatialAaStats().fallback == PostFallback::Diagnostic,
                "Diagnostics bypass filtering");
        s.shading = ShadingMode::Pbr;
        for (auto extent : {glm::uvec2(1, 1), glm::uvec2(3, 5), glm::uvec2(83, 57)}) {
            r.resizeOffscreen(extent.x, extent.y);
            frame(r, c, s);
            capture(r, file);
            require(r.spatialAaStats().effective == mode, "Spatial AA survives tiny/odd resize");
        }
        r.setRenderHeight(41);
        r.resizeOffscreen(127, 81);
        frame(r, c, s);
        capture(r, file);
    }
    require(r.validationErrors() == 0, "Resize and switching validation clean");
}
void colors(const std::filesystem::path &file) {
    Renderer r(options());
    auto s = scene();
    Camera c;
    c.local = {0, 0, 10};
    MeshMaterial m;
    m.unlit = m.doubleSided = true;
    m.baseColour = {.12, .35, .7, 1};
    m.emissive = {.1, .2, .3};
    MeshInstance i;
    i.mesh = r.uploadMesh(makeQuad());
    i.material = r.createMaterial(m);
    i.scale = {100, 100, 1};
    r.createInstance(i);
    for (auto mapper : {ToneMapper::AcesFitted, ToneMapper::Agx}) {
        s.output.toneMapper = mapper;
        for (bool bloom : {false, true}) {
            s.bloom.intensity = bloom ? .2f : 0;
            s.bloom.threshold = .1f;
            s.antialiasing = AntialiasingMode::Off;
            frame(r, c, s);
            const auto reference = capture(r, file);
            for (auto mode : {AntialiasingMode::Fxaa, AntialiasingMode::Smaa1x}) {
                s.antialiasing = mode;
                frame(r, c, s);
                const auto filtered = capture(r, file);
                for (size_t j = 0; j < reference.size(); ++j)
                    require(std::abs(int(reference[j]) - int(filtered[j])) <= 1,
                            "Flat color preserves tone mapping, bloom and gamma within one UNORM step");
            }
        }
    }
}
void budget(const std::filesystem::path &file) {
    auto o = options();
    o.spatialAaBudgetBytes = 0;
    Renderer r(o);
    auto s = scene();
    Camera c;
    frame(r, c, s);
    const auto off = capture(r, file);
    for (auto mode : {AntialiasingMode::Fxaa, AntialiasingMode::Smaa1x}) {
        s.antialiasing = mode;
        frame(r, c, s);
        require(r.spatialAaStats().effective == AntialiasingMode::Off &&
                    r.spatialAaStats().fallback == PostFallback::Budget && r.spatialAaStats().bytes == 0,
                "Budget exhaustion falls back without retaining allocations");
        require(capture(r, file) == off, "Budget fallback preserves Off output");
    }
}
} // namespace
int main() {
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("ow-spatial-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    try {
        edges(dir / "capture.ppm");
        colors(dir / "capture.ppm");
        budget(dir / "capture.ppm");
        std::filesystem::remove_all(dir);
        std::cout << "Spatial AA tests passed\n";
    } catch (const std::exception &e) {
        std::filesystem::remove_all(dir);
        std::cerr << e.what() << '\n';
        return 1;
    }
}

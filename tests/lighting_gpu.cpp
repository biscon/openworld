#include "renderer.hpp"
#include "render_fixtures.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <backends/imgui_impl_vulkan.h>
#include <chrono>
#include <fstream>
#include <imgui.h>
#include <iostream>
#include <limits>
using namespace ow;
namespace {
struct Fixture {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openworld-lighting-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(path);
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
    }
    ~Fixture() {
        ImGui::DestroyContext();
        std::error_code e;
        std::filesystem::remove_all(path, e);
    }
};
std::vector<unsigned char> capture(Renderer &r, const std::filesystem::path &path) {
    r.capture(path);
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    unsigned w, h, max;
    f >> magic >> w >> h >> max;
    f.get();
    require(magic == "P6" && w == r.width() && h == r.height() && max == 255, "Capture header");
    std::vector<unsigned char> p(size_t(w) * h * 3);
    require(bool(f.read(reinterpret_cast<char *>(p.data()), p.size())), "Capture payload");
    return p;
}
void frame(Renderer &r, const Camera &c, const SceneSettings &s) {
    ImGui_ImplVulkan_NewFrame();
    auto &io = ImGui::GetIO();
    io.DisplaySize = {float(r.width()), float(r.height())};
    io.DeltaTime = 1.f / 60;
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled({4, 4}, {28, 28}, IM_COL32(128, 128, 128, 255));
    ImGui::GetForegroundDrawList()->AddRectFilled({32, 4}, {56, 28}, IM_COL32(128, 128, 128, 128));
    ImGui::Render();
    require(r.render(c, s), "Render succeeds");
}
size_t projected(const Camera &c, uint32_t w, uint32_t h, glm::vec3 p) {
    auto clip = c.viewProjection(float(w) / h) * glm::vec4(p - glm::vec3(c.local), 1);
    auto ndc = glm::vec2(clip) / clip.w;
    return size_t((ndc.y * .5f + .5f) * h) * w + uint32_t((ndc.x * .5f + .5f) * w);
}
void finite(const HdrSnapshot &s) {
    for (auto p : s.pixels)
        for (int i = 0; i < 4; i++)
            require(std::isfinite(p[i]) && p[i] >= 0, "HDR pixels finite and nonnegative");
}
} // namespace
int main() {
    try {
        Fixture fixture;
        RendererOptions options;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.requireValidation = true;
        options.width = 320;
        options.height = 240;
        options.terrainGrid = {32, .5f};
        options.terrainCapacity = 32;
        Renderer renderer(options);
        TerrainRequest terrain;
        terrain.settings.heightScale = 0;
        renderer.requestTerrain(terrain);
        Camera camera;
        camera.local = {0, 0, 16};
        SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        scene.terrainVisible = false;
        fixtures::Swatches swatches(renderer);
        std::fill_n(scene.background, 3, 0.f);
        frame(renderer, camera, scene);
        auto first = renderer.readHdrSnapshot();
        finite(first);
        auto generation = renderer.terrainGenerationCount();
        float largest = 0;
        for (auto p : first.pixels)
            largest = std::max(largest, p.x);
        require(largest > 8, "HDR retains emissive radiance above one");
        // Owned emissive quads: exact radiance independent of material BRDF.
        for (int i = 0; i < 5; i++) {
            auto pixel = projected(camera, 320, 240, {(i - 2) * 2.5f, -3.6f, 0});
            near(first.pixels[pixel].x, 1000 * std::exp2(float(i * 3)) * first.preExposure,
                 .001 * std::max(1.f, first.pixels[pixel].x), "Emissive pre-exposure units");
        }
        for (auto mapper : {ToneMapper::AcesFitted, ToneMapper::Agx}) {
            scene.output.toneMapper = mapper;
            frame(renderer, camera, scene);
            auto hdr = renderer.readHdrSnapshot();
            auto bytes = capture(renderer, fixture.path / "output.ppm");
            for (int row = 2; row < 4; row++)
                for (int x = 0; x < 5; x++) {
                    auto p = projected(camera, 320, 240, {(x - 2) * 2.5f, 3.6f - row * 2.4f, 0});
                    auto expected = toneMap(glm::vec3(hdr.pixels[p]), mapper);
                    for (int k = 0; k < 3; k++)
                        near(bytes[p * 3 + k], 255 * linearToSrgb(expected[k]), 2,
                             "GPU tone map and single sRGB encoding match CPU reference");
                }
            near(bytes[(10 * 320 + 10) * 3], 128, 1, "UI authored grey encoded once");
            near(bytes[(10 * 320 + 40) * 3], 255 * linearToSrgb(srgbToLinear(128.f / 255) * (128.f / 255)), 2,
                 "UI alpha blends in display-linear space");
        }
        scene.output.compensation = 1;
        frame(renderer, camera, scene);
        auto brighter = renderer.readHdrSnapshot();
        for (size_t p = 0; p < first.pixels.size(); p += 37)
            for (int c = 0; c < 3; c++)
                near(brighter.pixels[p][c], 2 * first.pixels[p][c], .003 * std::max(1.f, first.pixels[p][c]),
                     "One stop doubles HDR without rebaking");
        auto bytes = capture(renderer, fixture.path / "exposed.ppm");
        near(bytes[(10 * 320 + 10) * 3], 128, 1, "UI independent of exposure");
        scene.output.compensation = 0;
        scene.lighting.sunEnabled = false;
        scene.lighting.environmentEnabled = false;
        frame(renderer, camera, scene);
        auto dark = renderer.readHdrSnapshot();
        auto centre = projected(camera, 320, 240, {-5, 3.6f, 0});
        near(dark.pixels[centre].x, 0, 1e-7, "No ambient magic light");
        scene.lighting.environmentEnabled = true;
        frame(renderer, camera, scene);
        auto env = renderer.readHdrSnapshot();
        require(env.pixels[centre].x > .001f, "Environment lights the dielectric with sun disabled");
        scene.lighting.environmentRotation = 2;
        frame(renderer, camera, scene);
        auto rotated = renderer.readHdrSnapshot();
        float difference = 0;
        for (size_t p = 0; p < env.pixels.size(); p++)
            difference += glm::length(env.pixels[p] - rotated.pixels[p]);
        require(difference > 1, "Environment rotation updates reflections");
        scene.lighting.environmentEnabled = false;
        scene.lighting.sunEnabled = true;
        swatches.clear();
        scene.terrainVisible = true;
        camera.local = {8, 24, 8};
        camera.pitch = -1.570796f;
        frame(renderer, camera, scene);
        auto ground = renderer.readHdrSnapshot();
        finite(ground);
        const auto vp = camera.viewProjection(320.f / 240);
        const auto inverse = glm::inverse(vp);
        for (int y = 115; y < 126; y += 5)
            for (int x = 155; x < 166; x += 5) {
                glm::vec4 ray = inverse * glm::vec4((x + .5f) / 320 * 2 - 1, (y + .5f) / 240 * 2 - 1, 1, 1);
                auto relative = glm::vec3(ray) / ray.w;
                relative *= float(-camera.local.y) / relative.y;
                auto expected = directBrdf(scene.material, {0, 1, 0}, glm::normalize(-relative),
                                           glm::normalize(scene.lighting.sunDirection)) *
                                scene.lighting.sunLux * ground.preExposure;
                auto actual = glm::vec3(ground.pixels[y * 320 + x]);
                require(glm::length(actual - expected) < .002f,
                        "Terrain PBR matches CPU BRDF with camera-relative view direction");
            }
        require(renderer.terrainGenerationCount() == generation,
                "Mesh fixtures and lighting edits preserve terrain generation");
        scene.normalView = true;
        frame(renderer, camera, scene);
        auto diagnostic = capture(renderer, fixture.path / "diagnostic.ppm");
        scene.output.ev100 = 0;
        frame(renderer, camera, scene);
        require(diagnostic == capture(renderer, fixture.path / "diagnostic2.ppm"),
                "Terrain diagnostic bypasses exposure");
        rejects([&] { renderer.resizeOffscreen(0, 100); }, "Reject zero extent");
        for (auto extent : {glm::uvec2(513, 257), glm::uvec2(320, 240)}) {
            renderer.resizeOffscreen(extent.x, extent.y);
            frame(renderer, camera, scene);
            auto hdr = renderer.readHdrSnapshot();
            require(hdr.width == extent.x && hdr.height == extent.y, "Resize recreates HDR target");
            finite(hdr);
            require(renderer.renderingStats().hdrBytes >= uint64_t(extent.x) * extent.y * 16,
                    "Both in-flight HDR slots accounted");
        }
        scene.normalView = false;
        scene.material.roughness = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { frame(renderer, camera, scene); }, "Reject non-finite material");
        scene.material.roughness = .8f;
        frame(renderer, camera, scene);
        frame(renderer, camera, scene);
        require(renderer.validationErrors() == 0, "HDR resource lifecycle validation clean");
        std::cout << "Lighting GPU contracts passed; HDR " << renderer.renderingStats().hdrBytes
                  << " bytes; lighting " << renderer.renderingStats().lightingBytes << " bytes\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

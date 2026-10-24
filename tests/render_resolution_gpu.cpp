#include "render_fixtures.hpp"
#include "render_resolution.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <backends/imgui_impl_vulkan.h>
#include <chrono>
#include <fstream>
#include <imgui.h>
#include <iostream>
using namespace ow;
namespace {
struct Fixture {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openworld-resolution-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
void finite(const HdrSnapshot &s) {
    for (auto p : s.pixels)
        for (int i = 0; i < 4; i++)
            require(std::isfinite(p[i]) && p[i] >= 0, "HDR pixels finite and nonnegative");
}
} // namespace

namespace {
glm::vec3 sample(const HdrSnapshot &hdr, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const float sx = (x + .5f) * hdr.width / w - .5f;
    const float sy = (y + .5f) * hdr.height / h - .5f;
    const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
    auto texel = [&](int tx, int ty) {
        return glm::vec3(hdr.pixels[size_t(std::clamp(ty, 0, int(hdr.height) - 1)) * hdr.width +
                                    std::clamp(tx, 0, int(hdr.width) - 1)]);
    };
    return glm::mix(glm::mix(texel(ix, iy), texel(ix + 1, iy), sx - ix),
                    glm::mix(texel(ix, iy + 1), texel(ix + 1, iy + 1), sx - ix), sy - iy);
}
void checkOutput(Renderer &r, const SceneSettings &scene, const HdrSnapshot &hdr,
                 const std::vector<unsigned char> &bytes, const GtaoSnapshot *ao = nullptr) {
    auto pixel = [&](uint32_t x, uint32_t y) {
        glm::vec3 expected;
        if (ao) {
            const auto sx = std::min(uint32_t((x + .5) * ao->width / r.width()), ao->width - 1);
            const auto sy = std::min(uint32_t((y + .5) * ao->height / r.height()), ao->height - 1);
            expected = glm::vec3(ao->pixels[size_t(sy) * ao->width + sx]);
        } else {
            expected = toneMap(sample(hdr, x, y, r.width(), r.height()), scene.output.toneMapper);
        }
        if (y >= 4 && y < 28) {
            if (x >= 4 && x < 28)
                expected = glm::vec3(srgbToLinear(128.f / 255));
            if (x >= 32 && x < 56)
                expected = glm::mix(expected, glm::vec3(srgbToLinear(128.f / 255)), 128.f / 255);
        }
        for (int c = 0; c < 3; ++c)
            near(bytes[(size_t(y) * r.width() + x) * 3 + c], 255 * linearToSrgb(expected[c]), 3,
                 "Scaled output matches mapped scene, tone mapping, and native UI");
    };
    for (uint32_t y = 0; y < r.height(); y += 5)
        for (uint32_t x = 0; x < r.width(); x += 5)
            pixel(x, y);
    for (uint32_t x : {0u, r.width() - 1})
        for (uint32_t y : {0u, r.height() - 1})
            pixel(x, y);
    for (uint32_t x : {3u, 4u, 27u, 28u, 31u, 32u, 55u, 56u})
        pixel(x, 10);
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
        options.renderHeight = 120;
        options.terrainGrid = {8, .5f};
        options.terrainCapacity = 8;
        Renderer renderer(options);
        Camera camera;
        camera.local = {0, 0, 16};
        SceneSettings scene;
        scene.terrainVisible = false;
        scene.shadows.mode = ShadowMode::Off;
        scene.background[0] = .03f;
        scene.background[1] = .05f;
        scene.background[2] = .09f;
        fixtures::Swatches swatches(renderer);
        LocalLight light;
        light.position.local = {0, 2, 3};
        light.intensity = 100000;
        light.range = 40;
        auto lightHandle = renderer.createLight(light);
        rejects([&] { renderer.setRenderHeight(16385); }, "Reject excessive render height");
        HdrSnapshot previous;
        uint64_t previousBytes = 0;
        struct Case {
            uint32_t w, h, renderHeight;
        };
        const Case cases[] = {{320, 240, 120}, {640, 480, 120}, {513, 257, 121},
                              {200, 100, 180}, {320, 240, 120}, {321, 181, 0}};
        for (size_t i = 0; i < std::size(cases); ++i) {
            auto c = cases[i];
            renderer.resizeOffscreen(c.w, c.h);
            const auto before = renderer.renderHeight();
            renderer.setRenderHeight(c.renderHeight);
            require(renderer.renderHeight() == before, "Height updates wait for frame boundary");
            scene.gtao.mode = i >= 2 ? GtaoMode::Half : GtaoMode::Off;
            scene.visibility = i >= 2 ? VisibilityMode::HiZ : VisibilityMode::Indirect;
            scene.output.toneMapper = i % 2 ? ToneMapper::Agx : ToneMapper::AcesFitted;
            frame(renderer, camera, scene);
            frame(renderer, camera,
                  scene); // Exercise both fence-owned depth/HDR slots.
            auto hdr = renderer.readHdrSnapshot();
            finite(hdr);
            auto expected = sceneRenderExtent(c.w, c.h, c.renderHeight);
            require(hdr.width == expected.width && hdr.height == expected.height &&
                        renderer.renderWidth() == hdr.width && renderer.renderHeight() == hdr.height,
                    "HDR readback uses scene extent independently of output");
            auto bytes = capture(renderer, fixture.path / "scaled.ppm");
            checkOutput(renderer, scene, hdr, bytes);
            if (i == 1) {
                require(previous.pixels == hdr.pixels, "Same-aspect output resize preserves scene pixels");
                require(previousBytes == renderer.renderingStats().hdrBytes,
                        "Same-aspect output resize preserves HDR memory footprint");
            }
            previous = hdr;
            previousBytes = renderer.renderingStats().hdrBytes;
            scene.localLights = LocalLightMode::BruteForce;
            frame(renderer, camera, scene);
            auto brute = renderer.readHdrSnapshot();
            for (size_t p = 0; p < hdr.pixels.size(); ++p)
                require(glm::length(brute.pixels[p] - hdr.pixels[p]) < .003f,
                        "Clusters at scene resolution match brute-force lighting");
            scene.localLights = LocalLightMode::Clustered;
            if (i >= 2) {
                if (renderer.visibilityStats().effective == VisibilityMode::HiZ) {
                    auto pyramid = renderer.readVisibilityPyramid();
                    require(pyramid.width == hdr.width && pyramid.height == hdr.height &&
                                pyramid.levels.back().size() == 1,
                            "Hi-Z uses the full scene-sized depth pyramid");
                } else {
                    std::cout << "Hi-Z unsupported: scaled pyramid check skipped\n";
                }
                if (renderer.gtaoStats().effective != GtaoMode::Off) {
                    scene.gtao.diagnostic = GtaoDiagnostic::Normals;
                    frame(renderer, camera, scene);
                    auto ao = renderer.readGtaoSnapshot();
                    require(ao.width == hdr.width && ao.height == hdr.height,
                            "AO reconstruction uses scene dimensions");
                    bytes = capture(renderer, fixture.path / "ao.ppm");
                    checkOutput(renderer, scene, hdr, bytes, &ao);
                    scene.gtao.diagnostic = GtaoDiagnostic::Shaded;
                } else {
                    std::cout << "GTAO unsupported: scaled diagnostic check skipped\n";
                }
            }
        }
        renderer.removeLight(lightHandle);
        // UI is neither jittered nor a bloom/history input, even at threshold zero.
        swatches.clear();
        scene.gtao = {};
        scene.antialiasing = ow::AntialiasingMode::Taa;
        scene.bloom.intensity = 1;
        scene.bloom.threshold = 0;
        scene.background[0] = scene.background[1] = scene.background[2] = 0;
        frame(renderer, camera, scene);
        frame(renderer, camera, scene);
        const auto post = renderer.readPostSnapshot();
        for (auto pixel : post.resolved)
            require(glm::vec3(pixel) == glm::vec3(0), "UI is absent from temporal HDR");
        for (const auto &level : post.bloom)
            for (auto pixel : level)
                require(glm::vec3(pixel) == glm::vec3(0), "UI does not enter bloom extraction");
        const auto postOutput = capture(renderer, fixture.path / "post-ui.ppm");
        scene.antialiasing = ow::AntialiasingMode::Off;
        scene.bloom.intensity = 0;
        frame(renderer, camera, scene);
        require(postOutput == capture(renderer, fixture.path / "plain-ui.ppm"),
                "Native UI edges are unchanged by TAA and bloom");
        require(renderer.validationErrors() == 0, "Scaled rendering resource lifecycle validation clean");
        std::cout << "Scene resolution GPU contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

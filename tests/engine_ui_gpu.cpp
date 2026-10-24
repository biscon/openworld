#include "engine_ui.hpp"
#include "test_support.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <iostream>

using namespace ow;
namespace {
struct ImGuiLifetime {
    ImGuiLifetime() {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
    }
    ~ImGuiLifetime() { ImGui::DestroyContext(); }
};
} // namespace
int main(int argc, char **argv) {
    try {
        ImGuiLifetime lifetime;
        RendererOptions options;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.requireValidation = true;
        options.width = 1280;
        options.height = 800;
        options.renderHeight = 120;
        options.terrainGrid = {8, .5f};
        options.terrainCapacity = 8;
        Renderer renderer(options);
        Camera camera;
        camera.reset(options.terrainGrid, {}, 30);
        SceneSettings scene;
        scene.atmosphere.mode = AtmosphereMode::Off;
        scene.shadows.mode = ShadowMode::Off;
        TerrainRequest terrain;
        renderer.requestTerrain(terrain);
        LightingDemoControls demo;
        const std::string error, status, reviewPath = "owned-review-fixture.json";
        UiContext context{renderer, options, scene, terrain, camera, demo, error, status, reviewPath};
        EngineUi ui;
        const auto frame = [&] {
            ImGui_ImplVulkan_NewFrame();
            auto &io = ImGui::GetIO();
            io.DisplaySize = {float(renderer.width()), float(renderer.height())};
            io.DeltaTime = 1.f / 60;
            ImGui::NewFrame();
            const auto actions = ui.draw(context);
            require(!actions.regenerate && !actions.resetCamera && !actions.saveReview &&
                        !actions.loadReview && !actions.reloadPalette && !actions.renderHeightChanged &&
                        !actions.lightingDemoChanged && !actions.lightingDemoReanchor &&
                        actions.fogPreset == -1,
                    "Idle panel drawing does not request application actions");
            ImGui::Render();
            require(renderer.render(camera, scene), "UI frame renders");
        };
        const auto capture = [&](const char *name) {
            if (argc > 1) {
                std::filesystem::create_directories(argv[1]);
                renderer.capture(std::filesystem::path(argv[1]) / (std::string(name) + ".ppm"));
            }
        };
        frame();
        frame();
        capture("menu-only");
        for (size_t i = 0; i < uiWindowCount; ++i) {
            ui.state.open.fill(false);
            ui.state.open[i] = true;
            frame();
            frame();
            capture(uiWindows[i].id);
        }
        require(terrain.settings.seed == TerrainSettings{}.seed && scene.shadows.mode == ShadowMode::Off &&
                    !demo.enabled,
                "Drawing all panels preserves application settings");
        // Closing diagnostic windows must not reset their active visualization.
        scene.normalView = true;
        scene.shadows.diagnostic = 2;
        scene.gtao.diagnostic = GtaoDiagnostic::Normals;
        ui.state.open.fill(false);
        frame();
        require(scene.normalView && scene.shadows.diagnostic == 2 &&
                    scene.gtao.diagnostic == GtaoDiagnostic::Normals,
                "Hiding diagnostic windows preserves visualization modes");
        renderer.waitIdle();
        require(renderer.validationEnabled() && renderer.validationErrors() == 0,
                "UI Vulkan validation clean");
        std::cout << "Every UI panel rendered with Vulkan validation\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

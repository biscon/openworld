#include "engine_ui.hpp"
#include "lighting_demo.hpp"
#include "renderer.hpp"
#include "review_view.hpp"
#include "volumetric_demo.hpp"
#include <optional>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
struct Platform {
    SDL_Window *window = nullptr;
    bool sdl = false;
    bool imgui = false;
    bool imguiPlatform = false;
    ~Platform() {
        if (imguiPlatform)
            ImGui_ImplSDL3_Shutdown();
        if (imgui)
            ImGui::DestroyContext();
        if (window)
            SDL_DestroyWindow(window);
        if (sdl)
            SDL_Quit();
    }
};

void help() {
    std::cout << "OpenWorld | SDL3 + Vulkan + HLSL + Dear ImGui\n"
                 "  --frames N            Exit after N rendered frames\n"
                 "  --headless            Render offscreen without a display server\n"
                 "  --render-height N     Scene height, 1..16384 (default 1080); width "
                 "follows aspect\n"
                 "  --capture FILE.ppm    Save final offscreen frame (requires "
                 "--headless)\n"
                 "  --review-view PATH    Replay a saved camera and rendering "
                 "settings\n"
                 "  --save-review-view PATH  Save the final view on exit\n"
                 "  --shader-dir PATH     Override compiled shader directory\n"
                 "  --validation          Enable validation when available\n"
                 "  --no-validation       Disable validation\n"
                 "  --require-validation  Fail if validation is unavailable\n"
                 "  --terrain-size M      Power-of-two width, 4..512 m (default 128)\n"
                 "  --terrain-cells N     Power-of-two cells, 8..1024 at 0.5 m\n"
                 "  --chunk-x N           Neighbourhood centre chunk X (default 0)\n"
                 "  --chunk-z N           Neighbourhood centre chunk Z (default 0)\n"
                 "  --view-radius N       Render radius in base chunks (default ~8 km, "
                 "max ~16 km)\n"
                 "  --cache-dir PATH      Override per-user terrain cache directory\n"
                 "  --vegetation          Enable streamed vegetation (default on)\n"
                 "  --no-vegetation       Disable vegetation\n"
                 "  --vegetation-distance M  Render distance, 20..200 m (default 80)\n"
                 "  --no-cache            Disable disk caching, retain streaming\n"
                 "  --terrain-gpu-mib N   GPU terrain budget (default 512)\n"
                 "  --terrain-host-mib N  Host terrain budget (default 256)\n"
                 "  --cache-mib N         Disk cache budget (default 8192)\n"
                 "  --single-chunk        Fixed single-chunk inspection (no "
                 "streaming)\n"
                 "  --chunk-borders       Highlight chunk boundaries (draw-only)\n"
                 "  --no-lod              Draw finest resident detail (fixed mode: "
                 "full detail)\n"
                 "  --lod-colours         Colour patches by selected LOD\n"
                 "  --vertex-normals      Compare legacy vertex-normal interpolation\n"
                 "  --base                Show uneroded terrain\n"
                 "  --atmosphere MODE     off, analytic or physical (default "
                 "physical)\n"
                 "  --no-horizon          Disable distant coverage and visual "
                 "curvature\n"
                 "  --fog PRESET          off, mist or shafts (default off)\n"
                 "  --lighting-demo       Enable generated lights and mesh fixtures\n"
                 "  --material-dir PATH   Override imported terrain palette directory\n"
                 "  --no-material-assets  Use the constant fallback without loading "
                 "assets\n"
                 "  --fixed-materials     Force fixed-binding material fallback\n"
                 "  --rgba-materials      Force uncompressed material textures\n"
                 "  --constant-material   Disable textured terrain for comparison\n"
                 "  --legacy-lighting     Use the old diagnostic shading\n"
                 "  --agx                 Use AgX neutral instead of ACES fitted\n"
                 "  --no-depth-prepass    Disable default opaque depth prepass\n"
                 "  --terrain-triangles N Terrain triangle budget (default 2097152)\n"
                 "  --aa MODE             off, fxaa, smaa or taa (default smaa)\n"
                 "  --no-taa              Alias for --aa off\n"
                 "  --no-bloom            Disable HDR bloom\n"
                 "  --bloom-intensity N   Bloom strength, 0..2 (default 0.05)\n"
                 "  --ev100 VALUE         Manual camera exposure (default 15)\n"
                 "  --no-ui               Hide the overlay (useful for captures)\n"
                 "  --help                Show this help\n"
                 "Hold RMB: look / WASD + QE: fly / Shift: boost / Escape: release or "
                 "quit / F10: borderless fullscreen.\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        bool headless = false;
        ow::LightingDemoControls demoControls;
        uint32_t fogPreset = 0;
        uint64_t frameLimit = 0;
        ow::SceneSettings scene;
        scene.vegetation.enabled = true;
        scene.vegetation.distance = 80;
        std::optional<bool> vegetationEnabled;
        std::optional<float> vegetationDistance;
        scene.antialiasing = ow::AntialiasingMode::Smaa1x;
        std::optional<ow::AntialiasingMode> aaOverride;
        scene.opaqueDepthPrepass = true;
        scene.bloom.intensity = .05f;
        std::filesystem::path capture, reviewPath, reviewSavePath;
        std::optional<ow::ReviewView> review;
        std::string reviewStatus;
        ow::RendererOptions options;
        options.width = 1920;
        options.height = 1080;
        options.renderHeight = 1080;
        options.lod.enabled = true;
        options.streaming.enabled = true;
        options.streaming.hierarchical = true;
        options.streaming.horizonBytes = 128ull << 20;
        options.curvedHorizon = true;
        scene.atmosphere.mode = ow::AtmosphereMode::Physical;
        bool explicitRadius = false;
        double terrainSize = 128;
        bool singleChunk = false;
        ow::TerrainRequest terrain;
        uint32_t terrainCells = 0;
        bool explicitSize = false;
#ifndef OPENWORLD_DEBUG
        options.validation = false;
#endif
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.materialDirectory = OPENWORLD_MATERIAL_DIR;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc)
                    throw std::runtime_error("Missing value after " + arg);
                return argv[i];
            };
            if (arg == "--help") {
                help();
                return 0;
            }
            if (arg == "--vegetation")
                vegetationEnabled = true;
            else if (arg == "--no-vegetation")
                vegetationEnabled = false;
            else if (arg == "--vegetation-distance") {
                vegetationDistance = std::stof(value());
                if (!std::isfinite(*vegetationDistance) || *vegetationDistance < 20 ||
                    *vegetationDistance > 200)
                    throw std::invalid_argument("Vegetation distance must be 20..200 m");
            } else if (arg == "--review-view")
                reviewPath = value();
            else if (arg == "--save-review-view")
                reviewSavePath = value();
            else if (arg == "--fog") {
                const auto preset = value();
                if (preset == "off")
                    fogPreset = 0;
                else if (preset == "mist")
                    fogPreset = 1;
                else if (preset == "shafts")
                    fogPreset = 2;
                else
                    throw std::invalid_argument("--fog expects off, mist or shafts");
            } else if (arg == "--lighting-demo")
                demoControls.enabled = true;
            else if (arg == "--atmosphere") {
                auto v = value();
                if (v == "off")
                    scene.atmosphere.mode = ow::AtmosphereMode::Off;
                else if (v == "analytic")
                    scene.atmosphere.mode = ow::AtmosphereMode::Analytic;
                else if (v == "physical")
                    scene.atmosphere.mode = ow::AtmosphereMode::Physical;
                else
                    throw std::invalid_argument("--atmosphere expects off, analytic or physical");
            } else if (arg == "--no-horizon") {
                options.streaming.horizonBytes = 0;
                options.curvedHorizon = false;
            } else if (arg == "--material-dir")
                options.materialDirectory = value();
            else if (arg == "--no-material-assets")
                options.materialDirectory.clear();
            else if (arg == "--fixed-materials")
                options.forceFixedMaterials = true;
            else if (arg == "--rgba-materials")
                options.forceRgbaMaterials = true;
            else if (arg == "--constant-material")
                scene.terrainMaterial.enabled = false;
            else if (arg == "--legacy-lighting")
                scene.shading = ow::ShadingMode::Legacy;
            else if (arg == "--no-depth-prepass")
                scene.opaqueDepthPrepass = false;
            else if (arg == "--terrain-triangles") {
                const auto n = std::stoull(value());
                if (n < 8192 || n > 33554432)
                    throw std::invalid_argument("Terrain triangle budget must be 8192..33554432");
                options.lod.triangleBudget = n;
            } else if (arg == "--aa") {
                const auto mode = value();
                if (mode == "off")
                    aaOverride = ow::AntialiasingMode::Off;
                else if (mode == "fxaa")
                    aaOverride = ow::AntialiasingMode::Fxaa;
                else if (mode == "smaa")
                    aaOverride = ow::AntialiasingMode::Smaa1x;
                else if (mode == "taa")
                    aaOverride = ow::AntialiasingMode::Taa;
                else
                    throw std::invalid_argument("--aa requires off, fxaa, smaa or taa");
            } else if (arg == "--no-taa")
                aaOverride = ow::AntialiasingMode::Off;
            else if (arg == "--no-bloom")
                scene.bloom.intensity = 0;
            else if (arg == "--bloom-intensity")
                scene.bloom.intensity = std::stof(value());
            else if (arg == "--agx")
                scene.output.toneMapper = ow::ToneMapper::Agx;
            else if (arg == "--ev100")
                scene.output.ev100 = std::stof(value());
            else if (arg == "--render-height") {
                const auto text = value();
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("--render-height requires an integer in 1..16384");
                const auto height = std::stoull(text);
                if (!height || height > 16384)
                    throw std::invalid_argument("--render-height must be in 1..16384");
                options.renderHeight = uint32_t(height);
            } else if (arg == "--headless")
                headless = true;
            else if (arg == "--single-chunk")
                singleChunk = true;
            else if (arg == "--cache-dir")
                options.streaming.cacheDirectory = value();
            else if (arg == "--no-cache")
                options.streaming.diskCache = false;
            else if (arg == "--view-radius" || arg == "--terrain-gpu-mib" || arg == "--terrain-host-mib" ||
                     arg == "--cache-mib") {
                const auto text = value();
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument(arg + " requires an integer");
                const auto n = std::stoull(text);
                if (arg == "--view-radius") {
                    if (n > 4096)
                        throw std::invalid_argument(
                            "View radius must be 0..4096, subject to base chunk size");
                    explicitRadius = true;
                    scene.viewRadius = int(n);
                } else {
                    if (!n || n > 1048576)
                        throw std::invalid_argument("Memory/cache budget must be 1..1048576 MiB");
                    auto &budget = arg == "--terrain-gpu-mib"    ? options.streaming.gpuBytes
                                   : arg == "--terrain-host-mib" ? options.streaming.hostBytes
                                                                 : options.streaming.diskBytes;
                    budget = n * ow::terrainMiB;
                }
            } else if (arg == "--chunk-x" || arg == "--chunk-z") {
                const auto text = value();
                size_t end = 0;
                const auto coordinate = std::stoll(text, &end);
                if (end != text.size())
                    throw std::runtime_error(arg + " requires a signed integer");
                (arg == "--chunk-x" ? terrain.chunk.x : terrain.chunk.z) = coordinate;
            } else if (arg == "--no-lod")
                scene.lod = false;
            else if (arg == "--lod-colours")
                scene.lodColours = true;
            else if (arg == "--chunk-borders")
                scene.chunkBorders = true;
            else if (arg == "--vertex-normals")
                scene.filteredNormals = false;
            else if (arg == "--base")
                scene.eroded = false;
            else if (arg == "--no-ui")
                options.ui = false;
            else if (arg == "--terrain-size") {
                const auto text = value();
                size_t end = 0;
                const double number = std::stod(text, &end);
                if (end != text.size())
                    throw std::runtime_error(arg + " requires a number");
                terrainSize = number;
                explicitSize = true;
            } else if (arg == "--terrain-cells") {
                const auto text = value();
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("--terrain-cells requires an integer in 8..1024");
                const auto cells = std::stoull(text);
                if (cells < 8 || cells > 1024)
                    throw std::runtime_error("--terrain-cells must be in 8..1024");
                terrainCells = uint32_t(cells);
            } else if (arg == "--frames") {
                const auto text = value();
                if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("--frames requires a positive integer");
                frameLimit = std::stoull(text);
                if (!frameLimit)
                    throw std::runtime_error("--frames must be greater than zero");
            } else if (arg == "--capture")
                capture = value();
            else if (arg == "--shader-dir")
                options.shaderDirectory = value();
            else if (arg == "--validation")
                options.validation = true;
            else if (arg == "--no-validation")
                options.validation = false;
            else if (arg == "--require-validation")
                options.requireValidation = true;
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (terrainCells && explicitSize)
            throw std::runtime_error("Choose --terrain-cells or --terrain-size, not both");
        options.terrainGrid = ow::terrainGrid(terrainCells ? terrainCells * 0.5 : terrainSize, 0.5f);
        options.terrainCapacity = options.terrainGrid.cells;
        if (!explicitRadius)
            scene.viewRadius = std::max(1, int(8192.0 / options.terrainGrid.extent()));
        if (scene.viewRadius > ow::terrainMaximumRadius(options.terrainGrid))
            throw std::invalid_argument("View radius exceeds 16 km for this chunk footprint");
        options.lod.patchCells = std::min(32u, options.terrainGrid.cells);
        ow::validateLodGrid(options.terrainGrid, options.lod.patchCells);
        options.streaming.enabled = !singleChunk;
        if (options.streaming.cacheDirectory.empty())
            options.streaming.cacheDirectory = ow::terrainDefaultCacheDirectory();
        if (options.streaming.cacheDirectory.empty())
            options.streaming.diskCache = false;
        options.terrainChunkCapacity = singleChunk ? 1 : 9;
        ow::validateBatch(options.terrainGrid, ow::terrainNeighbourhood(terrain, singleChunk),
                          options.terrainChunkCapacity);
        if (!reviewPath.empty()) {
            review = ow::loadReviewView(reviewPath);
            ow::applyReviewStartup(options, *review);
            scene = review->scene;
            terrain = {review->terrainOrigin, review->terrain};
            singleChunk = !options.streaming.enabled;
        }
        if (aaOverride)
            scene.antialiasing = *aaOverride;
        if (vegetationEnabled)
            scene.vegetation.enabled = *vegetationEnabled;
        if (vegetationDistance)
            scene.vegetation.distance = *vegetationDistance;
        if (options.streaming.enabled && options.streaming.hierarchical) {
            options.vegetation.library = std::make_shared<ow::VegetationLibrary>(
                ow::readVegetationLibrary(OPENWORLD_VEGETATION_LIBRARY_DIR));
            options.vegetation.streaming.enabled = true;
            options.vegetation.streaming.cellCapacity = 12288;
            options.vegetation.streaming.demandRadius = 216;
            options.vegetation.streaming.retentionRadius = 232;
            options.vegetation.budgetBytes = 256ull << 20;
        }
        if (!options.vegetation.streaming.enabled)
            scene.vegetation.enabled = false;
        std::string terrainError;
        if (options.requireValidation)
            options.validation = true;
        if (!capture.empty() && !headless)
            throw std::runtime_error("--capture currently requires --headless");
        if (headless && !frameLimit)
            frameLimit = 8;

        Platform platform;
        if (!headless) {
            if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
                throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
            platform.sdl = true;
            platform.window =
                SDL_CreateWindow("OpenWorld | Terrain", 1920, 1080,
                                 SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!platform.window)
                throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        platform.imgui = true;
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 4.0f;
        style.WindowPadding = ImVec2(18, 18);
        if (platform.window && options.ui) {
            if (!ImGui_ImplSDL3_InitForVulkan(platform.window))
                throw std::runtime_error("ImGui SDL3 initialization failed");
            platform.imguiPlatform = true;
        }

        ow::EngineUi ui;
        std::filesystem::path uiStatePath;
        if (!headless && options.ui) {
            if (char *pref = SDL_GetPrefPath("OpenWorld", "OpenWorld")) {
                uiStatePath = std::filesystem::path(pref) / "ui-state.json";
                SDL_free(pref);
                try {
                    ow::loadUiState(uiStatePath, ui.state);
                } catch (const std::exception &error) {
                    std::cerr << "UI layout not restored: " << error.what() << '\n';
                }
            } else
                std::cerr << "UI settings path unavailable: " << SDL_GetError() << '\n';
        }

        options.window = platform.window;
        ow::Renderer renderer(options); // Destroyed before the ImGui context and SDL window.
        renderer.requestTerrainBatch(ow::terrainNeighbourhood(terrain, singleChunk));
        ow::Camera camera;
        bool fitSingleChunkCamera = false;
        const auto resetTerrainCamera = [&] {
            fitSingleChunkCamera = singleChunk;
            camera.reset(options.terrainGrid, terrain.chunk, terrain.settings.heightScale * 1.25f);
            if (singleChunk) {
                // Aim into the small inspection patch even when the conservative
                // mountain bound is far above the local plain.
                camera.pitch = -std::atan2(float(camera.local.y + terrain.settings.heightScale * 0.2f),
                                           float(options.terrainGrid.extent() * 0.45));
            } else {
                const double extent = options.terrainGrid.extent();
                camera.local = {extent * 0.5, terrain.settings.heightScale * 1.25 + extent * 1.65,
                                extent * 2.6};
                camera.update({}, 0, options.terrainGrid);
            }
        };
        resetTerrainCamera();
        if (review) {
            camera = review->camera;
            fitSingleChunkCamera = false;
            renderer.setTerrainLodReviewState(review->lod);
        }
        if (reviewPath.empty()) {
            if (char *pref = SDL_GetPrefPath("OpenWorld", "OpenWorld")) {
                reviewPath = std::filesystem::path(pref) / "review-view.json";
                SDL_free(pref);
            } else
                reviewStatus = "Review path unavailable: " + std::string(SDL_GetError());
        }
        const auto reviewPathLabel = reviewPath.string();
        const auto resetCamera = [&] { resetTerrainCamera(); };
        ow::VolumetricDemo fogDemo(renderer);
        const auto applyFogPreset = [&](uint32_t preset) {
            scene.volumetrics.enabled = preset != 0;
            scene.volumetrics.extinction = preset == 1 ? .0008f : .0002f;
            scene.volumetrics.height = float(camera.local.y);
            scene.volumetrics.scaleHeight = 40;
            scene.volumetrics.localDensity = 1;
            scene.volumetrics.samples = 2;
            scene.volumetrics.anisotropy = preset == 2 ? .7f : .5f;
            auto anchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                         glm::dvec3(camera.forward()) * 45.);
            fogDemo.configure(preset, anchor);
        };
        if (fogPreset)
            applyFogPreset(fogPreset);
        ow::LightingDemo lightingDemo(renderer);
        bool demoDirty = demoControls.enabled;
        ow::WorldPosition demoAnchor;
        float demoSeconds = 0;
        bool captured = false;
        auto captureMouse = [&](bool enable) {
            if (platform.window && !SDL_SetWindowRelativeMouseMode(platform.window, enable))
                throw std::runtime_error(SDL_GetError());
            captured = enable;
            if (enable && options.ui)
                ImGui::SetWindowFocus(nullptr);
        };
        bool running = true;
        bool fullscreenRequested = false;
        uint64_t frames = 0;
        auto previous = std::chrono::steady_clock::now();
        while (running && (!frameLimit || frames < frameLimit)) {
            ow::CameraInput input;
            SDL_Event event;
            while (platform.window && SDL_PollEvent(&event)) {
                if (options.ui)
                    ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    running = false;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                    event.type == SDL_EVENT_WINDOW_RESTORED ||
                    event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
                    event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
                    renderer.resize();
                if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN)
                    fullscreenRequested = true;
                if (event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
                    fullscreenRequested = false;
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F10) {
                    const bool next = !fullscreenRequested;
                    if (SDL_SetWindowFullscreen(platform.window, next))
                        fullscreenRequested = next;
                    else
                        std::cerr << "Fullscreen request failed: " << SDL_GetError() << '\n';
                }
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MINIMIZED)
                    captureMouse(false);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT &&
                    !ImGui::GetIO().WantCaptureMouse)
                    captureMouse(true);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
                    captureMouse(false);
                if (event.type == SDL_EVENT_MOUSE_MOTION && captured)
                    input.look += glm::vec2(event.motion.xrel, event.motion.yrel);
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_ESCAPE) {
                    if (captured)
                        captureMouse(false);
                    else if (!ImGui::GetIO().WantCaptureKeyboard)
                        running = false;
                }
            }
            if (!running)
                break;
            if (platform.window && (SDL_GetWindowFlags(platform.window) & SDL_WINDOW_MINIMIZED)) {
                SDL_Delay(16);
                previous = std::chrono::steady_clock::now();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const float dt =
                headless ? 1.0f / 60.0f
                         : std::clamp(std::chrono::duration<float>(now - previous).count(), 0.0001f, 0.1f);
            previous = now;
            if (captured) {
                const bool *keys = SDL_GetKeyboardState(nullptr);
                input.movement = glm::vec3(float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]),
                                           float(keys[SDL_SCANCODE_E]) - float(keys[SDL_SCANCODE_Q]),
                                           float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S]));
                input.boost = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                auto &activeCamera = camera;
                activeCamera.update(input, dt, options.terrainGrid);
            }

            if (options.ui) {
                ImGui_ImplVulkan_NewFrame();
                if (platform.window)
                    ImGui_ImplSDL3_NewFrame();
                else {
                    ImGui::GetIO().DisplaySize = ImVec2(float(renderer.width()), float(renderer.height()));
                    ImGui::GetIO().DeltaTime = dt;
                }
                ImGui::NewFrame();
                ow::UiContext uiContext{renderer,     options,      scene,        terrain,        camera,
                                        demoControls, terrainError, reviewStatus, reviewPathLabel};
                const auto actions = ui.draw(uiContext);
                if (actions.renderHeightChanged)
                    renderer.setRenderHeight(options.renderHeight);
                if (actions.reloadPalette)
                    renderer.reloadMaterials(options.materialDirectory);
                if (actions.resetCamera)
                    resetCamera();
                if (actions.fogPreset >= 0)
                    applyFogPreset(uint32_t(actions.fogPreset));
                if (actions.lightingDemoReanchor)
                    demoAnchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                                glm::dvec3(camera.forward()) * 14.);
                demoDirty |= actions.lightingDemoChanged;
                if (actions.saveReview) {
                    try {
                        if (reviewPath.empty())
                            throw std::runtime_error("Review path unavailable");
                        ow::ReviewView saved;
                        saved.camera = camera;
                        saved.terrain = terrain.settings;
                        saved.terrainOrigin = terrain.chunk;
                        saved.scene = scene;
                        saved.lod = renderer.terrainLodReviewState();
                        saved.startup = options;
                        saved.startup.width = renderer.width();
                        saved.startup.height = renderer.height();
                        ow::saveReviewView(reviewPath, saved);
                        reviewStatus = "Saved " + reviewPath.string();
                    } catch (const std::exception &e) {
                        reviewStatus = e.what();
                    }
                }
                if (actions.loadReview) {
                    try {
                        const auto saved = ow::loadReviewView(reviewPath);
                        if (!ow::compatibleReviewStartup(options, saved))
                            throw std::runtime_error("Startup differs. Restart with --review-view \"" +
                                                     reviewPath.string() + "\"");
                        renderer.requestTerrainBatch(
                            ow::terrainNeighbourhood({saved.terrainOrigin, saved.terrain}, singleChunk));
                        camera = saved.camera;
                        terrain = {saved.terrainOrigin, saved.terrain};
                        scene = saved.scene;
                        fitSingleChunkCamera = false;
                        renderer.setTerrainLodReviewState(saved.lod);
                        options.renderHeight = saved.startup.renderHeight;
                        renderer.setRenderHeight(options.renderHeight);
                        reviewStatus = "Loaded " + reviewPath.string();
                    } catch (const std::exception &e) {
                        reviewStatus = e.what();
                    }
                }
                if (actions.regenerate) {
                    try {
                        renderer.requestTerrainBatch(ow::terrainNeighbourhood(terrain, singleChunk));
                        terrainError.clear();
                    } catch (const std::invalid_argument &error) {
                        terrainError = error.what();
                    }
                }
                ImGui::Render();
            }
            if (demoDirty) {
                if (frames == 0)
                    demoAnchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                                glm::dvec3(camera.forward()) * 14.);
                lightingDemo.configure(demoControls.enabled, demoAnchor, uint32_t(demoControls.lights),
                                       demoControls.overlap, uint32_t(demoControls.layers));
                demoDirty = false;
            }
            if (demoControls.enabled && demoControls.animation) {
                demoSeconds += dt;
                lightingDemo.animate(demoSeconds);
            }
            scene.vegetation.time += dt;
            if (renderer.render(camera, scene)) {
                ++frames;
                if (fitSingleChunkCamera && scene.terrainVisible) {
                    // Startup/explicit reset only, never ordinary navigation:
                    // fit the small inspection patch using its actual mesh bound.
                    float highest = 0;
                    for (const auto &sample : renderer.readTerrainSnapshot())
                        highest = std::max(highest, std::max(sample.base.w, sample.eroded.w));
                    camera.reset(options.terrainGrid, terrain.chunk, highest);
                    renderer.resetTemporalHistory();
                    fitSingleChunkCamera = false;
                    if (demoControls.enabled) {
                        demoAnchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                                    glm::dvec3(camera.forward()) * 14.);
                        demoDirty = true;
                    }
                }
            } else if (platform.window)
                SDL_Delay(16);
        }
        if (!uiStatePath.empty()) {
            try {
                ow::saveUiState(uiStatePath, ui.state);
            } catch (const std::exception &error) {
                std::cerr << "UI layout not saved: " << error.what() << '\n';
            }
        }
        renderer.waitIdle();
        if (!reviewSavePath.empty()) {
            ow::ReviewView saved;
            saved.camera = camera;
            saved.terrain = terrain.settings;
            saved.terrainOrigin = terrain.chunk;
            saved.scene = scene;
            saved.lod = renderer.terrainLodReviewState();
            saved.startup = options;
            saved.startup.width = renderer.width();
            saved.startup.height = renderer.height();
            ow::saveReviewView(reviewSavePath, saved);
        }

        if (!capture.empty() && frames)
            renderer.capture(capture);
        std::cout << "Terrain: " << options.terrainGrid.extent() << " m square, "
                  << options.terrainGrid.spacing << " m spacing, " << options.terrainGrid.cells
                  << " cells/side\n";
        std::cout << "Chunks: " << renderer.terrainChunkCount()
                  << "; triangles: " << renderer.terrainRenderStats().triangles
                  << "; terrain allocation: " << renderer.terrainAllocatedBytes() / 1048576.0 << " MiB\n";
        std::cout << "Selected patches: " << renderer.terrainRenderStats().selected
                  << "; culled: " << renderer.terrainRenderStats().culled
                  << "; first LOD range: " << renderer.terrainRenderStats().firstRange << " m\n";
        if (options.streaming.enabled) {
            const auto &s = renderer.terrainStreamingStats();
            std::cout << "Streaming: coverage " << s.coverage << ", detail " << s.detail << ", pending "
                      << s.pending << ", cache hits " << s.hits << ", misses " << s.misses << ", errors "
                      << s.errors << '\n';
        }
        const auto &renderStats = renderer.renderingStats();
        std::cout << "HDR: " << renderStats.hdrBytes / 1048576.0
                  << " MiB; lighting: " << renderStats.lightingBytes / 1048576.0
                  << " MiB; scene: " << renderStats.sceneMilliseconds
                  << " ms; output + UI: " << renderStats.outputMilliseconds
                  << " ms; lighting startup: " << renderStats.lightingStartupMilliseconds << " ms\n";
        std::cout << "Terrain generations: " << renderer.terrainGenerationCount() << '\n';
        std::cout << "Rendered " << frames << " frames; validation errors: " << renderer.validationErrors()
                  << '\n';
        return renderer.validationErrors() ? EXIT_FAILURE : EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "OpenWorld: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

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
#include <cstdio>
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
                 "  --review-view PATH    Replay a saved camera and rendering settings\n"
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
                 "  --atmosphere MODE     off, analytic or physical (default physical)\n"
                 "  --no-horizon          Disable distant coverage and visual curvature\n"
                 "  --fog PRESET          off, mist or shafts (default off)\n"
                 "  --lighting-demo       Enable generated lights and mesh fixtures\n"
                 "  --material-dir PATH   Override imported terrain palette directory\n"
                 "  --no-material-assets  Use the constant fallback without loading assets\n"
                 "  --fixed-materials     Force fixed-binding material fallback\n"
                 "  --rgba-materials      Force uncompressed material textures\n"
                 "  --constant-material   Disable textured terrain for comparison\n"
                 "  --legacy-lighting     Use the old diagnostic shading\n"
                 "  --agx                 Use AgX neutral instead of ACES fitted\n"
                 "  --no-depth-prepass    Disable default opaque depth prepass\n"
                 "  --terrain-triangles N Terrain triangle budget (default 2097152)\n"
                 "  --no-taa              Disable temporal antialiasing\n"
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
        bool headless = false, lightingDemoEnabled = false;
        uint32_t fogPreset = 0;
        uint64_t frameLimit = 0;
        ow::SceneSettings scene;
        scene.taa.enabled = true;
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
            if (arg == "--review-view")
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
                lightingDemoEnabled = true;
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
            } else if (arg == "--no-taa")
                scene.taa.enabled = false;
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
        int demoLightCount = 32, demoLayers = 3;
        bool demoOverlap = false, demoAnimation = false, demoDirty = lightingDemoEnabled;
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
        bool demo = false;
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
                bool regenerate = false;
                const ImVec2 viewport = ImGui::GetIO().DisplaySize;
                const float maxWidth = std::max(100.f, viewport.x - 48);
                const float maxHeight = std::max(100.f, viewport.y - 48);
                const auto setupWindow = [&](ImVec2 position, float height) {
                    ImGui::SetNextWindowPos(position, ImGuiCond_FirstUseEver);
                    ImGui::SetNextWindowSize(ImVec2(std::min(390.f, maxWidth), std::min(height, maxHeight)),
                                             ImGuiCond_FirstUseEver);
                    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(280.f, maxWidth), 100),
                                                        ImVec2(maxWidth, maxHeight));
                };
                setupWindow(ImVec2(24, 24), 950);
                if (ImGui::Begin("OpenWorld")) {
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1), "VULKAN / TERRAIN");
                    ImGui::Spacing();
                    ImGui::TextWrapped("%s", renderer.deviceName().c_str());
                    ImGui::Text("Scene: %u x %u | Output: %u x %u", renderer.renderWidth(),
                                renderer.renderHeight(), renderer.width(), renderer.height());
                    ImGui::Text("%.1f FPS | F10: fullscreen", ImGui::GetIO().Framerate);
                    char renderHeightLabel[32];
                    std::snprintf(renderHeightLabel, sizeof(renderHeightLabel), "%u pixels",
                                  options.renderHeight);
                    if (ImGui::BeginCombo("Render height", renderHeightLabel)) {
                        for (uint32_t height : {720u, 900u, 1080u, 1440u, 2160u}) {
                            char label[32];
                            std::snprintf(label, sizeof(label), "%u pixels", height);
                            if (ImGui::Selectable(label, options.renderHeight == height)) {
                                options.renderHeight = height;
                                renderer.setRenderHeight(height);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::Text("Validation: %s", renderer.validationEnabled() ? "enabled" : "disabled");
                    ImGui::Text("Validation errors: %u", renderer.validationErrors());
                    ImGui::Separator();
                    ImGui::PushItemWidth(160);
                    ImGui::Text("Chunk size: %.1f m (startup setting)", options.terrainGrid.extent());
                    if (!terrainError.empty())
                        ImGui::TextWrapped("%s (terrain unchanged)", terrainError.c_str());
                    if (options.streaming.enabled)
                        ImGui::SliderInt("View radius (base chunks)", &scene.viewRadius, 0,
                                         ow::terrainMaximumRadius(options.terrainGrid));
                    ImGui::SliderFloat("Erosion wavelength (m)", &terrain.settings.erosionWavelength, 64,
                                       8192, "%.0f",
                                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Erosion strength", &terrain.settings.erosionStrength, 0, 1, "%.3f",
                                       ImGuiSliderFlags_AlwaysClamp);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Crease rounding", &terrain.settings.creaseRounding, 0, 1, "%.3f",
                                       ImGuiSliderFlags_AlwaysClamp);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::Checkbox("Show eroded terrain", &scene.eroded);
                    ImGui::Checkbox("Terrain LOD", &scene.lod);
                    ImGui::SameLine();
                    ImGui::Checkbox("Frustum culling", &scene.frustumCulling);
                    int visibilityMode = int(scene.visibility);
                    if (ImGui::Combo("Visibility", &visibilityMode, "Direct\0Indirect\0Hi-Z\0"))
                        scene.visibility = ow::VisibilityMode(visibilityMode);
                    int shadowMode = int(scene.shadows.mode);
                    if (ImGui::Combo("Sun shadows", &shadowMode, "Off\0CSM\0"))
                        scene.shadows.mode = ow::ShadowMode(shadowMode);
                    ImGui::SliderFloat("Near shadows (m)", &scene.shadows.nearDistance, 100, 4000);
                    ImGui::SliderFloat("Far shadows (0 = view)", &scene.shadows.farDistance, 0, 16000);
                    ImGui::SliderFloat("Shadow bias (texels)", &scene.shadows.biasTexels, 0, 4);
                    ImGui::SliderFloat("Normal bias (texels)", &scene.shadows.normalBiasTexels, 0, 2);
                    int shadowView = int(scene.shadows.diagnostic);
                    if (ImGui::Combo("Shadow view", &shadowView, "Shaded\0Visibility\0Cascades\0Coverage\0"))
                        scene.shadows.diagnostic = uint32_t(shadowView);
                    const auto &shadowStats = renderer.shadowStats();
                    ImGui::Text("Shadow casters: %u | overflow %u | incomplete %u", shadowStats.casters,
                                shadowStats.overflow, shadowStats.incomplete);
                    uint64_t shadowTriangles = 0;
                    for (auto triangles : shadowStats.triangles)
                        shadowTriangles += triangles;
                    ImGui::Text("Shadow raster triangles: %llu", (unsigned long long)shadowTriangles);
                    ImGui::Text("Shadow storage: %.2f MiB | range %.0f / %.0f m",
                                double(shadowStats.allocatedBytes) / (1024 * 1024), shadowStats.nearDistance,
                                shadowStats.farDistance);
                    const char *shadowFallbacks[] = {"none", "sampled depth unavailable", "caster capacity",
                                                     "disabled/wireframe"};
                    ImGui::Text("Effective shadows: %s | %s",
                                shadowStats.effective == ow::ShadowMode::CSM ? "CSM" : "Off",
                                shadowFallbacks[unsigned(shadowStats.fallback)]);
                    ImGui::Text("Shadow preparation: %.2f ms", shadowStats.preparationMilliseconds);
                    if (shadowStats.timingsAvailable)
                        ImGui::Text("Shadow GPU: %.2f ms", shadowStats.totalMilliseconds);
                    if (ImGui::TreeNode("Cascade details")) {
                        for (uint32_t i = 0; i < ow::shadowCascades; ++i)
                            ImGui::Text("%u: %u casters | %u tris | %.2f ms", i + 1,
                                        shadowStats.cascadeCasters[i], shadowStats.triangles[i],
                                        shadowStats.rasterMilliseconds[i]);
                        ImGui::TreePop();
                    }
                    int aoMode = int(scene.gtao.mode);
                    if (ImGui::Combo("GTAO", &aoMode, "Off\0Half\0Full\0"))
                        scene.gtao.mode = ow::GtaoMode(aoMode);
                    ImGui::SliderFloat("AO radius (m)", &scene.gtao.radius, .1f, 5.f, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp);
                    int aoDiagnostic = int(scene.gtao.diagnostic);
                    if (ImGui::Combo("AO view", &aoDiagnostic, "Shaded\0Visibility\0Raw\0Normals\0Edges\0"))
                        scene.gtao.diagnostic = ow::GtaoDiagnostic(aoDiagnostic);
                    ImGui::Checkbox("Opaque depth prepass", &scene.opaqueDepthPrepass);
                    if (scene.visibility == ow::VisibilityMode::HiZ ||
                        renderer.gtaoStats().effective != ow::GtaoMode::Off)
                        ImGui::TextUnformatted("Hi-Z / GTAO requires opaque depth prepass");
                    if (ImGui::CollapsingHeader("Rendering diagnostics")) {
                        ImGui::Checkbox("Filtered terrain normals", &scene.filteredNormals);
                        ImGui::Checkbox("Normal colours", &scene.normalView);
                        ImGui::SameLine();
                        ImGui::Checkbox("Wireframe", &scene.wireframe);
                        ImGui::Checkbox("Chunk borders", &scene.chunkBorders);
                        ImGui::SameLine();
                        ImGui::Checkbox("LOD colours", &scene.lodColours);
                        ImGui::Checkbox("Freeze LOD camera", &scene.freezeLod);
                        ImGui::Checkbox("Additional error refinement", &scene.terrainLodQuality);
                        ImGui::SliderFloat("LOD error (pixels)", &scene.terrainLodPixelError, .5f, 8.f,
                                           "%.1f");
                        ImGui::Checkbox("LOD error colours", &scene.terrainLodErrorView);
                    }
                    ImGui::SliderFloat("Flight speed (m/s)", &camera.speed, 1, 1000, "%.0f",
                                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
                    if (ImGui::Button("Reset camera"))
                        resetCamera();
                    if (ImGui::Button("Save review view")) {
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
                    ImGui::SameLine();
                    if (ImGui::Button("Load review view")) {
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
                    if (!reviewStatus.empty())
                        ImGui::TextWrapped("%s", reviewStatus.c_str());
                    ImGui::ColorEdit3("Background", scene.background);
                    ImGui::PopItemWidth();
                    ImGui::Checkbox("ImGui demo", &demo);
                    ImGui::Separator();
                    if (!options.streaming.enabled)
                        ImGui::Text("Centre: (%lld, %lld) | %u chunks",
                                    static_cast<long long>(terrain.chunk.x),
                                    static_cast<long long>(terrain.chunk.z), renderer.terrainChunkCount());
                    if (options.streaming.enabled) {
                        const auto &s = renderer.terrainStreamingStats();
                        ImGui::Text("Radius: %d requested / %d effective (max %d)", scene.viewRadius,
                                    std::min(scene.viewRadius, s.maximumRadius), s.maximumRadius);
                        ImGui::Text("View: %.2f km requested / %.2f km effective",
                                    scene.viewRadius * options.terrainGrid.extent() / 1000,
                                    s.effectiveRadius * options.terrainGrid.extent() / 1000);
                        ImGui::Text("Resident: %u coverage / %u detail", s.coverage, s.detail);
                        ImGui::Text("Shadow terrain: %u reserved slots | %u requested | %u "
                                    "missing | %.2f MiB",
                                    s.shadowSlots, s.shadowRequested, s.shadowMissing,
                                    double(s.shadowBytes) / ow::terrainMiB);
                        ImGui::Text("Detail demand: %u eligible / %u selected / %u ready", s.detailEligible,
                                    s.detailSelected, s.detailReady);
                        for (uint32_t l = 0; l < s.residentLevels.size(); ++l)
                            if (s.residentLevels[l])
                                ImGui::Text("Tile L%u: %u resident", l, s.residentLevels[l]);
                        ImGui::Text("Pool deferrals: %llu", (unsigned long long)s.pressureEvents);
                        ImGui::Text("Pending: %u coverage / %u detail / %u I/O", s.pending, s.detailPending,
                                    s.ioJobs);
                        ImGui::Text("View footprint: %.0f m square",
                                    options.terrainGrid.extent() *
                                        (2 * std::min(scene.viewRadius, s.maximumRadius) + 1));
                        ImGui::Text("Cache: %llu hits / %llu misses / %llu errors",
                                    (unsigned long long)s.hits, (unsigned long long)s.misses,
                                    (unsigned long long)s.errors);
                        ImGui::Text("Host working storage: %.1f MiB", s.hostBytes / double(ow::terrainMiB));
                        ImGui::Text("Generation recorded: %u batches / %u stages", s.dispatches,
                                    s.generationSegments);
                        ImGui::Text("Estimated: %.2f / %.2f ms; %s", s.generationEstimatedMilliseconds,
                                    ow::TerrainGenerationBudget::targetMilliseconds,
                                    ow::terrainGenerationStopName(s.generationStop));
                        ImGui::Text("Completed GPU: %.2f ms / %u groups / %u stages",
                                    s.generationMilliseconds, s.completedGenerationGroups,
                                    s.completedGenerationSegments);
                        ImGui::Text("Transfer: %.2f MiB", s.transferBytes / double(ow::terrainMiB));
                        ImGui::Text("Detail cancel/preempt: %llu / %llu; stale reads: %llu",
                                    (unsigned long long)s.detailCancellations,
                                    (unsigned long long)s.detailPreemptions,
                                    (unsigned long long)s.discardedReads);
                        if (s.activeDetail || s.detailRetiring)
                            ImGui::Text("Detail (%lld,%lld L%u): %.0f%% %s", (long long)s.activeDetailTile.x,
                                        (long long)s.activeDetailTile.z, s.activeDetailTile.level,
                                        s.activeDetailProgress * 100,
                                        s.detailRetiring ? "retiring" : "working");
                        ImGui::Text("Recorded groups: %u / %u hard limit", s.generationGroups,
                                    s.generationGroupBudget);
                        ImGui::Text("Terrain GPU: %.2f ms", s.terrainMilliseconds);
                    }
                    ImGui::Text("Camera chunk: (%lld, %lld)", static_cast<long long>(camera.chunk.x),
                                static_cast<long long>(camera.chunk.z));
                    ImGui::Text("Mesh: %.2f m | Normals: %.2f m", options.terrainGrid.spacing,
                                options.terrainGrid.spacing / options.normalSubdivisions);
                    ImGui::Text("%u x %u cells/chunk", options.terrainGrid.cells, options.terrainGrid.cells);
                    const auto &stats = renderer.terrainRenderStats();
                    ImGui::Text("CPU selected: %llu triangles | %u patches",
                                static_cast<unsigned long long>(stats.triangles), stats.selected);
                    ImGui::Text("Culled: %u | Bounds: %s", stats.culled,
                                stats.boundsReady ? "ready" : "pending");
                    ImGui::Text("LOD first range: %.1f m", stats.firstRange);
                    ImGui::Text("LOD error: %.1f px | %u over target", stats.maximumErrorPixels,
                                stats.overTargetPatches);
                    if (options.streaming.hierarchical) {
                        ImGui::Text("Replacement: %.2f px | refined %u", stats.maximumTransitionPixels,
                                    stats.refined);
                        ImGui::Text("Ground: %u patches | %u limited", stats.protectedPatches,
                                    stats.protectedMissing);
                        ImGui::Text("Budget: %llu tris | %u deferred",
                                    (unsigned long long)stats.triangleBudget, stats.budgetLimited);
                        ImGui::Text("Residency limited: %u", stats.residencyLimited);
                    }
                    for (uint32_t level = 0; level < stats.perLevel.size(); ++level)
                        if (stats.perLevel[level])
                            ImGui::Text("LOD %u: %u patches | %llu triangles (%.1f m)", level,
                                        stats.perLevel[level],
                                        (unsigned long long)stats.trianglesPerLevel[level],
                                        stats.firstRange * float(1u << level));
                    ImGui::Text("Terrain allocation: %.2f MiB", renderer.terrainAllocatedBytes() / 1048576.0);
                    ImGui::Text("Chunk generations: %llu",
                                static_cast<unsigned long long>(renderer.terrainGenerationCount()));
                    ImGui::TextWrapped("Terrain edits apply on release. Base/eroded "
                                       "comparison uses cached heights and normals.");
                    ImGui::TextWrapped("Hold RMB: look + WASD/QE fly. Shift: boost. "
                                       "Escape: release / quit.");
                }
                ImGui::End();
                setupWindow(ImVec2(viewport.x >= 828 ? 438.f : 24.f, viewport.x >= 828 ? 24.f : 64.f), 270);
                if (ImGui::Begin("Base Terrain Generation")) {
                    if (!terrainError.empty())
                        ImGui::TextWrapped("%s (terrain unchanged)", terrainError.c_str());
                    ImGui::PushItemWidth(150);
                    ImGui::InputScalar("Seed", ImGuiDataType_U32, &terrain.settings.seed);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Regional wavelength (m)", &terrain.settings.regionWavelength, 4096,
                                       65536, "%.0f",
                                       ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Mountain wavelength (m)", &terrain.settings.wavelength, 64, 8192,
                                       "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Height scale (m)", &terrain.settings.heightScale, 0, 2000, "%.0f",
                                       ImGuiSliderFlags_AlwaysClamp);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SliderFloat("Ridge strength", &terrain.settings.ridgeStrength, 0, 1, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0: rounded hills. 1: ridged mountains. Changes "
                                          "the base before erosion.");
                    ImGui::SliderFloat("Base detail", &terrain.settings.baseDetail, 0, 1, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp);
                    regenerate |= ImGui::IsItemDeactivatedAfterEdit();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("0: broad, smooth landforms. 1: full smaller-scale detail.\n"
                                          "Set both sliders to 1 for the original terrain shape.");
                    ImGui::PopItemWidth();
                    ImGui::TextWrapped("Terrain edits apply on release. Use Show eroded "
                                       "terrain in OpenWorld to compare.");
                }
                ImGui::End();
                setupWindow(ImVec2(std::max(24.f, viewport.x - 414.f), 300), 450);
                if (ImGui::Begin("Lighting & Materials")) {
                    ImGui::PushItemWidth(155);
                    if (ImGui::Checkbox("Lighting demo", &lightingDemoEnabled)) {
                        demoAnchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                                    glm::dvec3(camera.forward()) * 14.);
                        demoDirty = true;
                    }
                    if (lightingDemoEnabled) {
                        demoDirty |= ImGui::SliderInt("Local lights", &demoLightCount, 0, 4096);
                        demoDirty |= ImGui::Checkbox("Overlapping lights", &demoOverlap);
                        demoDirty |= ImGui::SliderInt("Transparent layers", &demoLayers, 0, 16);
                        ImGui::Checkbox("Animate lights", &demoAnimation);
                        if (ImGui::Button("Evening lighting")) {
                            scene.lighting.sunLux = 1000;
                            scene.lighting.environmentNits = 30;
                            scene.output.ev100 = 9;
                        }
                    }
                    int localMode = int(scene.localLights), diagnostic = int(scene.clusterDiagnostic);
                    if (ImGui::Combo("Local-light path", &localMode, "Disabled\0Brute force\0Clustered\0"))
                        scene.localLights = ow::LocalLightMode(localMode);
                    if (ImGui::Combo("Clusters", &diagnostic, "Shaded\0Occupancy\0Overflow\0"))
                        scene.clusterDiagnostic = uint32_t(diagnostic);
                    const auto &cs = renderer.sceneStats();
                    ImGui::Text("Lights %u | clusters %u / %u | overflow %u", cs.activeLights,
                                cs.occupiedClusters, cs.grid.count(), cs.overflowClusters);
                    ImGui::Text("Occupancy mean %.1f max %u | tile %u | slots %u", cs.meanOccupancy,
                                cs.maximumOccupancy, cs.grid.tile, cs.grid.indices);
                    ImGui::Text("Clusters %.2f MiB | geometry %.2f MiB", cs.clusterBytes / 1048576.,
                                cs.geometryBytes / 1048576.);
                    if (cs.timingsAvailable)
                        ImGui::Text("Assign %.2f | opaque %.2f | blend %.2f ms", cs.assignmentMilliseconds,
                                    cs.opaqueMilliseconds, cs.transparentMilliseconds);
                    int shading = int(scene.shading);
                    if (ImGui::Combo("Shading", &shading, "PBR\0Legacy diagnostic\0"))
                        scene.shading = ow::ShadingMode(shading);
                    if (scene.shading == ow::ShadingMode::Legacy ||
                        (scene.terrainVisible && (scene.normalView || scene.wireframe || scene.chunkBorders ||
                                                  scene.lodColours || scene.terrainLodErrorView)))
                        ImGui::TextWrapped("Diagnostic shading: exposure and tone mapping bypassed.");
                    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Checkbox("Temporal antialiasing", &scene.taa.enabled);
                        ImGui::SliderFloat("Bloom intensity", &scene.bloom.intensity, 0, .5f);
                        ImGui::SliderFloat("Bloom threshold", &scene.bloom.threshold, 0, 100000, "%.0f");
                        ImGui::SliderFloat("Bloom knee", &scene.bloom.knee, 0, 1);
                        ImGui::SliderFloat("Bloom radius", &scene.bloom.radius, .5f, 2);
                        const auto &post = renderer.postStats();
                        ImGui::Text("TAA %s | bloom %s | %.1f / %.1f MiB", post.taa ? "on" : "off",
                                    post.bloom ? "on" : "off", post.taaBytes / 1048576.,
                                    post.bloomBytes / 1048576.);
                        ImGui::Text("Post GPU %.3f / %.3f ms | history %s | reset %s", post.taaMilliseconds,
                                    post.bloomMilliseconds, post.historyValid ? "valid" : "reset",
                                    ow::temporalResetName(post.reset));
                        ImGui::Text("Post availability: %s / %s | rejected %u | overflow %u",
                                    ow::postFallbackName(post.taaFallback),
                                    ow::postFallbackName(post.bloomFallback), post.rejectedSurfaces,
                                    post.trackingOverflow);
                        ImGui::SliderFloat("EV100", &scene.output.ev100, 0, 24);
                        ImGui::SliderFloat("Compensation (stops)", &scene.output.compensation, -10, 10);
                        int mapper = int(scene.output.toneMapper);
                        if (ImGui::Combo("Tone mapper", &mapper, "ACES fitted\0AgX neutral\0"))
                            scene.output.toneMapper = ow::ToneMapper(mapper);
                    }
                    if (ImGui::CollapsingHeader("Local volumetrics")) {
                        auto &fog = scene.volumetrics;
                        ImGui::Checkbox("Enable local fog", &fog.enabled);
                        if (ImGui::Button("Mist preset"))
                            applyFogPreset(1);
                        ImGui::SameLine();
                        if (ImGui::Button("Shafts preset"))
                            applyFogPreset(2);
                        ImGui::SameLine();
                        if (ImGui::Button("Clear fog fixtures"))
                            applyFogPreset(0);
                        int quality = int(fog.quality), samples = int(fog.samples),
                            diagnostic = int(fog.diagnostic);
                        if (ImGui::Combo("Fog quality", &quality, "Normal\0High\0"))
                            fog.quality = ow::VolumetricQuality(quality);
                        ImGui::SliderFloat("Fog distance (m)", &fog.distance, 50, 4000);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Depth covered by the fog grid, not fog thickness.\n"
                                              "Shorter distances give finer depth detail.");
                        ImGui::TextUnformatted("Height fog (world-wide layer)");
                        ImGui::SliderFloat("Base density (/m)", &fog.extinction, 0, .02f, "%.5f");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Extinction at the reference altitude. Higher = thicker.\n"
                                              "0 disables the height layer. Local volumes are separate.\n"
                                              "At 0.001/m, 37%% of light remains after 1000 m at that altitude.");
                        ImGui::SliderFloat("Reference altitude (m)", &fog.height, -1000, 10000);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Altitude where Base density applies, not a top boundary.\n"
                                              "Density increases below it and decreases above it.");
                        ImGui::SliderFloat("Vertical falloff (m)", &fog.scaleHeight, 1, 500);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Rise this far and density falls to 37%%.\n"
                                              "Larger = a deeper layer; smaller = a sharper height transition.");
                        ImGui::ColorEdit3("Height-fog albedo", &fog.albedo.x);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Fraction of removed light scattered instead of absorbed.\n"
                                              "This changes brightness/colour, not extinction.");
                        ImGui::SliderFloat("Local volume density", &fog.localDensity, 0, 4, "%.2fx");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Thickness multiplier for all authored boxes/spheres, including presets.\n"
                                              "0 disables local volumes; height fog is unaffected.\n"
                                              "At 1x the preset box is 0.025/m (mist) or 0.035/m (shafts).");
                        ImGui::SliderFloat("Fog anisotropy", &fog.anisotropy, -.9f, .9f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Scattering direction for height fog and local volumes.\n"
                                              "Positive values brighten fog when looking toward the light.\n"
                                              "0 scatters equally in all directions; this is not density.");
                        if (ImGui::SliderInt("Samples per cell", &samples, 1, 2))
                            fog.samples = uint32_t(samples);
                        ImGui::Checkbox("Fog temporal history", &fog.temporal);
                        ImGui::SliderFloat("Fog history weight", &fog.historyWeight, 0, .9f);
                        if (ImGui::Combo(
                                "Fog diagnostic", &diagnostic,
                                "Shaded\0Transmission\0Scattering\0Shadow coverage\0History rejection\0"))
                            fog.diagnostic = uint32_t(diagnostic);
                        const auto &v = renderer.volumetricStats();
                        ImGui::Text("%s | %ux%ux%u | %.2f MiB", ow::volumetricFallbackName(v.fallback),
                                    v.grid.x, v.grid.y, v.grid.z, v.bytes / 1048576.);
                        ImGui::Text("Lights %u | excluded %u | overflow %u | rejected %u", v.selectedLights,
                                    v.excludedLights, v.overflowClusters, v.rejectedCells);
                        if (v.timingsAvailable)
                            ImGui::Text("Assign %.3f | inject %.3f | history %.3f | integrate %.3f | sky "
                                        "%.3f | composite %.3f ms",
                                        v.assignmentMilliseconds, v.injectionMilliseconds,
                                        v.temporalMilliseconds, v.integrationMilliseconds, v.skyMilliseconds,
                                        v.compositeMilliseconds);
                    }
                    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen)) {
                        int mode = int(scene.atmosphere.mode), quality = int(scene.atmosphere.quality);
                        if (ImGui::Combo("Sky", &mode, "Off\0Analytic\0Physical\0"))
                            scene.atmosphere.mode = ow::AtmosphereMode(mode);
                        if (ImGui::Combo("Atmosphere quality", &quality, "Normal\0High\0"))
                            scene.atmosphere.quality = ow::AtmosphereQuality(quality);
                        ImGui::Checkbox("Aerial perspective", &scene.atmosphere.aerialPerspective);
                        if (ImGui::Button("Clear air"))
                            scene.atmosphere.aerosolDensity = 1;
                        ImGui::SameLine();
                        if (ImGui::Button("Hazy air"))
                            scene.atmosphere.aerosolDensity = 4;
                        ImGui::SliderFloat("Aerosol density", &scene.atmosphere.aerosolDensity, 0, 10);
                        ImGui::SliderFloat("Boundary haze (artistic)", &scene.atmosphere.boundaryHaze, 0, 3);
                        const auto &a = renderer.atmosphereStats();
                        ImGui::Text("Atmosphere %.2f MiB | %s", a.bytes / 1048576.,
                                    ow::atmosphereFallbackName(a.fallback));
                        if (a.timingsAvailable)
                            ImGui::Text("LUT %.3f | IBL %.3f | transport %.3f ms", a.lutMilliseconds,
                                        a.environmentMilliseconds, a.compositeMilliseconds);
                        const auto &t = renderer.terrainStreamingStats();
                        ImGui::Text("Outer coverage %.1f km | horizon %u / %u",
                                    t.outerEffectiveRadius * options.terrainGrid.extent() / 1000,
                                    t.horizonReady, t.horizonRequested);
                        ImGui::Text("Distant levels %u | %.1f MiB%s", t.horizonLevels,
                                    double(t.horizonBytes) / (1024 * 1024),
                                    t.horizonLimited ? " | range limited" : "");
                    }
                    if (ImGui::CollapsingHeader("Sun & environment", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Checkbox("Sun", &scene.lighting.sunEnabled);
                        ImGui::SliderFloat("Illuminance (lux)", &scene.lighting.sunLux, 0, 200000, "%.0f");
                        ImGui::ColorEdit3("Sun colour", &scene.lighting.sunColour.x);
                        ImGui::SliderFloat3("Direction to sun", &scene.lighting.sunDirection.x, -1, 1);
                        ImGui::Checkbox("Environment", &scene.lighting.environmentEnabled);
                        if (scene.atmosphere.mode == ow::AtmosphereMode::Physical)
                            ImGui::SliderFloat("Sky lighting multiplier",
                                               &scene.atmosphere.environmentMultiplier, 0, 3);
                        else {
                            ImGui::SliderFloat("Brightness (nits)", &scene.lighting.environmentNits, 0, 20000,
                                               "%.0f");
                            ImGui::SliderAngle("Environment rotation", &scene.lighting.environmentRotation,
                                               -180, 180);
                        }
                    }
                    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                        auto &material = scene.material;
                        ImGui::ColorEdit3("Base colour (linear)", &material.baseColour.x);
                        ImGui::SliderFloat("Roughness", &material.roughness, .045f, 1);
                        ImGui::SliderFloat("Metallic", &material.metallic, 0, 1);
                        ImGui::SliderFloat("Ambient occlusion", &material.ao, 0, 1);
                        ImGui::DragFloat3("Emissive (nits)", &material.emissive.x, 100, 0, 1000000, "%.0f");
                    }
                    const auto &visibility = renderer.visibilityStats();
                    const char *modes[] = {"Direct", "Indirect", "Hi-Z"};
                    const char *fallbacks[] = {"None", "Indirect unsupported", "Hi-Z unsupported",
                                               "Candidate capacity", "Wireframe"};
                    ImGui::Text("Visibility: %s | Fallback: %s", modes[uint32_t(visibility.effective)],
                                fallbacks[uint32_t(visibility.fallback)]);
                    ImGui::Text("Candidates: %u | Batches: %u | %.2f MiB", visibility.candidates,
                                visibility.batches, visibility.allocatedBytes / 1048576.0);
                    if (visibility.countsAvailable) {
                        ImGui::Text("Completed frame %llu (%s): %u drawn | %u frustum | %u occluded",
                                    (unsigned long long)visibility.completedFrame,
                                    modes[uint32_t(visibility.completedMode)], visibility.submitted,
                                    visibility.frustumRejected, visibility.occlusionRejected);
                        ImGui::Text("Depth triangles: %llu terrain / %llu mesh",
                                    (unsigned long long)visibility.depthTriangles.terrain,
                                    (unsigned long long)visibility.depthTriangles.meshes);
                        ImGui::Text("Opaque colour triangles: %llu terrain / %llu mesh",
                                    (unsigned long long)visibility.colourTriangles.terrain,
                                    (unsigned long long)visibility.colourTriangles.meshes);
                    }
                    if (visibility.timingsAvailable)
                        ImGui::Text("Depth %.2f | Pyramid %.2f | Cull %.2f | Colour %.2f ms",
                                    visibility.depthMilliseconds, visibility.pyramidMilliseconds,
                                    visibility.cullingMilliseconds, visibility.colourMilliseconds);
                    const auto &ao = renderer.gtaoStats();
                    const char *aoModes[] = {"Off", "Half", "Full"};
                    const char *aoFallbacks[] = {"None", "Unsupported", "Budget", "Wireframe"};
                    ImGui::Text("GTAO: %s | %s | %.2f MiB", aoModes[uint32_t(ao.effective)],
                                aoFallbacks[uint32_t(ao.fallback)], ao.allocatedBytes / 1048576.0);
                    if (ao.timingsAvailable)
                        ImGui::Text("Completed %s AO: Prep %.2f | Eval %.2f | Filter %.2f ms",
                                    aoModes[uint32_t(ao.completedMode)], ao.prepareMilliseconds,
                                    ao.evaluateMilliseconds, ao.filterMilliseconds);
                    const auto &stats = renderer.renderingStats();
                    ImGui::Text("HDR: %.2f MiB | Lighting: %.2f MiB", stats.hdrBytes / 1048576.0,
                                stats.lightingBytes / 1048576.0);
                    if (stats.timingsAvailable)
                        ImGui::Text("Scene: %.2f ms | Output + UI: %.2f ms", stats.sceneMilliseconds,
                                    stats.outputMilliseconds);
                    if (stats.timingsAvailable)
                        ImGui::Text("Full GPU frame: %.2f ms", stats.frameMilliseconds);
                    ImGui::Text("Terrain CPU: select %.2f | prep %.2f | final %.2f ms",
                                stats.terrainSelectionMilliseconds, stats.terrainPreparationMilliseconds,
                                stats.terrainFinalizeMilliseconds);
                    ImGui::PopItemWidth();
                }
                ImGui::End();
                ImGui::SetNextWindowPos(ImVec2(438, 320), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(360, 330), ImGuiCond_FirstUseEver);
                if (ImGui::Begin("Terrain Materials")) {
                    auto &m = scene.terrainMaterial;
                    if (ImGui::Button("Baseline"))
                        ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::Baseline);
                    ImGui::SameLine();
                    if (ImGui::Button("Balanced"))
                        ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::Balanced);
                    ImGui::SameLine();
                    if (ImGui::Button("High"))
                        ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::High);
                    ImGui::Checkbox("Textured terrain", &m.enabled);
                    if (scene.shading == ow::ShadingMode::Legacy || scene.normalView || scene.lodColours ||
                        scene.terrainLodErrorView || scene.wireframe || scene.chunkBorders)
                        ImGui::TextWrapped("Terrain diagnostics currently override material shading.");
                    ImGui::Checkbox("Coverage weights", &m.weights);
                    ImGui::Combo("Material diagnostic", &m.diagnostic,
                                 "Shaded\0Adjusted weights\0Rock projections\0Shading "
                                 "normals\0Logical reads\0");
                    ImGui::Checkbox("Height blend", &m.heightBlend);
                    ImGui::SliderFloat("Height influence", &m.heightInfluence, 0, 1);
                    ImGui::SliderFloat("Blend width", &m.blendWidth, .01f, 1);
                    ImGui::Checkbox("Cliff projection", &m.cliffs);
                    ImGui::Checkbox("Prune weak axis (comparison)", &m.twoAxis);
                    ImGui::Checkbox("Macro variation", &m.macro);
                    ImGui::SliderFloat("Macro strength", &m.macroStrength, 0, 1);
                    ImGui::SliderFloat("Macro repeat (m)", &m.macroRepeatMetres, 16, 1024, "%.0f",
                                       ImGuiSliderFlags_Logarithmic);

                    int isolation = m.isolate + 1;
                    if (ImGui::Combo("Material", &isolation, "Blend\0Grass\0Soil\0Rock\0Gravel\0"))
                        m.isolate = isolation - 1;
                    ImGui::SliderFloat("Repeat (metres)", &m.repeatMetres, .25f, 32.f, "%.2f",
                                       ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("Normal strength", &m.normalStrength, 0, 2);
                    ImGui::SliderFloat("Roughness multiplier", &m.roughness, .1f, 2);
                    ImGui::ColorEdit3("Tint", &m.tint.x);
                    ImGui::Combo("Micro filtering", &m.filtering,
                                 "Trilinear\0Anisotropic 8x\0Anisotropic 16x\0");
                    if (ImGui::Button("Reload palette"))
                        renderer.reloadMaterials(options.materialDirectory);
                    const auto &ms = renderer.materialStats();
                    ImGui::Text("Macro: %s | Max logical reads: %u", ms.macroLoaded ? "loaded" : "neutral",
                                ms.maximumTextureReads);
                    ImGui::Text("%s | Effective AF: %.0fx", ms.bindless ? "Bindless" : "Fixed palette",
                                ms.effectiveAnisotropy);
                    ImGui::Text("Images: %.2f MiB / %.0f MiB", ms.imageBytes / 1048576.,
                                options.materialBudgetBytes / 1048576.);
                    if (!ms.error.empty())
                        ImGui::TextWrapped("%s", ms.error.c_str());
                }
                ImGui::End();
                if (regenerate) {
                    try {
                        renderer.requestTerrainBatch(ow::terrainNeighbourhood(terrain, singleChunk));
                        terrainError.clear();
                    } catch (const std::invalid_argument &error) {
                        terrainError = error.what();
                    }
                }
                if (demo)
                    ImGui::ShowDemoWindow(&demo);
                ImGui::Render();
            }
            if (demoDirty) {
                if (frames == 0)
                    demoAnchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                                glm::dvec3(camera.forward()) * 14.);
                lightingDemo.configure(lightingDemoEnabled, demoAnchor, uint32_t(demoLightCount), demoOverlap,
                                       uint32_t(demoLayers));
                demoDirty = false;
            }
            if (lightingDemoEnabled && demoAnimation) {
                demoSeconds += dt;
                lightingDemo.animate(demoSeconds);
            }
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
                    if (lightingDemoEnabled) {
                        demoAnchor = ow::translated(ow::worldPosition(camera, options.terrainGrid),
                                                    glm::dvec3(camera.forward()) * 14.);
                        demoDirty = true;
                    }
                }
            } else if (platform.window)
                SDL_Delay(16);
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

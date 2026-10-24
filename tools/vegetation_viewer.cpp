#include "renderer.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <iostream>
using namespace ow;
namespace {
struct Platform {
    SDL_Window *window = nullptr;
    bool imgui = false, backend = false;
    ~Platform() {
        if (backend)
            ImGui_ImplSDL3_Shutdown();
        if (imgui)
            ImGui::DestroyContext();
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }
};
Camera viewpoint(int view) {
    Camera c;
    c.speed = 4;
    c.renderFar = 300;
    c.local = {0, 1.6, 10};
    c.pitch = -.15f;
    if (view == 1) {
        c.local = {0, .65, 10};
        c.pitch = -.03f;
    }
    if (view == 2) {
        c.local = {0, 14, 12};
        c.pitch = -.7f;
    }
    if (view == 3) {
        c.local = {0, 1.2, 3};
        c.pitch = -.18f;
    }
    return c;
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool headless = false, fixed = false, rgba = false;
        uint32_t frames = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--headless")
                headless = true;
            else if (arg == "--fixed")
                fixed = true;
            else if (arg == "--rgba")
                rgba = true;
            else if (arg == "--frames" && i + 1 < argc)
                frames = std::stoul(argv[++i]);
            else
                throw std::invalid_argument("vegetation_viewer [--headless --frames N] [--fixed] [--rgba]");
        }
        if (headless && !frames)
            frames = 8;
        Platform platform;
        if (!headless) {
            if (!SDL_Init(SDL_INIT_VIDEO))
                throw std::runtime_error(SDL_GetError());
            platform.window = SDL_CreateWindow("OpenWorld | Vegetation V2", 1280, 800,
                                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (!platform.window)
                throw std::runtime_error(SDL_GetError());
            ImGui::CreateContext();
            platform.imgui = true;
            ImGui::StyleColorsDark();
            ImGui::GetIO().IniFilename = nullptr;
            if (!ImGui_ImplSDL3_InitForVulkan(platform.window))
                throw std::runtime_error("ImGui SDL initialization failed");
            platform.backend = true;
        }
        auto library = std::make_shared<VegetationLibrary>(
            readVegetationLibrary(OPENWORLD_VEGETATION_LIBRARY_DIR, rgba));
        auto population = std::make_shared<VegetationPopulation>(makeVegetationPopulation(64, true));
        RendererOptions o;
        o.window = platform.window;
        o.ui = !headless;
        o.width = 640;
        o.height = 360;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.forceFixedMaterials = fixed;
        o.terrainGrid = {8, .5f};
        o.terrainCapacity = 8;
        o.vegetation.library = library;
        o.vegetation.population = population;
        Renderer r(o);
        auto ground = makeVegetationFixture({}, 64).ground;
        MeshMaterial material;
        material.baseColour = {1, 1, 1, 1};
        material.doubleSided = true;
        material.colourTexture =
            r.uploadTexture(readTexture(std::filesystem::path(OPENWORLD_VEGETATION_LIBRARY_DIR) /
                                        (rgba ? "ground_rgba.dds" : "ground.dds")));
        for (auto &v : ground.vertices)
            v.uv *= .25f;
        MeshInstance instance;
        instance.mesh = r.uploadMesh(ground);
        instance.material = r.createMaterial(material);
        r.createInstance(instance);
        SceneSettings scene;
        scene.terrainVisible = false;
        scene.taa.enabled = true;
        scene.shadows.mode = ShadowMode::CSM;
        Camera camera = viewpoint(0);
        bool running = true, captured = false, paused = false;
        float direction = .42f, sunAngle = .9f;
        int view = 0, ao = 0, content = 0, lastInspection = -1;
        auto last = std::chrono::steady_clock::now();
        uint32_t frame = 0;
        while (running && (!frames || frame < frames)) {
            auto now = std::chrono::steady_clock::now();
            float dt = std::min(.05f, std::chrono::duration<float>(now - last).count());
            last = now;
            CameraInput input;
            SDL_Event event;
            while (platform.window && SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    running = false;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    r.resize();
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
                    running = false;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT &&
                    !ImGui::GetIO().WantCaptureMouse) {
                    captured = true;
                    SDL_SetWindowRelativeMouseMode(platform.window, true);
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
                    captured = false;
                    SDL_SetWindowRelativeMouseMode(platform.window, false);
                }
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    captured = false;
                    SDL_SetWindowRelativeMouseMode(platform.window, false);
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION && captured)
                    input.look += glm::vec2(event.motion.xrel, event.motion.yrel);
            }
            if (platform.window && captured) {
                const bool *keys = SDL_GetKeyboardState(nullptr);
                input.movement = {float(keys[SDL_SCANCODE_D]) - float(keys[SDL_SCANCODE_A]),
                                  float(keys[SDL_SCANCODE_E]) - float(keys[SDL_SCANCODE_Q]),
                                  float(keys[SDL_SCANCODE_W]) - float(keys[SDL_SCANCODE_S])};
                input.boost = keys[SDL_SCANCODE_LSHIFT];
            }
            camera.update(input, headless ? 1.f / 120 : dt, o.terrainGrid);
            if (!paused)
                scene.vegetation.time += headless ? 1.f / 120 : dt;
            if (platform.window) {
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                ImGui::Begin("Vegetation review");
                ImGui::TextUnformatted("Right mouse + WASD/QE: fly. Shift: faster.");
                if (ImGui::Combo("View", &view, "Close\0Grazing\0Aerial\0Asset inspection\0")) {
                    camera = viewpoint(view);
                    r.resetTemporalHistory();
                }
                if (ImGui::Combo("Content", &content, "Mixed\0Meadow\0Grass A\0Grass B\0Fern\0Shrub\0")) {
                    scene.vegetation.isolateAsset = content == 0 ? -1 : content == 1 ? -2 : content - 2;
                    r.resetTemporalHistory();
                }
                int lod = scene.vegetation.forceLod + 1;
                if (ImGui::Combo("LOD", &lod, "Automatic\0Full\0Middle\0Far\0"))
                    scene.vegetation.forceLod = lod - 1;
                ImGui::Checkbox("Vegetation", &scene.vegetation.enabled);
                ImGui::Checkbox("Pause wind", &paused);
                if (ImGui::SliderFloat("Time", &scene.vegetation.time, 0, 120))
                    r.resetTemporalHistory();
                ImGui::SliderFloat("Wind strength", &scene.vegetation.wind, 0, 2);
                ImGui::SliderAngle("Wind direction", &direction);
                ImGui::SliderFloat("Density", &scene.vegetation.density, 0, 1);
                ImGui::Checkbox("Leaf transmission", &scene.vegetation.transmission);
                ImGui::Checkbox("Vegetation depth prepass", &scene.vegetation.depthPrepass);
                ImGui::SliderAngle("Sun elevation", &sunAngle, 5, 175);
                ImGui::SliderFloat("Exposure EV100", &scene.output.ev100, 12, 17);
                ImGui::Checkbox("TAA", &scene.taa.enabled);
                ImGui::Combo("GTAO", &ao, "Off\0Half\0Full\0");
                bool shadows = scene.shadows.mode == ShadowMode::CSM;
                if (ImGui::Checkbox("Scene shadows", &shadows))
                    scene.shadows.mode = shadows ? ShadowMode::CSM : ShadowMode::Off;
                ImGui::Checkbox("Coverage", &scene.vegetation.coverage);
                if (ImGui::Button("Save view")) {
                    std::ofstream f("vegetation-view.txt");
                    f << camera.chunk.x << ' ' << camera.chunk.z << ' ' << camera.local.x << ' '
                      << camera.local.y << ' ' << camera.local.z << ' ' << camera.yaw << ' ' << camera.pitch;
                }
                ImGui::SameLine();
                if (ImGui::Button("Load view")) {
                    std::ifstream f("vegetation-view.txt");
                    Camera next = camera;
                    if (f >> next.chunk.x >> next.chunk.z >> next.local.x >> next.local.y >> next.local.z >>
                        next.yaw >> next.pitch) {
                        camera = next;
                        r.resetTemporalHistory();
                    }
                }
                const auto &stats = r.vegetationStats();
                ImGui::Text("Draw entries %u | casters %u | overflow %u", stats.visible, stats.casters,
                            stats.overflow);
                ImGui::Text("%.1f MiB | prepare %.3f ms | select %.3f ms", stats.allocatedBytes / 1048576.,
                            stats.preparationMilliseconds, stats.selectionMilliseconds);
                ImGui::End();
                ImGui::Render();
            }
            scene.vegetation.inspectIdentity = ~0u;
            if (view == 3 && content >= 2) {
                uint32_t asset = uint32_t(content - 2);
                uint32_t identity = asset < 2 ? asset + 2 : asset - 2;
                // First cell of the immutable fixture; position the inspection camera
                // around its specimen.
                scene.vegetation.inspectIdentity = identity;
                if (lastInspection != content) {
                    lastInspection = content;
                    const auto &plant = population->instances[identity];
                    camera.chunk = {};
                    camera.local = population->cells[0].origin.local + glm::dvec3(plant.rootScale) +
                                   glm::dvec3(0, .9, 2);
                    camera.pitch = -.18f;
                    camera.yaw = 0;
                }
            }
            if (view != 3)
                lastInspection = -1;
            scene.vegetation.windDirection = {std::cos(direction), std::sin(direction)};
            scene.gtao.mode = GtaoMode(ao);
            scene.lighting.sunDirection = {-std::cos(sunAngle), std::sin(sunAngle), .35f};
            r.render(camera, scene);
            ++frame;
        }
        r.waitIdle();
        if (r.validationErrors())
            throw std::runtime_error("Vegetation viewer validation failed");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

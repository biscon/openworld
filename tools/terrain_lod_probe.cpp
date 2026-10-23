// Optional default-size review/measurement, separate from the small CTest
// suite.
#include "renderer.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace ow;
int main(int argc, char **argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "lod-review";
        std::filesystem::create_directories(output);
        RendererOptions options;
        options.ui = false;
        options.requireValidation = true;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.terrainGrid = {256, .5f};
        options.terrainCapacity = 256;
        options.terrainChunkCapacity = 9;
        options.lod = {true, 32, 64};
        Renderer renderer(options);
        renderer.requestTerrainBatch(terrainNeighbourhood({}));
        Camera camera;
        camera.reset(options.terrainGrid, {}, 200);
        camera.local = {64, 411.2, 332.8};
        camera.update({}, 0, options.terrainGrid);
        SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        for (int i = 0; i < 3; ++i)
            renderer.render(camera, scene);
        const auto generation = renderer.terrainGenerationCount(), memory = renderer.terrainAllocatedBytes();
        auto report = [&](const char *name, bool save) {
            scene.lod = true;
            renderer.render(camera, scene);
            const auto stats = renderer.terrainRenderStats();
            if (save)
                renderer.capture(output / (std::string(name) + "-lod.ppm"));
            scene.lod = false;
            renderer.render(camera, scene);
            const auto full = renderer.terrainRenderStats().triangles;
            std::cout << name << ": LOD " << stats.triangles << ", full " << full << ", patches " << stats.selected
                      << ", culled " << stats.culled << ", range " << stats.firstRange << " m; levels";
            for (auto n : stats.perLevel)
                std::cout << ' ' << n;
            std::cout << '\n';
            if (save)
                renderer.capture(output / (std::string(name) + "-full.ppm"));
            scene.lod = true;
        };
        report("overview", true);
        scene.lodColours = true;
        renderer.render(camera, scene);
        renderer.capture(output / "levels.ppm");
        scene.lodColours = false;
        const auto central = renderer.readTerrainSnapshot(4), left = renderer.readTerrainSnapshot(3);
        for (int i = -2; i <= 2; ++i) {
            const auto &samples = i < 0 ? left : central;
            const uint32_t x = i < 0 ? uint32_t(256 + i * 2) : uint32_t(i * 2);
            const float height = samples[128 * 257 + x].eroded.w;
            camera.chunk = {};
            camera.local = {i, height + 1.7, 64};
            camera.yaw = .6f;
            camera.pitch = -.1f;
            camera.update({}, 0, options.terrainGrid);
            report(("ground-" + std::to_string(i)).c_str(), i == 0);
        }
        scene.wireframe = true;
        scene.lodColours = true;
        renderer.render(camera, scene);
        renderer.capture(output / "wire.ppm");
        scene.wireframe = false;
        scene.lodColours = false;
        for (int i = 0; i < 8; ++i) {
            camera.chunk = {};
            camera.local = {64 + i * 20.0, 250 + i * 40.0, 200 + i * 30.0};
            camera.pitch = -.65f;
            camera.yaw = 0;
            camera.update({}, 0, options.terrainGrid);
            report(("flight-" + std::to_string(i)).c_str(), false);
        }
        renderer.waitIdle();
        if (renderer.terrainGenerationCount() != generation || renderer.terrainAllocatedBytes() != memory ||
            renderer.validationErrors())
            throw std::runtime_error("Camera sequence changed residency/generation "
                                     "or caused Vulkan errors");
        std::cout << "Generations: " << generation << "; terrain allocation: " << memory / 1048576.0
                  << " MiB; validation errors: 0\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

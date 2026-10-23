// Offline sampling diagnostic. Binary output is native float32 TerrainSample
// rows, not a world-cache format. The numeric arguments describe its
// dimensions/domain.
#include "renderer.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int geography(const std::filesystem::path &output) {
    std::filesystem::create_directories(output);
    ow::RendererOptions options;
    options.ui = false;
    options.requireValidation = true;
    options.shaderDirectory = OPENWORLD_SHADER_DIR;
    options.terrainGrid = {512, 125}; // 64 km square; offline only.
    options.terrainCapacity = 512;
    options.normalSubdivisions = 1;
    ow::Renderer renderer(options);
    std::ofstream report(output / "summary.csv");
    report << "seed,plain_fraction,mountain_fraction,min_height,max_height,plain_"
              "mean_slope,mountain_mean_slope\n";
    for (uint32_t seed : {1u, 7u, 42u, 34434u}) {
        ow::TerrainRequest request;
        request.settings.seed = seed;
        const auto samples = renderer.sampleTerrainGeography({512, 125}, request);
        const auto prefix = output / ("seed-" + std::to_string(seed));
        std::ofstream binary(prefix.string() + ".bin", std::ios::binary);
        binary.write(reinterpret_cast<const char *>(samples.data()), samples.size() * sizeof(samples[0]));
        std::ofstream height(prefix.string() + "-height.ppm", std::ios::binary);
        std::ofstream slope(prefix.string() + "-slope.ppm", std::ios::binary);
        std::ofstream weight(prefix.string() + "-mountains.ppm", std::ios::binary);
        for (auto *file : {&height, &slope, &weight})
            *file << "P6\n513 513\n255\n";
        size_t plains = 0, mountains = 0, mountainIndex = 0;
        double plainSlope = 0, mountainSlope = 0;
        float lo = std::numeric_limits<float>::max(), hi = -lo;
        float nearest = std::numeric_limits<float>::max();
        auto pixel = [](std::ofstream &out, float value) {
            const auto v = static_cast<unsigned char>(std::clamp(value, 0.f, 1.f) * 255);
            const unsigned char bytes[3] = {v, v, v};
            out.write(reinterpret_cast<const char *>(bytes), 3);
        };
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto &v = samples[i].base;
            float g = glm::length(glm::vec2(v));
            lo = std::min(lo, v.w);
            hi = std::max(hi, v.w);
            if (v.z < 0.01f) {
                ++plains;
                plainSlope += g;
            }
            if (v.z > 0.99f) {
                ++mountains;
                mountainSlope += g;
                float distance = std::hypot(float(i % 513) - 256, float(i / 513) - 256);
                if (distance < nearest) {
                    nearest = distance;
                    mountainIndex = i;
                }
            }
            pixel(height, v.w / 1400);
            pixel(slope, std::atan(g) / 1.570796327f);
            pixel(weight, v.z);
        }
        report << seed << ',' << double(plains) / samples.size() << ',' << double(mountains) / samples.size()
               << ',' << lo << ',' << hi << ',' << plainSlope / std::max(size_t(1), plains) << ','
               << mountainSlope / std::max(size_t(1), mountains) << '\n';
        // A 16 km view around a deterministic mountain region, with a low camera
        // aimed into the patch. Reuse the same production renderer for both
        // variants.
        const ow::TerrainGrid viewGrid{512, 32};
        request.chunk = {int64_t((mountainIndex % 513) * 125 / 16384),
                         int64_t((mountainIndex / 513) * 125 / 16384)};
        renderer.setTerrainGrid(viewGrid);
        renderer.requestTerrain(request);
        ow::Camera camera;
        camera.reset(viewGrid, request.chunk, 0);
        camera.local = {8192, 1800, 16000};
        camera.pitch = -0.10f;
        for (bool eroded : {false, true}) {
            ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
            scene.gtao.mode = ow::GtaoMode::Off;
            scene.eroded = eroded;
            renderer.render(camera, scene);
            renderer.capture(prefix.string() + (eroded ? "-eroded.ppm" : "-base.ppm"));
        }
        const auto mesh = renderer.readTerrainSnapshot();
        const auto &eye = mesh[500 * 513 + 256];
        camera.local.y = std::max(eye.base.w, eye.eroded.w) + 3;
        camera.pitch = 0.02f;
        for (bool eroded : {false, true}) {
            ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
            scene.gtao.mode = ow::GtaoMode::Off;
            scene.eroded = eroded;
            renderer.render(camera, scene);
            renderer.capture(prefix.string() + (eroded ? "-ground-eroded.ppm" : "-ground-base.ppm"));
        }
        // Find a fully plain 2 km patch, then retain its exact world coordinate
        // and samples for detrended relief measurements and ground-level review.
        bool foundPlain = false;
        for (int z = 0; z < 32 && !foundPlain; ++z)
            for (int x = 0; x < 32 && !foundPlain; ++x) {
                request.chunk = {x, z};
                const auto coarse = renderer.sampleTerrainGeography({16, 128}, request);
                foundPlain =
                    std::all_of(coarse.begin(), coarse.end(), [](const auto &s) { return s.base.z == 0; });
            }
        if (!foundPlain)
            throw std::runtime_error("No plains review patch found");
        const ow::TerrainGrid plainGrid{256, 8};
        const auto plain = renderer.sampleTerrainGeography(plainGrid, request);
        if (!std::all_of(plain.begin(), plain.end(), [](const auto &s) { return s.base.z == 0; }))
            throw std::runtime_error("Plains review patch contains a transition");
        std::ofstream plainFile(prefix.string() + "-plains.bin", std::ios::binary);
        plainFile.write(reinterpret_cast<const char *>(plain.data()), plain.size() * sizeof(plain[0]));
        std::ofstream coordinates(prefix.string() + "-plains.txt");
        coordinates << request.chunk.x * 2048 << ' ' << request.chunk.z * 2048 << " 2048 8\n";
        renderer.setTerrainGrid(plainGrid);
        renderer.requestTerrain(request);
        camera.reset(plainGrid, request.chunk, plain[128 * 257 + 128].base.w);
        ow::SceneSettings plainScene;
    plainScene.shadows.mode = ow::ShadowMode::Off;
        plainScene.gtao.mode = ow::GtaoMode::Off;
        plainScene.eroded = false;
        renderer.render(camera, plainScene);
        renderer.capture(prefix.string() + "-plains-elevated.ppm");
        camera.local = {1024, plain[240 * 257 + 128].base.w + 3, 1920};
        camera.pitch = -.01f;
        renderer.render(camera, plainScene);
        renderer.capture(prefix.string() + "-plains-ground.ppm");
        if (!plainFile || !coordinates)
            throw std::runtime_error("Cannot write plains review output");
        if (!binary || !height || !slope || !weight || !report)
            throw std::runtime_error("Cannot write geography diagnostic output");
    }
    std::cout << "Geography maps: " << output << "; validation errors: " << renderer.validationErrors()
              << '\n';
    return renderer.validationErrors() ? 1 : 0;
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--geography")
            return geography(argv[2]);
        int positional = argc;
        for (int i = 8; i < argc; ++i)
            if (std::string(argv[i]).rfind("--", 0) == 0) {
                positional = i;
                break;
            }
        if (positional < 8 || positional > 11) {
            std::cerr << "Usage: terrain_probe --geography OUTPUT_DIRECTORY\n       "
                         "terrain_probe OUTPUT CELLS SPACING CHUNK_X CHUNK_Z ROUNDING SEED "
                         "[NORMAL_SUBDIVISIONS [capture [REFERENCE_HEIGHT]]] [--erosion-wavelength M] "
                         "[--erosion-strength S] [--ridge-strength 0..1] [--base-detail 0..1] [--lod 0|1]\n";
            return 1;
        }
        auto number = [&](int i) {
            size_t end = 0;
            const double value = std::stod(argv[i], &end);
            if (end != std::string(argv[i]).size() || !std::isfinite(value))
                throw std::invalid_argument("Invalid numeric probe argument");
            return value;
        };
        const double cells = number(2), seed = number(7), x = number(4), z = number(5);
        if (cells < 8 || cells > 1024 || std::floor(cells) != cells || seed < 0 || seed > UINT32_MAX ||
            std::floor(seed) != seed || std::abs(x) > 1e9 || std::abs(z) > 1e9 || std::floor(x) != x ||
            std::floor(z) != z)
            throw std::invalid_argument("Probe integer argument out of range");
        ow::RendererOptions options;
        options.ui = false;
        options.requireValidation = true;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.terrainGrid = {uint32_t(cells), float(number(3))};
        options.terrainCapacity = uint32_t(cells);
        const double subdivisions = positional >= 9 ? number(8) : 1;
        if (subdivisions != 1 && subdivisions != 2)
            throw std::invalid_argument("Subdivisions must be 1 or 2");
        options.normalSubdivisions = uint32_t(subdivisions);
        ow::TerrainRequest request;
        request.chunk = {int64_t(x), int64_t(z)};
        request.settings.creaseRounding = float(number(6));
        request.settings.seed = uint32_t(seed);
        for (int i = positional; i < argc; i += 2) {
            if (i + 1 >= argc)
                throw std::invalid_argument("Missing probe option value");
            const std::string option = argv[i];
            if (option == "--erosion-wavelength")
                request.settings.erosionWavelength = float(number(i + 1));
            else if (option == "--erosion-strength")
                request.settings.erosionStrength = float(number(i + 1));
            else if (option == "--ridge-strength")
                request.settings.ridgeStrength = float(number(i + 1));
            else if (option == "--base-detail")
                request.settings.baseDetail = float(number(i + 1));
            else if (option == "--lod") {
                const double enabled = number(i + 1);
                if (enabled != 0 && enabled != 1)
                    throw std::invalid_argument("LOD must be 0 or 1");
                options.lod = {enabled == 1, std::min(32u, options.terrainGrid.cells), 64};
            } else
                throw std::invalid_argument("Unknown probe option: " + option);
        }
        ow::Renderer renderer(options);
        renderer.requestTerrain(request);
        ow::Camera camera;
        camera.reset(options.terrainGrid, request.chunk);
        ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        renderer.render(camera, scene);
        auto samples = renderer.readTerrainSnapshot();
        std::ofstream out(argv[1], std::ios::binary);
        out.write(reinterpret_cast<const char *>(samples.data()),
                  std::streamsize(samples.size() * sizeof(ow::TerrainSample)));
        if (!out)
            throw std::runtime_error("Cannot write probe samples");
        if (positional >= 10) {
            if (std::string(argv[9]) != "capture")
                throw std::invalid_argument("Expected capture flag");
            const auto n = options.terrainGrid.cells;
            // A reproducible elevated close-up, relative to the patch's own surface.
            const double centre =
                positional == 11 ? number(10) : samples[size_t(n / 2) * (n + 1) + n / 2].eroded.w;
            camera.reset(options.terrainGrid, request.chunk, float(centre));
            const double extent = options.terrainGrid.extent();
            camera.local = {extent * 0.5, centre + extent * 0.9, extent * 1.2};
            camera.pitch = -std::atan2(0.9f, 0.7f);
            for (int mode = 0; mode < 6; ++mode) {
                if (mode == 4) {
                    const auto &eye = samples[size_t(n * 7 / 8) * (n + 1) + n / 2];
                    camera.local = {extent * .5, std::max(eye.base.w, eye.eroded.w) + 3,
                                    double(n * 7 / 8) * options.terrainGrid.spacing};
                    camera.pitch = 0;
                }
                scene.eroded = mode != 3 && mode != 5;
                scene.normalView = mode == 1;
                scene.wireframe = mode == 2;
                renderer.render(camera, scene);
                renderer.capture(std::string(argv[1]) + "." + std::to_string(mode) + ".ppm");
            }
        }
        renderer.waitIdle();
        std::cout << "Samples: " << samples.size() << "; validation errors: " << renderer.validationErrors()
                  << '\n';
        return renderer.validationErrors() ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

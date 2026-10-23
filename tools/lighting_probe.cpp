#include "lighting_demo.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace ow;
namespace {
double percentile(std::vector<double> &v, double p) {
    std::sort(v.begin(), v.end());
    double at = (v.size() - 1) * p;
    auto lo = size_t(at), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (at - lo);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument("Usage: lighting_probe OUTPUT_DIR [--quick] [--frames N] [--lights "
                                        "N] [--fixed] [--require-validation]");
        const std::filesystem::path output = argv[1];
        std::filesystem::create_directories(output);
        bool quick = false, fixed = false, validation = false;
        uint32_t frames = 120, onlyLights = ~0u;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--quick")
                quick = true;
            else if (arg == "--fixed")
                fixed = true;
            else if (arg == "--require-validation")
                validation = true;
            else if ((arg == "--frames" || arg == "--lights") && i + 1 < argc) {
                auto n = std::stoul(argv[++i]);
                if (arg == "--frames")
                    frames = uint32_t(n);
                else
                    onlyLights = uint32_t(n);
            } else
                throw std::invalid_argument("Unknown probe argument");
        }
        if (!frames || frames > 10000 || (onlyLights != ~0u && onlyLights > 4096))
            throw std::invalid_argument("Invalid probe sample/light count");
        if (quick && frames == 120)
            frames = 6;
        RendererOptions o;
        o.ui = false;
        o.validation = validation;
        o.requireValidation = validation;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.forceFixedMaterials = fixed;
        o.terrainGrid = {32, 2};
        o.terrainCapacity = 32;
        o.width = quick ? 320 : 1920;
        o.height = quick ? 180 : 1080;
        Renderer r(o);
        LightingDemo demo(r);
        SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        scene.terrainMaterial.enabled = false;
        scene.lighting.sunLux = 1000;
        scene.lighting.environmentNits = 30;
        scene.output.ev100 = 9;
        TerrainRequest terrain;
        terrain.settings.heightScale = 0;
        r.requestTerrain(terrain);
        Camera camera;
        camera.local = {32, 5, 48};
        camera.pitch = -.08f;
        const WorldPosition anchor{0, 0, {32, 3, 32}};
        std::ofstream meta(output / "metadata.txt");
        meta << "device=" << r.deviceName() << "\nvalidation=" << r.validationEnabled()
             << "\nbindless=" << r.materialStats().bindless << "\nquick=" << quick << "\nsamples=" << frames
             << "\n";
        std::ofstream csv(output / "timings.csv");
        csv << "width,height,lights,overlap,layers,path,cpu_median_ms,cpu_p95_ms,scene_median_ms,scene_p95_"
               "ms,assign_median_ms,assign_p95_ms,opaque_median_ms,opaque_p95_ms,blend_median_ms,blend_p95_"
               "ms,occupied,mean_occupancy,max_occupancy,overflow,cluster_bytes,geometry_bytes,timestamps\n";
        csv << std::setprecision(8);
        std::vector<uint32_t> counts =
            quick ? std::vector<uint32_t>{0, 32, 256} : std::vector<uint32_t>{0, 32, 256, 1024, 4096};
        if (onlyLights != ~0u)
            counts = {onlyLights};
        const std::vector<glm::uvec2> extents =
            quick ? std::vector<glm::uvec2>{{320, 180}} : std::vector<glm::uvec2>{{1920, 1080}, {3840, 2160}};
        for (auto extent : extents) {
            r.resizeOffscreen(extent.x, extent.y);
            for (auto count : counts)
                for (bool overlap : {false, true})
                    for (uint32_t layers : {0u, 3u, 8u}) {
                        demo.configure(true, anchor, count, overlap, layers);
                        for (auto mode : {LocalLightMode::BruteForce, LocalLightMode::Clustered}) {
                            scene.localLights = mode;
                            for (int i = 0; i < (quick ? 4 : 16); ++i)
                                r.render(camera, scene);
                            std::vector<double> cpu, sceneTimes, assignment, opaque, blend;
                            for (auto *v : {&cpu, &sceneTimes, &assignment, &opaque, &blend})
                                v->reserve(frames);
                            for (uint32_t i = 0; i < frames; ++i) {
                                auto start = std::chrono::steady_clock::now();
                                r.render(camera, scene);
                                cpu.push_back(std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - start)
                                                  .count());
                                const auto &s = r.sceneStats();
                                sceneTimes.push_back(r.renderingStats().sceneMilliseconds);
                                assignment.push_back(s.assignmentMilliseconds);
                                opaque.push_back(s.opaqueMilliseconds);
                                blend.push_back(s.transparentMilliseconds);
                            }
                            const auto &s = r.sceneStats();
                            csv << extent.x << ',' << extent.y << ',' << count << ',' << overlap << ','
                                << layers << ','
                                << (mode == LocalLightMode::Clustered ? "clustered" : "brute");
                            for (auto *v : {&cpu, &sceneTimes, &assignment, &opaque, &blend})
                                csv << ',' << percentile(*v, .5) << ',' << percentile(*v, .95);
                            csv << ',' << s.occupiedClusters << ',' << s.meanOccupancy << ','
                                << s.maximumOccupancy << ',' << s.overflowClusters << ',' << s.clusterBytes
                                << ',' << s.geometryBytes << ',' << s.timingsAvailable << '\n';
                            csv.flush();
                            if (mode == LocalLightMode::Clustered && layers == 3)
                                r.capture(output / (std::to_string(extent.x) + "_" + std::to_string(count) +
                                                    (overlap ? "_overlap.ppm" : "_sparse.ppm")));
                            std::cout << extent.x << 'x' << extent.y << " lights=" << count
                                      << " overlap=" << overlap << " layers=" << layers
                                      << " path=" << uint32_t(mode) << " scene=" << percentile(sceneTimes, .5)
                                      << " ms\n";
                        }
                    }
        }
        r.waitIdle();
        if (r.validationErrors())
            throw std::runtime_error("Probe reported Vulkan validation errors");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

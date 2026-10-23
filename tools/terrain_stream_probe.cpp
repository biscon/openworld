#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using Clock = std::chrono::steady_clock;
void run(const std::filesystem::path &output, uint32_t cells, int radius, int lap) {
    ow::RendererOptions options;
    options.ui = false;
    options.requireValidation = true;
    options.shaderDirectory = OPENWORLD_SHADER_DIR;
    options.terrainGrid = {cells, .5f};
    options.terrainCapacity = cells;
    options.lod = {true, std::min(32u, cells), 64};
    options.streaming.enabled = true;
    options.streaming.cacheDirectory = output / "cache";
    ow::Renderer r(options);
    ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.viewRadius = radius;
    ow::Camera camera;
    camera.reset(options.terrainGrid);
    camera.local = {options.terrainGrid.extent() * .5, 450, options.terrainGrid.extent() * .5};
    const auto allocation = r.terrainAllocatedBytes();
    std::vector<double> times;
    times.reserve(50000);
    uint64_t transfers = 0;
    const auto start = Clock::now();
    const auto frame = [&] {
        const auto t = Clock::now();
        r.render(camera, scene);
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t).count());
        transfers += r.terrainStreamingStats().transferBytes;
        if (times.back() > 100)
            std::cout << "Slow frame " << times.size() << ": " << times.back()
                      << " ms, generations=" << r.terrainGenerationCount()
                      << ", dispatches=" << r.terrainStreamingStats().dispatches
                      << ", triangles=" << r.terrainRenderStats().triangles << '\n';
        if (r.terrainAllocatedBytes() != allocation || r.validationErrors())
            throw std::runtime_error("Streaming allocation/validation regression");
    };
    const auto settle = [&] {
        int stable = 0;
        for (int i = 0; i < 20000 && stable < 80; ++i) {
            frame();
            const auto &s = r.terrainStreamingStats();
            stable = (s.pending == 0 && s.detailPending == 0 && s.ioJobs == 0) ? stable + 1 : 0;
        }
        if (stable < 80)
            throw std::runtime_error("Probe convergence deadline");
    };
    settle();
    const auto startup = r.terrainStreamingStats();
    std::cout << (lap ? "warm" : "cold") << " initial: " << startup.coverage << " coverage, " << startup.detail
              << " detail, " << startup.generations << " generations, " << startup.hits << " hits\n";
    if (lap && startup.generations)
        throw std::runtime_error("Warm-cache start regenerated terrain");
    r.capture(output / (lap ? "warm.ppm" : "cold.ppm"));
    // Cross boundaries in both directions, turn, change radius, then teleport with work pending.
    for (int i = 0; i < 180; ++i) {
        camera.local.x += options.terrainGrid.extent() * .025 * (i < 90 ? 1 : -1);
        camera.update({}, 0, options.terrainGrid);
        camera.yaw += .008f;
        scene.viewRadius = i < 60 ? radius : i < 100 ? 0 : 8;
        frame();
    }
    camera.chunk = {-1000000, 1000000};
    for (int i = 0; i < 12; ++i)
        frame();
    camera.chunk = {};
    scene.viewRadius = radius;
    settle();
    r.capture(output / (lap ? "warm-return.ppm" : "cold-return.ppm"));
    const auto &s = r.terrainStreamingStats();
    std::sort(times.begin(), times.end());
    const auto percentile = [&](double p) { return times[size_t(p * (times.size() - 1))]; };
    std::cout << (lap ? "warm" : "cold") << ": " << times.size() << " frames, "
              << std::chrono::duration<double>(Clock::now() - start).count() << " s; ms p50=" << percentile(.5)
              << " p95=" << percentile(.95) << " p99=" << percentile(.99) << " max=" << times.back()
              << "; GPU=" << s.gpuBytes / double(ow::terrainMiB) << " MiB host=" << s.hostBytes / double(ow::terrainMiB)
              << " MiB; transfer=" << transfers / double(ow::terrainMiB) << " MiB; generations=" << s.generations
              << " hits=" << s.hits << " errors=" << s.errors << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "/tmp/openworld-stream-review";
        const auto cells = argc > 2 ? uint32_t(std::stoul(argv[2])) : 256;
        const int radius = argc > 3 ? std::stoi(argv[3]) : 2;
        if (radius < 0 || radius > 8)
            throw std::invalid_argument("Probe radius must be 0..8");
        std::filesystem::create_directories(output);
        run(output, cells, radius, 0);
        run(output, cells, radius, 1);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

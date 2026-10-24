#include "renderer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <numeric>

// Count render-thread C++ allocation calls in the render() interval. This is not a driver
// malloc profiler.
namespace {
std::atomic<uint64_t> allocationCalls{0};
thread_local uint64_t threadAllocationCalls = 0;
} // namespace
void *operator new(std::size_t n) {
    allocationCalls.fetch_add(1, std::memory_order_relaxed);
    ++threadAllocationCalls;

    if (auto p = std::malloc(std::max(n, size_t(1))))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t n, std::align_val_t a) {
    allocationCalls.fetch_add(1, std::memory_order_relaxed);
    ++threadAllocationCalls;
    void *p = nullptr;
    if (posix_memalign(&p, std::max(size_t(a), sizeof(void *)), std::max(n, size_t(1))) == 0)
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }
using namespace ow;
using Clock = std::chrono::steady_clock;
namespace {
double percentile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[size_t(q * (v.size() - 1))];
}
struct Samples {
    std::vector<double> cpu, gpu, prepare, update;
    uint64_t bytes = 0, allocations = 0;
    uint32_t visible = 0, pending = 0, updates = 0, generated = 0;
    Samples() {
        cpu.reserve(600);
        gpu.reserve(600);
        prepare.reserve(600);
        update.reserve(600);
    }
    void add(Renderer &r, double ms, uint64_t calls) {
        const auto &v = r.vegetationStats();
        allocations += calls;
        cpu.push_back(ms);
        gpu.push_back(r.renderingStats().frameMilliseconds);
        prepare.push_back(v.preparationMilliseconds);
        update.push_back(v.updateMilliseconds);
        bytes = v.allocatedBytes;
        visible = v.visible;
        pending = v.pendingCells;
        updates += v.updatedCells;
        generated += v.generatedCells;
        if (v.overflow)
            throw std::runtime_error("Vegetation list overflow invalidates measurement");
    }
    void print(const char *name) const {
        std::cout << name << ": CPU median/p95 " << percentile(cpu, .5) << " / " << percentile(cpu, .95)
                  << " ms; GPU " << percentile(gpu, .5) << " / " << percentile(gpu, .95)
                  << " ms; vegetation CPU " << percentile(prepare, .5) << " / " << percentile(prepare, .95)
                  << " ms; update GPU p95/max " << percentile(update, .95) << " / " << percentile(update, 1)
                  << " ms; " << double(bytes) / (1024 * 1024) << " MiB; " << visible << " visible entries; "
                  << updates << " updates, " << generated << " generated, " << pending << " pending; "
                  << allocations << " render-thread C++ allocation calls\n";
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 2 && (argc != 3 || std::string(argv[2]) != "--cpu-check"))
            throw std::invalid_argument(
                "Usage: vegetation_stream_probe OUTPUT_DIRECTORY (one short pair/route)");
        std::filesystem::path output = argv[1];
        std::filesystem::create_directories(output);
        RendererOptions o;
        o.ui = false;
        o.validation = false;
        o.width = 1920;
        o.height = 1080;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.terrainGrid = {256, .5f};
        o.terrainCapacity = 256;
        o.lod = {true, 32, 64};
        o.streaming.enabled = o.streaming.hierarchical = true;
        o.streaming.maximumDistance = 512;
        o.streaming.shadowCasterBytes = 0;
        o.streaming.cacheDirectory = output / "terrain-cache";
        o.vegetation.library =
            std::make_shared<VegetationLibrary>(readVegetationLibrary(OPENWORLD_VEGETATION_LIBRARY_DIR));
        o.vegetation.streaming.enabled = true;
        o.shadowCapacity.resolution = 1024;
        Renderer r(o);
        TerrainRequest terrain;
        terrain.settings.heightScale = 12;
        terrain.settings.seed = 173;
        r.requestTerrain(terrain);
        Camera camera;
        camera.local = {32, 8, 32};
        camera.pitch = -.08f;
        SceneSettings scene;
        scene.viewRadius = 1;
        scene.taa.enabled = true;
        scene.opaqueDepthPrepass = true;
        scene.shadows.mode = ShadowMode::CSM;
        auto start = Clock::now();
        const auto deadline = start + std::chrono::seconds(75);
        uint64_t frameAllocations = 0;
        auto frame = [&]() {
            if (Clock::now() > deadline)
                throw std::runtime_error("75-second probe cap; unfinished gates remain pending");
            scene.vegetation.time += 1.f / 120;
            auto begin = Clock::now();
            auto calls = threadAllocationCalls;
            r.render(camera, scene);
            frameAllocations = threadAllocationCalls - calls;
            return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        };
        Samples cold;
        uint32_t quiet = 0;
        for (uint32_t i = 0; i < 2400; ++i) {
            double ms = frame();
            if (i < 600)
                cold.add(r, ms, frameAllocations);
            const auto &v = r.vegetationStats();
            const auto &t = r.terrainStreamingStats();
            quiet = v.residentCells > 0 && v.pendingCells == 0 && v.updatedCells == 0 && t.pending == 0 &&
                            t.detailPending == 0
                        ? quiet + 1
                        : 0;
            if (quiet >= 8)
                break;
        }
        if (quiet < 8)
            throw std::runtime_error("Cold streaming did not settle within bounded frame count");
        auto heights = r.readStreamedTileHeights({0, 0, 0});
        camera.local.y = heights[64 * 257 + 64].y + 1.7;
        cold.print("cold");
        if (argc == 3) {
            Samples check;
            for (uint32_t i = 0; i < 240; ++i) {
                double ms = frame();
                if (i >= 120)
                    check.add(r, ms, frameAllocations);
            }
            check.print("focused steady-state CPU check");
            return 0;
        }
        std::array<Samples, 2> pair;
        for (uint32_t enabled = 0; enabled < 2; ++enabled) {
            scene.vegetation.enabled = enabled != 0;
            // Re-populate after disabling, outside the fixed timed sample window.
            if (enabled) {
                quiet = 0;
                for (uint32_t i = 0; i < 1600; ++i) {
                    frame();
                    const auto &v = r.vegetationStats();
                    quiet = v.residentCells && v.pendingCells == 0 && v.updatedCells == 0 ? quiet + 1 : 0;
                    if (quiet >= 4)
                        break;
                }
            }
            for (uint32_t i = 0; i < 720; ++i) {
                double ms = frame();
                if (i >= 120)
                    pair[enabled].add(r, ms, frameAllocations);
            }
            pair[enabled].print(enabled ? "on" : "off");
        }
        std::cout << "Incremental GPU median: " << percentile(pair[1].gpu, .5) - percentile(pair[0].gpu, .5)
                  << " ms\n";
        r.capture(output / "close.ppm");
        Samples route;
        for (uint32_t i = 0; i < 360; ++i) {
            if (i < 180)
                camera.local.x += .15;
            if (i == 180)
                camera.chunk = {8, -8};
            if (i == 188)
                camera.chunk = {0, 0};
            double ms = frame();
            route.add(r, ms, frameAllocations);
        }
        route.print("warm traversal / teleport / return");
        scene.taa.enabled = false;
        for (int i = 0; i < 8; ++i)
            frame();
        r.capture(output / "taa-off.ppm");
        std::cout << "Total seconds: " << std::chrono::duration<double>(Clock::now() - start).count() << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

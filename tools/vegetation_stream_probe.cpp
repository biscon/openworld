#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "review_view.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <numeric>

// Count render-thread C++ allocation calls in the render() interval. This is
// not a driver malloc profiler.
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
    std::vector<double> cpu, gpu, prepare, update, selection, depth, colour, shadows;
    uint64_t bytes = 0, allocations = 0;
    uint32_t visible = 0, pending = 0, updates = 0, generated = 0;
    Samples() {
        cpu.reserve(600);
        gpu.reserve(600);
        prepare.reserve(600);
        update.reserve(600);
        selection.reserve(600);
        depth.reserve(600);
        colour.reserve(600);
        shadows.reserve(600);
    }
    void add(Renderer &r, double ms, uint64_t calls) {
        const auto &v = r.vegetationStats();
        allocations += calls;
        cpu.push_back(ms);
        gpu.push_back(r.renderingStats().frameMilliseconds);
        prepare.push_back(v.preparationMilliseconds);
        update.push_back(v.updateMilliseconds);
        selection.push_back(v.selectionMilliseconds);
        depth.push_back(v.depthMilliseconds);
        colour.push_back(v.colourMilliseconds);
        shadows.push_back(v.shadowMilliseconds);
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
        std::cout << "  vegetation GPU medians: select " << percentile(selection, .5) << ", depth "
                  << percentile(depth, .5) << ", colour " << percentile(colour, .5) << ", shadows "
                  << percentile(shadows, .5) << " ms\n";
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument(
                "Usage: vegetation_stream_probe OUTPUT [--cpu-check] "
                "[--engine-scene] [--distance 20..200] [--occlusion-check] [--route-check] [--taa-check]");
        bool cpuOnly = false, routeOnly = false, engineScene = false, occlusionCheck = false,
             taaCheck = false;
        float distance = 70;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--cpu-check")
                cpuOnly = true;
            else if (arg == "--taa-check")
                taaCheck = true;
            else if (arg == "--route-check")
                routeOnly = true;
            else if (arg == "--engine-scene") {
                engineScene = true;
                distance = 80;
            } else if (arg == "--occlusion-check")
                occlusionCheck = true;
            else if (arg == "--distance" && i + 1 < argc)
                distance = std::stof(argv[++i]);
            else
                throw std::invalid_argument("Unknown probe option: " + arg);
        }
        if (!std::isfinite(distance) || distance < 20 || distance > 200)
            throw std::invalid_argument("Distance must be 20..200 m");
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
        if (engineScene || distance > 80) {
            o.vegetation.streaming.cellCapacity = 12288;
            o.vegetation.streaming.demandRadius = 216;
            o.vegetation.streaming.retentionRadius = 232;
            o.vegetation.budgetBytes = 256ull << 20;
        }
        if (engineScene) {
            o.streaming.maximumDistance = 16384;
            o.streaming.horizonBytes = 128ull << 20;
            o.streaming.shadowCasterBytes = 64ull << 20;
            o.curvedHorizon = true;
            o.materialDirectory = OPENWORLD_MATERIAL_DIR;
            o.shadowCapacity.resolution = ShadowCapacity{}.resolution;
        }
        Renderer r(o);
        TerrainRequest terrain;
        if (!engineScene)
            terrain.settings.heightScale = 12;
        terrain.settings.seed = 173;
        r.requestTerrain(terrain);
        Camera camera;
        camera.local = {32, 8, 32};
        camera.pitch = -.08f;
        SceneSettings scene;
        scene.viewRadius = engineScene ? 64 : 3;
        scene.vegetation.distance = distance;
        if (engineScene) {
            scene.atmosphere.mode = AtmosphereMode::Physical;
            scene.bloom.intensity = .05f;
        }
        scene.taa.enabled = true;
        scene.opaqueDepthPrepass = true;
        scene.shadows.mode = ShadowMode::CSM;
        auto start = Clock::now();
        const auto deadline = start + std::chrono::seconds(engineScene ? 180 : 90);
        uint64_t frameAllocations = 0;
        auto frame = [&]() {
            if (Clock::now() > deadline)
                throw std::runtime_error("Probe time cap reached; unfinished gates remain pending");
            scene.vegetation.time += 1.f / 120;
            auto begin = Clock::now();
            auto calls = threadAllocationCalls;
            r.render(camera, scene);
            frameAllocations = threadAllocationCalls - calls;
            return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        };
        Samples cold;
        std::vector<glm::vec2> heights;
        uint32_t quiet = 0;
        for (uint32_t i = 0; i < (engineScene ? 12000u : 2400u); ++i) {
            double ms = frame();
            if (i < 600)
                cold.add(r, ms, frameAllocations);
            const auto &v = r.vegetationStats();
            const auto &t = r.terrainStreamingStats();
            if (heights.empty() && i % 60 == 0) {
                for (const auto &tile : r.readTerrainResidency())
                    if (tile.key == TerrainTileKey{0, 0, 0} && tile.resident) {
                        heights = r.readStreamedTileHeights({0, 0, 0});
                        camera.local.y = heights[64 * 257 + 64].y + 1.7;
                        break;
                    }
            }
            if (i && i % 600 == 0)
                std::cout << "Settling: " << i << " frames; vegetation ready/pending/missing "
                          << v.residentCells << '/' << v.pendingCells << '/' << v.missingTerrainCells
                          << "; terrain " << t.pending << " + detail " << t.detailPending << std::endl;
            quiet = v.residentCells > 0 && v.pendingCells == 0 && v.updatedCells == 0 && t.pending == 0 &&
                            t.detailPending == 0
                        ? quiet + 1
                        : 0;
            if (quiet >= 8)
                break;
        }
        if (quiet < 8)
            throw std::runtime_error("Cold streaming did not settle within bounded frame count");
        if (heights.empty())
            heights = r.readStreamedTileHeights({0, 0, 0});
        camera.local.y = heights[64 * 257 + 64].y + 1.7;
        ReviewView bookmark;
        bookmark.camera = camera;
        bookmark.terrain = terrain.settings;
        bookmark.scene = scene;
        bookmark.startup = o;
        saveReviewView(output / "close-view.json", bookmark);
        if (taaCheck) {
            scene.vegetation.wind = 0;
            for (bool enabled : {false, true}) {
                scene.taa.thinGeometry = enabled;
                r.resetTemporalHistory();
                std::vector<double> timings;
                timings.reserve(120);
                uint64_t allocations = 0;
                for (uint32_t i = 0; i < 152; ++i) {
                    frame();
                    if (i >= 32) {
                        timings.push_back(r.postStats().taaMilliseconds);
                        allocations += frameAllocations;
                    }
                }
                std::cout << "Thin TAA " << enabled << ": median/p95 " << percentile(timings, .5) << " / "
                          << percentile(timings, .95) << " ms; coverage "
                          << r.postStats().thinGeometryBytes / 1048576. << " MiB; allocations " << allocations
                          << std::endl;
                // Blocking observations are outside the measured interval. Capture
                // successive jitter phases, not just one flattering still image.
                for (uint32_t i = 0; i < 8; ++i) {
                    frame();
                    r.capture(output /
                              (std::string(enabled ? "thin-on-" : "thin-off-") + std::to_string(i) + ".ppm"));
                }
            }
            scene.vegetation.wind = 1;
            for (uint32_t i = 0; i < 32; ++i) {
                camera.local.x += .02;
                frame();
            }
            r.capture(output / "thin-wind-moving.ppm");
            return 0;
        }
        cold.print("cold");
        if (cpuOnly) {
            Samples check;
            for (uint32_t i = 0; i < 240; ++i) {
                double ms = frame();
                if (i >= 120)
                    check.add(r, ms, frameAllocations);
            }
            check.print("focused steady-state CPU check");
            return 0;
        }
        if (!routeOnly) {
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
            std::cout << "Incremental GPU median: "
                      << percentile(pair[1].gpu, .5) - percentile(pair[0].gpu, .5) << " ms\n";
            r.capture(output / "close.ppm");
            if (occlusionCheck) {
                MeshMaterial material;
                material.doubleSided = true;
                MeshInstance wall;
                wall.mesh = r.uploadMesh(makeQuad());
                wall.material = r.createMaterial(material);
                wall.position = translated(worldPosition(camera, o.terrainGrid), {0, 0, -12});
                wall.scale = {24, 16, 1};
                auto handle = r.createInstance(wall);
                for (bool enabled : {false, true}) {
                    scene.vegetation.occlusion = enabled;
                    Samples occlusion;
                    for (uint32_t i = 0; i < 720; ++i) {
                        double ms = frame();
                        if (i >= 120)
                            occlusion.add(r, ms, frameAllocations);
                    }
                    occlusion.print(enabled ? "occlusion on" : "occlusion off");
                }
                r.removeInstance(handle);
                scene.vegetation.occlusion = false;
            }
        }
        Samples route;
        for (uint32_t i = 0; i < 360; ++i) {
            if (i < 180) {
                camera.local.x += .15;
                const auto ix = uint32_t(camera.local.x / .5);
                const auto iz = uint32_t(camera.local.z / .5);
                camera.local.y = heights[iz * 257 + ix].y + 1.7;
            }
            if (i == 180)
                camera.chunk = {8, -8};
            if (i == 188)
                camera.chunk = {0, 0};
            double ms = frame();
            route.add(r, ms, frameAllocations);
        }
        route.print("warm traversal / teleport / return");
        // At 200 m the bounded 32-cell update queue cannot refill in the route's
        // 172 post-return frames. Measure and verify recovery independently.
        Samples recovery;
        quiet = 0;
        for (uint32_t i = 0; i < 1600 && quiet < 8; ++i) {
            double ms = frame();
            recovery.add(r, ms, frameAllocations);
            const auto &v = r.vegetationStats();
            quiet = v.residentCells && v.pendingCells == 0 && v.updatedCells == 0 ? quiet + 1 : 0;
        }
        recovery.print("post-return recovery");
        if (quiet < 8)
            throw std::runtime_error("Vegetation did not recover within the bounded return interval");
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

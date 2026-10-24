#include "../tests/render_fixtures.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
// Count C++ allocation calls in the render() interval. This is not a driver malloc profiler.
namespace {
std::atomic<uint64_t> allocationCalls{0};
}
void *operator new(std::size_t n) {
    allocationCalls.fetch_add(1, std::memory_order_relaxed);
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
namespace {
double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0 : values[size_t(p * (values.size() - 1))];
}
void image(const std::filesystem::path &path, uint32_t width, uint32_t height,
           const std::vector<glm::vec4> &pixels, bool hdr = true) {
    if (pixels.empty())
        return;
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (auto p : pixels) {
        auto c = hdr ? toneMap(glm::vec3(p), ToneMapper::AcesFitted)
                     : glm::clamp(glm::vec3(p), glm::vec3(0), glm::vec3(1));
        for (int k = 0; k < 3; ++k) {
            auto v = uint8_t(std::lround(linearToSrgb(c[k]) * 255));
            file.write(reinterpret_cast<const char *>(&v), 1);
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument("Usage: temporal_probe OUTPUT_DIR [--quick] [--width N --height "
                                        "N --frames N] [--require-validation] [--streaming]");
        std::filesystem::path output = argv[1];
        uint32_t width = 1920, height = 1080, frames = 120;
        bool validation = false, streaming = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--quick") {
                width = 320;
                height = 180;
                frames = 16;
            } else if (a == "--require-validation")
                validation = true;
            else if (a == "--streaming")
                streaming = true;
            else if ((a == "--width" || a == "--height" || a == "--frames") && i + 1 < argc) {
                auto v = std::stoul(argv[++i]);
                if (!v || v > 16384)
                    throw std::invalid_argument("Invalid probe count");
                if (a == "--width")
                    width = v;
                else if (a == "--height")
                    height = v;
                else
                    frames = v;
            } else
                throw std::invalid_argument("Unknown probe argument");
        }
        std::filesystem::create_directories(output);
        RendererOptions o;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.width = width;
        o.height = height;
        o.ui = false;
        o.validation = o.requireValidation = validation;
        o.terrainGrid = {32, .5f};
        o.terrainCapacity = 32;
        o.lod = {true, 8, 32};
        o.streaming.enabled = o.streaming.hierarchical = streaming;
        o.streaming.diskCache = false;
        o.streaming.maximumDistance = 128;
        o.streaming.diagnosticFrameSeconds = 1. / 60;
        Renderer renderer(o);
        fixtures::Swatches swatches(renderer);
        Camera camera;
        camera.local = {0, 0, 16};
        SceneSettings settings;
        settings.terrainVisible = streaming;
        settings.shadows.mode = ShadowMode::Off;
        if (streaming) {
            TerrainSettings terrain;
            terrain.heightScale = 10;
            renderer.setTerrainSettings(terrain);
            camera.local = {8, 15, 16};
            camera.pitch = -.3;
            settings.viewRadius = 2;
        }
        MeshMaterial m;
        m.alpha = AlphaMode::Blend;
        m.unlit = m.doubleSided = true;
        m.baseColour = {1, .2, .1, .3};
        m.emissive = {200000, 50000, 0};
        MeshInstance moving;
        moving.mesh = renderer.uploadMesh(makeQuad());
        moving.material = renderer.createMaterial(m);
        moving.scale = glm::vec3(.35);
        moving.position.local = {0, 1, 3};
        auto handle = renderer.createInstance(moving);
        std::ofstream metadata(output / "metadata.txt");
        metadata << "device=" << renderer.deviceName() << "\nvalidation=" << validation
                 << "\nextent=" << width << 'x' << height << "\nframes=" << frames
                 << "\nwarmup=16\nstreaming=" << streaming
                 << "\nallocation_counter=C++ new/new[] calls inside render; excludes malloc and "
                    "background CPU work attribution\n";
#ifdef NDEBUG
        metadata << "build=Release\n";
#else
        metadata << "build=Debug\n";
#endif
        if (streaming) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            do {
                renderer.render(camera, settings);
                auto &s = renderer.terrainStreamingStats();
                if (!s.pending && !s.detailPending && !s.ioJobs)
                    break;
            } while (std::chrono::steady_clock::now() < deadline);
            auto &s = renderer.terrainStreamingStats();
            metadata << "settled=" << (!s.pending && !s.detailPending && !s.ioJobs) << '\n';
        }
        std::ofstream csv(output / "timings.csv");
        csv << "mode,effective_taa,effective_bloom,cpu_median_ms,cpu_p95_ms,frame_gpu_median_ms,frame_"
               "gpu_p95_ms,taa_median_ms,taa_p95_ms,bloom_median_ms,bloom_p95_ms,taa_bytes,bloom_bytes,"
               "cpp_allocations,timestamps_available\n"
            << std::setprecision(8);
        std::ofstream passes(output / "bloom_passes.csv");
        passes << "mode,level,width,height,down_median_ms,down_p95_ms,up_median_ms,up_p95_ms\n";
        const auto sizes = bloomExtents(width, height);
        for (uint32_t mode = 0; mode < 4; ++mode) {
            settings.antialiasing = ((mode & 1) != 0) ? ow::AntialiasingMode::Taa : ow::AntialiasingMode::Off;
            settings.bloom.intensity = (mode & 2) ? .05f : 0;
            renderer.resetTemporalHistory();
            std::vector<double> cpu, gpu, taa, bloom;
            for (auto *v : {&cpu, &gpu, &taa, &bloom})
                v->reserve(frames);
            std::array<std::vector<double>, 6> downs, ups;
            for (auto &v : downs)
                v.reserve(frames);
            for (auto &v : ups)
                v.reserve(frames);
            uint64_t allocations = 0;
            for (uint32_t f = 0; f < frames + 16; ++f) {
                float t = float(f) / 60;
                camera.yaw = std::sin(t) * .05f;
                moving.position.local.x = 2 * std::sin(t * 2);
                renderer.updateInstance(handle, moving);
                auto before = allocationCalls.load(std::memory_order_relaxed);
                auto start = std::chrono::steady_clock::now();
                renderer.render(camera, settings);
                auto elapsed =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                        .count();
                auto after = allocationCalls.load(std::memory_order_relaxed);
                if (f >= 16) {
                    allocations += after - before;
                    cpu.push_back(elapsed);
                    gpu.push_back(renderer.renderingStats().frameMilliseconds);
                    taa.push_back(renderer.postStats().taaMilliseconds);
                    bloom.push_back(renderer.postStats().bloomMilliseconds);
                    for (size_t l = 0; l < sizes.size(); ++l) {
                        downs[l].push_back(renderer.postStats().bloomDownMilliseconds[l]);
                        ups[l].push_back(renderer.postStats().bloomUpMilliseconds[l]);
                    }
                }
            }
            const char *names[] = {"off", "taa", "bloom", "combined"};
            std::string name = names[mode];
            const auto &p = renderer.postStats();
            csv << name << ',' << p.taa << ',' << p.bloom;
            for (auto *v : {&cpu, &gpu, &taa, &bloom})
                csv << ',' << percentile(*v, .5) << ',' << percentile(*v, .95);
            csv << ',' << p.taaBytes << ',' << p.bloomBytes << ',' << allocations << ','
                << (p.timingsAvailable && renderer.renderingStats().timingsAvailable) << '\n';
            if (p.bloom)
                for (size_t l = 0; l < sizes.size(); ++l)
                    passes << name << ',' << l << ',' << sizes[l].x << ',' << sizes[l].y << ','
                           << percentile(downs[l], .5) << ',' << percentile(downs[l], .95) << ','
                           << percentile(ups[l], .5) << ',' << percentile(ups[l], .95) << '\n';
            renderer.capture(output / (name + ".ppm"));
            if (p.taa || p.bloom) {
                auto snap = renderer.readPostSnapshot();
                image(output / (name + "_resolved.ppm"), snap.width, snap.height, snap.resolved);
                if (p.taa) {
                    std::vector<glm::vec4> motion = snap.motion, reactive, rejected;
                    reactive.reserve(snap.reactivity.size());
                    rejected.reserve(snap.depth.size());
                    for (auto &v : motion)
                        v = {.5f + v.x * 8, .5f + v.y * 8, v.w, 1};
                    for (auto v : snap.reactivity)
                        reactive.emplace_back(v, v, v, 1);
                    for (auto v : snap.depth)
                        rejected.emplace_back(v < 0 ? 1.f : 0.f, 0, 0, 1);
                    image(output / (name + "_motion.ppm"), snap.width, snap.height, motion, false);
                    image(output / (name + "_reactive.ppm"), snap.width, snap.height, reactive, false);
                    image(output / (name + "_rejection.ppm"), snap.width, snap.height, rejected, false);
                }
                for (size_t l = 0; l < snap.bloom.size(); ++l)
                    image(output / (name + "_bloom_" + std::to_string(l) + ".ppm"), snap.bloomSizes[l].x,
                          snap.bloomSizes[l].y, snap.bloom[l]);
            }
            std::cout << name << ": CPU " << percentile(cpu, .5) << " ms, GPU " << percentile(gpu, .5)
                      << " ms, C++ allocations " << allocations << '\n';
        }
        if (renderer.validationErrors())
            throw std::runtime_error("Vulkan validation errors in probe");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

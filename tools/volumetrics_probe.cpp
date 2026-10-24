#include "../tests/render_fixtures.hpp"
#include "volumetric_demo.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
// Count C++ allocation calls in the render() interval. This is not a driver
// malloc profiler.
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
            throw std::invalid_argument("Usage: volumetrics_probe OUTPUT [--quick] [--require-validation] "
                                        "[--width N --height N --frames N] [--shadow-review] [--shader-dir DIR]");
        std::filesystem::path output = argv[1], shaderDirectory = OPENWORLD_SHADER_DIR;
        uint32_t width = 1920, height = 1080, count = 120;
        bool validation = false, shadowReview = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--quick") {
                width = 320;
                height = 180;
                count = 8;
            } else if (a == "--shadow-review")
                shadowReview = true;
            else if (a == "--shader-dir" && i + 1 < argc)
                shaderDirectory = argv[++i];
            else if (a == "--require-validation")
                validation = true;
            else if ((a == "--width" || a == "--height" || a == "--frames") && i + 1 < argc) {
                auto n = std::stoul(argv[++i]);
                if (n < 1 || n > 16384)
                    throw std::invalid_argument("Invalid probe size");
                if (a == "--width")
                    width = n;
                else if (a == "--height")
                    height = n;
                else
                    count = n;
            } else
                throw std::invalid_argument("Unknown probe option");
        }
        std::filesystem::create_directories(output);
        RendererOptions o;
        o.shaderDirectory = shaderDirectory;
        o.ui = false;
        o.validation = o.requireValidation = validation;
        o.width = width;
        o.height = height;
        o.terrainGrid = {8, .5f};
        o.terrainCapacity = 8;
        o.visibilityCapacity = 128;
        Renderer r(o);
        VolumetricDemo demo(r);
        WorldPosition anchor;
        anchor.local = {0, 100, -45};
        Camera base;
        base.local = {0, 100, 0};
        SceneSettings s;
        s.terrainVisible = false;
        s.atmosphere.mode = AtmosphereMode::Physical;
        s.antialiasing = ow::AntialiasingMode::Taa;
        s.bloom.intensity = .05f;
        s.shadows.nearDistance = 150;
        s.shadows.farDistance = 500;
        s.volumetrics.enabled = true;
        s.volumetrics.extinction = .0005f;
        s.volumetrics.height = 100;
        if (shadowReview) {
            base.pitch = .2f;
            base.yaw = -2.1f;
            anchor.local = {std::sin(base.yaw) * 45, 100, -std::cos(base.yaw) * 45};
            s.lighting.sunDirection = glm::normalize(glm::vec3(-.6, .8, .35));
            s.volumetrics.anisotropy = .7f;
            s.shadows.nearDistance = 1000;
        }
        demo.configure(2, anchor);
        LocalLight moving;
        moving.position = translated(anchor, {0, 3, 4});
        moving.range = 35;
        moving.intensity = 50000;
        auto light = r.createLight(moving);
        std::vector<LightHandle> dense;
        dense.reserve(256);
        std::ofstream csv(output / "timings.csv"), frames(output / "frames.csv"),
            meta(output / "metadata.txt");
        csv << "case,cpu_median_ms,cpu_p95_ms,gpu_median_ms,gpu_p95_ms,assign_ms,"
               "inject_ms,history_ms,"
               "integrate_ms,sky_ms,composite_ms,bytes,excluded,overflow,new_calls,"
               "timings_available\n";
        frames << "case,frame,cpu_ms,gpu_ms,volumetric_ms,bytes,new_calls\n";
        meta << "device=" << r.deviceName() << "\nvalidation=" << validation << "\nwidth=" << width
             << "\nheight=" << height << "\nframes=" << count << "\nwarmup=16\n"
             << "shadow_review=" << shadowReview << "\nshader_directory=" << shaderDirectory << '\n';
#ifdef NDEBUG
        meta << "build=Release\n";
#else
        meta << "build=Debug\n";
#endif
        for (std::string mode : {"off", "normal", "high", "single_sample", "spatial", "camera", "light",
                                 "transient_light", "dense"}) {
            if (shadowReview && mode != "normal" && mode != "high")
                continue;
            s.volumetrics.enabled = mode != "off";
            s.volumetrics.quality = mode == "high" ? VolumetricQuality::High : VolumetricQuality::Normal;
            s.volumetrics.temporal = mode != "spatial";
            s.volumetrics.samples = mode == "single_sample" ? 1 : 2;
            if (mode == "dense")
                for (int k = 0; k < 256; ++k) {
                    auto l = moving;
                    l.position = translated(anchor, {double(k % 8) - 4, 0, -double(k / 8)});
                    dense.push_back(r.createLight(l));
                }
            std::vector<double> cpu, gpu;
            cpu.reserve(count);
            gpu.reserve(count);
            uint64_t allocations = 0;
            r.resetTemporalHistory();
            for (uint32_t frame = 0; frame < count + 16; ++frame) {
                Camera camera = base;
                if (mode == "camera") {
                    camera.yaw = .15f * std::sin(float(frame) * .06f);
                    camera.local.x = std::sin(double(frame) * .04) * 4;
                }
                auto l = moving;
                l.volumetricHistory = mode != "transient_light";
                if (mode == "light" || mode == "transient_light")
                    l.position = translated(moving.position, {std::sin(double(frame) * .15) * 15, 0, 0});
                r.updateLight(light, l);
                auto before = allocationCalls.load();
                auto begin = std::chrono::steady_clock::now();
                r.render(camera, s);
                auto calls = allocationCalls.load() - before;
                double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                        .count();
                if (frame >= 16) {
                    auto &v = r.volumetricStats();
                    double total = v.assignmentMilliseconds + v.injectionMilliseconds +
                                   v.temporalMilliseconds + v.integrationMilliseconds + v.skyMilliseconds +
                                   v.compositeMilliseconds;
                    cpu.push_back(ms);
                    gpu.push_back(r.renderingStats().frameMilliseconds);
                    allocations += calls;
                    frames << mode << ',' << frame - 16 << ',' << ms << ',' << gpu.back() << ',' << total
                           << ',' << v.bytes << ',' << calls << '\n';
                }
            }
            auto v = r.volumetricStats();
            csv << mode << ',' << percentile(cpu, .5) << ',' << percentile(cpu, .95) << ','
                << percentile(gpu, .5) << ',' << percentile(gpu, .95) << ',' << v.assignmentMilliseconds
                << ',' << v.injectionMilliseconds << ',' << v.temporalMilliseconds << ','
                << v.integrationMilliseconds << ',' << v.skyMilliseconds << ',' << v.compositeMilliseconds
                << ',' << v.bytes << ',' << v.excludedLights << ',' << v.overflowClusters << ','
                << allocations << ',' << v.timingsAvailable << '\n';
            auto hdr = r.readHdrSnapshot();
            image(output / (mode + ".ppm"), hdr.width, hdr.height, hdr.pixels);
            if (v.active) {
                auto snapshot = r.readVolumetricSnapshot();
                std::vector<glm::vec4> layer(snapshot.grid.x * snapshot.grid.y);
                std::copy(snapshot.transmittance.end() - layer.size(), snapshot.transmittance.end(),
                          layer.begin());
                image(output / (mode + "_T.ppm"), snapshot.grid.x, snapshot.grid.y, layer, false);
            }
            if (r.validationErrors())
                throw std::runtime_error("Volumetric probe validation errors");
            std::cout << mode << " complete\n";
        }
        for (auto h : dense)
            r.removeLight(h);
        r.removeLight(light);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

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
            throw std::invalid_argument(
                "Usage: atmosphere_probe OUTPUT [--quick] [--streaming] "
                "[--require-validation] [--flat-horizon] [--width N --height N --frames N --radius "
                "N] [--horizon-distance M --altitude M --fill-seconds N]");
        std::filesystem::path output = argv[1];
        uint32_t width = 1920, height = 1080, count = 120, requestedRadius = 0, horizonDistance = 0,
                 fillSeconds = 60, altitude = 0;
        bool flatHorizon = false;
        bool quick = false, streaming = false, validation = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--quick") {
                quick = true;
                width = 320;
                height = 180;
                count = 16;
            } else if (a == "--streaming")
                streaming = true;
            else if (a == "--flat-horizon")
                flatHorizon = true;
            else if (a == "--require-validation")
                validation = true;
            else if ((a == "--width" || a == "--height" || a == "--frames" || a == "--radius" ||
                      a == "--horizon-distance" || a == "--fill-seconds" || a == "--altitude") &&
                     i + 1 < argc) {
                auto v = std::stoul(argv[++i]);
                if (!v || v > (a == "--horizon-distance" ? 1048576u
                               : a == "--fill-seconds"   ? 3600u
                                                         : 16384u))
                    throw std::invalid_argument("Invalid probe size");
                if (a == "--horizon-distance")
                    horizonDistance = v;
                else if (a == "--fill-seconds")
                    fillSeconds = v;
                else if (a == "--altitude")
                    altitude = v;
                else if (a == "--width")
                    width = v;
                else if (a == "--height")
                    height = v;
                else if (a == "--radius")
                    requestedRadius = v;
                else
                    count = v;
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
        o.terrainGrid = {streaming && !quick ? 256u : 32u, .5f};
        o.terrainCapacity = o.terrainGrid.cells;
        o.lod = {true, quick ? 8u : 32u, 64};
        o.streaming.enabled = o.streaming.hierarchical = streaming;
        o.streaming.diskCache = false;
        o.streaming.maximumDistance = quick ? 128 : 16384;
        o.streaming.horizonBytes = 128 * terrainMiB;
        o.streaming.horizonMaximumDistance = horizonDistance ? horizonDistance
                                             : quick         ? 256
                                                             : 1048576;
        o.curvedHorizon = !flatHorizon;
        o.streaming.diagnosticFrameSeconds = 1. / 60;
        o.shadowCapacity.resolution = quick ? 128 : 2048;
        Renderer r(o);
        Camera c;
        c.local = {0, 100, 0};
        c.pitch = .08f;
        SceneSettings s;
        s.terrainVisible = streaming;
        s.shadows.mode = ShadowMode::Off;
        s.viewRadius = requestedRadius ? int(requestedRadius) : quick ? 8 : 64;
        if (s.viewRadius > terrainMaximumRadius(o.terrainGrid))
            throw std::invalid_argument("Probe radius exceeds detailed view cap");
        s.taa.enabled = true;
        s.bloom.intensity = .05f;
        if (streaming) {
            TerrainSettings t;
            t.heightScale = quick ? 10 : 300;
            r.setTerrainSettings(t);
            c.local.y = quick ? 50 : 1000;
            c.pitch = -.1f;
        }
        if (altitude)
            c.local.y = altitude;
        std::ofstream meta(output / "metadata.txt");
        meta << "device=" << r.deviceName() << "\nvalidation=" << validation << "\nextent=" << width
             << 'x' << height << "\nframes=" << count << "\nwarmup=16\nstreaming=" << streaming
             << "\nsmall_workload=" << quick << "\ndetailed_radius_chunks=" << s.viewRadius
             << "\ncurved_horizon=" << !flatHorizon
             << "\nhorizon_cap=" << o.streaming.horizonMaximumDistance
             << "\nallocation_counter=C++ new/new[] in render, excludes malloc\n";
#ifdef NDEBUG
        meta << "build=Release\n";
#else
        meta << "build=Debug\n";
#endif
        if (streaming) {
            auto start = std::chrono::steady_clock::now();
            int stable = 0;
            do {
                r.render(c, s);
                auto &t = r.terrainStreamingStats();
                stable = (!t.pending && !t.detailPending && !t.ioJobs) ? stable + 1 : 0;
                if (stable > 32)
                    break;
            } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(fillSeconds));
            meta << "settled=" << (stable > 32) << "\nfill_seconds="
                 << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                 << '\n';
        }
        meta << "horizon_bytes=" << r.terrainStreamingStats().horizonBytes
             << "\nhorizon_levels=" << r.terrainStreamingStats().horizonLevels
             << "\nhorizon_effective=" << r.terrainStreamingStats().horizonEffectiveDistance
             << "\nhorizon_limited=" << r.terrainStreamingStats().horizonLimited << '\n';
        std::ofstream csv(output / "timings.csv"), raw(output / "frames.csv");
        csv << "mode,effective,fallback,cpu_median_ms,cpu_p95_ms,gpu_median_ms,gpu_p95_ms,lut_"
               "median_ms,lut_p95_ms,"
               "ibl_median_ms,ibl_p95_ms,composite_median_ms,composite_p95_ms,atmosphere_bytes,"
               "terrain_bytes,cpp_allocations,timestamps_available\n";
        raw << "mode,frame,cpu_ms,gpu_ms,lut_ms,ibl_ms,composite_ms,coverage_pending,horizon_ready,"
               "horizon_requested,profile_updates,sky_updates,environment_updates\n";
        const char *names[] = {"off",           "analytic",    "physical_static",
                               "physical_turn", "moving_sun",  "sunset",
                               "hazy",          "altitude_2km"};
        const double initialHeight = c.local.y;
        for (uint32_t mode = 0; mode < 8; ++mode) {
            s.atmosphere.mode = mode == 0   ? AtmosphereMode::Off
                                : mode == 1 ? AtmosphereMode::Analytic
                                            : AtmosphereMode::Physical;
            s.atmosphere.aerosolDensity = mode == 6 ? 4 : 1;
            c.local.y = mode == 7 ? initialHeight + 2000 : initialHeight;
            s.lighting.sunDirection =
                glm::normalize(mode == 5 ? glm::vec3(.5, .025, -1) : glm::vec3(-.6, .8, .35));
            c.yaw = 0;
            r.resetTemporalHistory();
            std::array<std::vector<double>, 5> samples;
            for (auto &v : samples)
                v.reserve(count);
            uint64_t allocations = 0;
            bool available = true;
            for (uint32_t f = 0; f < count + 16; ++f) {
                if (mode == 3)
                    c.yaw = std::sin(float(f) / 60) * .2f;
                if (mode == 4)
                    s.lighting.sunDirection =
                        glm::normalize(glm::vec3(-.6f + .005f * f, .8f, .35f));
                if (streaming && mode == 3) {
                    c.local.x += quick ? .2 : 100. / 60;
                    c.update({}, 0, o.terrainGrid);
                }
                auto before = allocationCalls.load();
                auto start = std::chrono::steady_clock::now();
                r.render(c, s);
                double cpu = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
                auto after = allocationCalls.load();
                const auto &a = r.atmosphereStats();
                const auto &t = r.terrainStreamingStats();
                if (f >= 16) {
                    allocations += after - before;
                    double values[] = {cpu, r.renderingStats().frameMilliseconds, a.lutMilliseconds,
                                       a.environmentMilliseconds, a.compositeMilliseconds};
                    for (int j = 0; j < 5; ++j)
                        samples[j].push_back(values[j]);
                    available &=
                        r.renderingStats().timingsAvailable && (mode < 2 || a.timingsAvailable);
                    raw << names[mode] << ',' << f;
                    for (auto v : values)
                        raw << ',' << v;
                    raw << ',' << t.pending << ',' << t.horizonReady << ',' << t.horizonRequested
                        << ',' << a.profileUpdates << ',' << a.skyUpdates << ','
                        << a.environmentUpdates << '\n';
                }
            }
            csv << names[mode] << ',' << uint32_t(r.atmosphereStats().effective) << ','
                << atmosphereFallbackName(r.atmosphereStats().fallback);
            for (auto &v : samples)
                csv << ',' << percentile(v, .5) << ',' << percentile(v, .95);
            csv << ',' << r.atmosphereStats().bytes << ',' << r.terrainAllocatedBytes() << ','
                << allocations << ',' << available << '\n';
            r.capture(output / (std::string(names[mode]) + ".ppm"));
            if (r.atmosphereStats().effective == AtmosphereMode::Physical) {
                auto a = r.readAtmosphereSnapshot();
                float scale = s.lighting.sunLux * exposure(s.output);
                for (auto &p : a.sky)
                    p *= scale;
                image(output / (std::string(names[mode]) + "_sky.ppm"), a.skyExtent.x,
                      a.skyExtent.y, a.sky);
                image(output / (std::string(names[mode]) + "_transmittance.ppm"), 256, 64,
                      a.transmittance, false);
                image(output / (std::string(names[mode]) + "_multiple.ppm"), 32, 32,
                      a.multipleScattering, false);
                const auto n = a.volumeSize;
                std::vector<glm::vec4> sliceT(a.aerialTransmittance.end() - n * n,
                                              a.aerialTransmittance.end());
                std::vector<glm::vec4> sliceS(a.aerialScattering.end() - n * n,
                                              a.aerialScattering.end());
                for (auto &p : sliceS)
                    p *= scale;
                image(output / (std::string(names[mode]) + "_aerial_T.ppm"), n, n, sliceT, false);
                image(output / (std::string(names[mode]) + "_aerial_S.ppm"), n, n, sliceS);
            }
            std::cout << names[mode] << ": GPU " << percentile(samples[1], .5)
                      << " ms, allocations " << allocations << '\n';
        }
        if (r.validationErrors())
            throw std::runtime_error("Atmosphere probe Vulkan validation");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

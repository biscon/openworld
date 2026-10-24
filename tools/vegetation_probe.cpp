#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <new>
#include <png.h>
#include <sstream>
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
void *operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
void *operator new(std::size_t n, std::align_val_t a) {
    allocationCalls.fetch_add(1, std::memory_order_relaxed);
    void *p = nullptr;
    if (posix_memalign(&p, std::max(size_t(a), sizeof(void *)), std::max(n, size_t(1))) == 0)
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}
void operator delete(void *p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
using namespace ow;
namespace {
using Clock = std::chrono::steady_clock;
struct Options {
    std::filesystem::path output;
    uint32_t width = 1920, height = 1080, frames = 600, warmup = 120, repeats = 3, side = 256;
    std::string sequence = "all", representation = "all", profile = "v1";
    bool mixed = false, rgba = false, prepass = true;
    bool quick = false, validation = false, fixed = false, variants = false, captures = true, canopy = false;
    float density = 1, grassShadowDistance = 25, exposure = 15;
    int asset = -1;
    GtaoMode gtao = GtaoMode::Off;
};
double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[size_t(p * (values.size() - 1))];
}
void png(const std::filesystem::path &path, uint32_t width, uint32_t height,
         const std::vector<uint8_t> &pixels) {
    png_image im{};
    im.version = PNG_IMAGE_VERSION;
    im.width = width;
    im.height = height;
    im.format = PNG_FORMAT_RGB;
    if (!png_image_write_to_file(&im, path.c_str(), 0, pixels.data(), 0, nullptr))
        throw std::runtime_error("Writing capture: " + path.string());
}
void capture(Renderer &r, const std::filesystem::path &path) {
    auto temporary = path;
    temporary += ".ppm";
    r.capture(temporary);
    std::ifstream f(temporary, std::ios::binary);
    std::string magic;
    uint32_t w, h, max;
    f >> magic >> w >> h >> max;
    f.get();
    if (magic != "P6" || max != 255 || w != r.width() || h != r.height())
        throw std::runtime_error("Invalid renderer capture");
    std::vector<uint8_t> bytes(size_t(w) * h * 3);
    f.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!f)
        throw std::runtime_error("Truncated renderer capture");
    png(path, w, h, bytes);
    f.close();
    std::filesystem::remove(temporary);
}
Camera cameraAt(const std::string &sequence, float t, uint32_t side, bool rolling) {
    Camera c;
    c.renderFar = 300;
    c.local = {0, 1.7, side * .15};
    c.pitch = -.10f;
    if (sequence == "asset") {
        c.local = {0, .65, 1.8};
        c.pitch = -.1f;
    }
    if (sequence == "grazing") {
        c.local.y = .35;
        c.pitch = .025f;
    }
    if (sequence == "aerial") {
        c.local.y = 12;
        c.pitch = -.55f;
        c.local.x = 3 * t;
    }
    if (sequence == "moving" || sequence == "rolling") {
        c.local.z -= 3 * t;
        c.yaw = .12f * std::sin(t * .6f);
    }
    c.local.y += vegetationGroundHeight(float(c.local.x), float(c.local.z), rolling);
    return c;
}
struct Coverage {
    double fraction = 0;
    std::array<double, 16> tiles{};
};
Coverage coverage(Renderer &r, const Camera &camera, SceneSettings settings) {
    settings.vegetation.coverage = true;
    settings.shadows.mode = ShadowMode::Off;
    settings.taa.enabled = false;
    settings.gtao.mode = GtaoMode::Off;
    settings.atmosphere.mode = AtmosphereMode::Off;
    settings.background[0] = settings.background[1] = settings.background[2] = 0;
    r.render(camera, settings);
    auto hdr = r.readHdrSnapshot();
    Coverage result;
    std::array<uint32_t, 16> counts{};
    for (uint32_t y = 0; y < hdr.height; ++y)
        for (uint32_t x = 0; x < hdr.width; ++x) {
            uint32_t tile = std::min(3u, y * 4 / hdr.height) * 4 + std::min(3u, x * 4 / hdr.width);
            ++counts[tile];
            bool covered = hdr.pixels[y * hdr.width + x].r > .9f;
            result.fraction += covered;
            result.tiles[tile] += covered;
        }
    result.fraction /= hdr.pixels.size();
    for (uint32_t i = 0; i < 16; ++i)
        result.tiles[i] /= counts[i];
    return result;
}
struct Sample {
    uint64_t id = 0;
    double cpu = 0, frameGpu = 0;
    VegetationStats vegetation;
    uint64_t allocations = 0;
    bool frameTiming = false;
};
struct Run {
    std::vector<Sample> samples;
};
Run run(Renderer &r, SceneSettings settings, const std::string &sequence, const Options &o, bool rolling) {
    r.resetTemporalHistory();
    Run result;
    result.samples.reserve(o.frames);
    std::vector<double> cpus(o.frames + o.warmup + 2);
    std::vector<uint64_t> allocations(cpus.size());
    // Last completed ID establishes the local start; two drain submissions reveal
    // final samples.
    uint64_t start = 0;
    for (uint32_t f = 0; f < o.frames + o.warmup + 2; ++f) {
        float t = float(f) / 120;
        settings.vegetation.time = t;
        auto camera = cameraAt(sequence, t, o.side, rolling);
        auto before = allocationCalls.load(std::memory_order_relaxed);
        auto begin = Clock::now();
        r.render(camera, settings);
        cpus[f] = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        allocations[f] = allocationCalls.load(std::memory_order_relaxed) - before;
        const auto &v = r.vegetationStats();
        // With two frame slots, the completed record belongs to this invocation
        // minus two.
        if (f >= 2 && v.completed) {
            uint32_t source = f - 2;
            if (!start)
                start = v.frame - source;
            if (v.frame != start + source)
                throw std::runtime_error("Mismatched completed vegetation frame");
            if (source >= o.warmup) {
                Sample sample;
                sample.id = v.frame;
                sample.cpu = cpus[source];
                sample.allocations = allocations[source];
                sample.vegetation = v;
                sample.frameGpu = r.renderingStats().frameMilliseconds;
                sample.frameTiming = r.renderingStats().timingsAvailable;
                result.samples.push_back(sample);
            }
        }
    }
    if (result.samples.size() != o.frames)
        throw std::runtime_error("Incomplete probe sample drain");
    return result;
}
void motionCapture(Renderer &r, SceneSettings settings, const std::string &sequence, const Options &o,
                   bool rolling, const std::string &prefix) {
    r.resetTemporalHistory();
    uint32_t count = o.quick ? 4 : 24;
    for (uint32_t f = 0; f < count * 4; ++f) {
        settings.vegetation.time = f / 120.f;
        r.render(cameraAt(sequence, f / 120.f, o.side, rolling), settings);
        if (f % 4 == 3) {
            std::ostringstream name;
            name << prefix << "_motion_" << std::setw(3) << std::setfill('0') << f / 4 << ".png";
            capture(r, o.output / name.str());
        }
    }
    if (r.postStats().taa) {
        auto snap = r.readPostSnapshot();
        std::vector<uint8_t> bytes(size_t(snap.width) * snap.height * 3);
        for (size_t i = 0; i < snap.motion.size(); ++i) {
            auto v = snap.motion[i];
            bytes[3 * i] = uint8_t(std::clamp(.5f + v.x * 16, 0.f, 1.f) * 255);
            bytes[3 * i + 1] = uint8_t(std::clamp(.5f + v.y * 16, 0.f, 1.f) * 255);
            bytes[3 * i + 2] = v.w > 0 ? 255 : 0;
        }
        png(o.output / (prefix + "_motion_vectors.png"), snap.width, snap.height, bytes);
        for (size_t i = 0; i < snap.depth.size(); ++i) {
            bytes[3 * i] = snap.depth[i] < 0 ? 255 : 0;
            bytes[3 * i + 1] = 0;
            bytes[3 * i + 2] = snap.reactivity[i] * 255;
        }
        png(o.output / (prefix + "_history_rejection.png"), snap.width, snap.height, bytes);
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument(
                "Usage: vegetation_probe OUTPUT_DIR [--quick] [--width N --height N "
                "--frames N --warmup N --repeats N --side N] [--sequence "
                "all|close|grazing|moving|aerial|rolling] [--representation "
                "all|blades|cards|hybrid] [--density 4|8] [--variants] [--fixed] "
                "[--require-validation] [--no-captures] [--grass-shadow-distance N] "
                "[--gtao off|half|full] [--canopy-fixture] [--profile v1|v2] "
                "[--mixed] [--rgba] [--no-prepass] [--asset grass|fern|shrub] [--exposure EV]");
        Options o;
        o.output = argv[1];
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--quick") {
                o.quick = true;
                o.width = 320;
                o.height = 180;
                o.frames = 8;
                o.warmup = 4;
                o.repeats = 1;
                o.side = 16;
            } else if (arg == "--asset" && i + 1 < argc) {
                std::string name = argv[++i];
                o.asset = name == "grass" ? 0 : name == "fern" ? 2 : name == "shrub" ? 3 : -1;
                if (o.asset < 0)
                    throw std::invalid_argument("Asset must be grass, fern or shrub");
                o.profile = "v2";
                o.sequence = "asset";
            } else if (arg == "--exposure" && i + 1 < argc) {
                o.exposure = std::stof(argv[++i]);
                if (!std::isfinite(o.exposure) || o.exposure < 0 || o.exposure > 24)
                    throw std::invalid_argument("Invalid exposure");
            } else if (arg == "--profile" && i + 1 < argc)
                o.profile = argv[++i];
            else if (arg == "--no-prepass")
                o.prepass = false;
            else if (arg == "--mixed")
                o.mixed = true;
            else if (arg == "--rgba")
                o.rgba = true;
            else if (arg == "--require-validation")
                o.validation = true;
            else if (arg == "--fixed")
                o.fixed = true;
            else if (arg == "--variants")
                o.variants = true;
            else if (arg == "--canopy-fixture")
                o.canopy = true;
            else if (arg == "--no-captures")
                o.captures = false;
            else if (arg == "--grass-shadow-distance" && i + 1 < argc) {
                o.grassShadowDistance = std::stof(argv[++i]);
                if (!std::isfinite(o.grassShadowDistance) || o.grassShadowDistance < 0 ||
                    o.grassShadowDistance > 50)
                    throw std::invalid_argument("Grass shadow distance must be in [0,50]");
            } else if (arg == "--gtao" && i + 1 < argc) {
                std::string value = argv[++i];
                if (value == "off")
                    o.gtao = GtaoMode::Off;
                else if (value == "half")
                    o.gtao = GtaoMode::Half;
                else if (value == "full")
                    o.gtao = GtaoMode::Full;
                else
                    throw std::invalid_argument("GTAO must be off, half or full");
            } else if ((arg == "--sequence" || arg == "--representation") && i + 1 < argc) {
                (arg == "--sequence" ? o.sequence : o.representation) = argv[++i];
            } else if (i + 1 < argc &&
                       (arg == "--width" || arg == "--height" || arg == "--frames" || arg == "--warmup" ||
                        arg == "--repeats" || arg == "--side" || arg == "--density")) {
                auto n = std::stoul(argv[++i]);
                if (!n || n > 16384)
                    throw std::invalid_argument("Invalid probe size/count");
                if (arg == "--width")
                    o.width = n;
                else if (arg == "--height")
                    o.height = n;
                else if (arg == "--frames")
                    o.frames = n;
                else if (arg == "--warmup")
                    o.warmup = n;
                else if (arg == "--repeats")
                    o.repeats = n;
                else if (arg == "--side")
                    o.side = n;
                else {
                    if (n != 4 && n != 8)
                        throw std::invalid_argument("Density must be 4 or 8");
                    o.density = n / 8.f;
                }
            } else
                throw std::invalid_argument("Unknown or incomplete probe argument: " + arg);
        }
        if (o.profile != "v1" && o.profile != "v2")
            throw std::invalid_argument("Profile must be v1 or v2");
        if (o.profile == "v2") {
            o.representation = "blades";
            o.grassShadowDistance = 0;
        }
        std::vector<std::string> sequences = {"close", "grazing", "moving", "aerial", "rolling"};
        if (o.sequence == "asset")
            sequences = {"asset"};
        if (o.sequence != "all") {
            if (std::find(sequences.begin(), sequences.end(), o.sequence) == sequences.end())
                throw std::invalid_argument("Unknown sequence");
            sequences = {o.sequence};
        }
        std::vector<std::string> representations = {"blades", "cards", "hybrid"};
        if (o.representation != "all" && std::find(representations.begin(), representations.end(),
                                                   o.representation) == representations.end())
            throw std::invalid_argument("Unknown representation");
        std::filesystem::create_directories(o.output);
        auto buildStart = Clock::now();
        auto assets = o.profile == "v1" ? readVegetationAssets(OPENWORLD_VEGETATION_DIR) : VegetationAssets{};
        std::shared_ptr<VegetationLibrary> library;
        if (o.profile == "v2")
            library = std::make_shared<VegetationLibrary>(
                readVegetationLibrary(OPENWORLD_VEGETATION_LIBRARY_DIR, o.rgba));
        double assetMs = std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();
        std::ofstream meta(o.output / "metadata.txt");
        meta << "profile=" << o.profile << "\nmixed=" << o.mixed << "\nrgba=" << o.rgba
             << "\nexposure_ev100=" << o.exposure << "\nasset=" << o.asset
             << "\nvegetation_prepass=" << o.prepass
             << "\nasset_revision=" << (library ? library->revision : 1) << "\n";
        meta << "extent=" << o.width << 'x' << o.height << "\nnative_output=true\nframes=" << o.frames
             << "\nwarmup=" << o.warmup << "\nrepeats=" << o.repeats << "\nside=" << o.side
             << "\ndensity=" << o.density * 8
             << "\nseed=173\nsimulation_step=1/120\nasset_load_ms=" << assetMs
             << "\nvalidation=" << o.validation << "\nquick=" << o.quick
             << "\nLOD=20,45,70m; fade=55..70m; grass_shadow_fade="
             << std::max(0.f, o.grassShadowDistance - 5) << ".." << o.grassShadowDistance << "m\n"
             << "allocation_counter=C++ new/new[] inside render; includes other "
                "renderer modules, excludes driver malloc\n"
             << "memory=driver allocation requirements, includes retained upload "
                "staging and diagnostic buffer; shared render targets excluded\n";
#ifdef NDEBUG
        meta << "build=Release\n";
#else
        meta << "build=Debug\n";
#endif
        std::ofstream pairs(o.output / "paired.csv");
        pairs << "sequence,representation,comparison,variant,repeat,cpu_delta_"
                 "median_ms,gpu_delta_median_ms,"
                 "gpu_delta_p95_ms,timestamps\n";
        std::ofstream raw(o.output / "frames.csv"), summary(o.output / "timings.csv"),
            coverageCsv(o.output / "coverage.csv"), report(o.output / "findings.md");
        if (!meta || !raw || !summary || !coverageCsv || !report || !pairs)
            throw std::runtime_error("Cannot open probe output files");
        raw << "sequence,representation,comparison,variant,repeat,enabled,"
               "completed_frame,cpu_ms,gpu_ms,prepare_ms,select_ms,depth_ms,colour_"
               "ms,shadows_ms,visible,triangles,casters,overflow,bytes,allocations,"
               "timestamps,caster_candidates,selection_groups\n";
        summary << "sequence,representation,comparison,variant,repeat,enabled,cpu_"
                   "median_ms,cpu_p95_ms,gpu_median_ms,gpu_p95_ms,prepare_median_"
                   "ms,prepare_p95_ms,vegetation_gpu_median_ms,vegetation_gpu_p95_"
                   "ms,allocations,bytes\n";
        coverageCsv << "landscape,sequence,representation,width,density,fraction,"
                       "tile_error\n";
        if (library)
            report << "# V2 foliage foundation results\n\nSaved reusable blade, fern and shrub assets. No "
                      "grass casting. "
                      "The configured depth prepass and GTAO are retained in both off/on frames. "
                      "Review paired.csv, timings.csv, coverage.csv and metadata.txt; timings alone do not "
                      "establish appearance acceptance.\n\n";
        else
            report << "# V1 vegetation comparison\n\nLegacy blade/card/hybrid comparison. "
                      "Review paired.csv and coverage.csv before drawing representation conclusions.\n\n";
        meta << "scene=terrain off; owned ground mesh; atmosphere off; volumetrics "
                "off; bloom off; HiZ off\n"
             << "canopy_fixture=" << o.canopy << "\n"
             << "requested_gtao=" << uint32_t(o.gtao) << "\ngrass_shadow_distance=" << o.grassShadowDistance
             << "\n"
             << "lighting=sun lux 100000; direction -0.6,0.8,0.35; environment "
                "3000 nits; exposure recorded above; "
                "ACES fitted\n"
             << "CSM=near 1000m; far 8192m; bias 0.75 texels; normal bias 0.25 "
                "texels; TAA history 0.9\n"
             << "variants=default TAA+CSM; shadows_off; taa_off; strong_wind 2; "
                "low_sun -0.8,0.15,0.4; "
             << (library ? "no_prepass" : "prepass") << "; half GTAO\n"
             << "allocation_bytes=peak committed Vulkan buffer/image requirements "
                "including texture upload "
                "peak; no shared target duplication\n";
        bool target = false;
        for (bool rolling : {false, true}) {
            bool needed = std::any_of(sequences.begin(), sequences.end(),
                                      [&](const auto &name) { return (name == "rolling") == rolling; });
            if (!needed)
                continue;
            auto begin = Clock::now();
            auto fixture =
                std::make_shared<VegetationFixture>(makeVegetationFixture(assets, o.side, rolling));
            RendererOptions options;
            options.ui = false;
            options.width = o.width;
            options.height = o.height;
            options.shaderDirectory = OPENWORLD_SHADER_DIR;
            options.validation = options.requireValidation = o.validation;
            options.forceFixedMaterials = o.fixed;
            options.terrainGrid = {8, .5f};
            options.terrainCapacity = 8;
            if (library) {
                options.vegetation.library = library;
                options.vegetation.population = std::make_shared<VegetationPopulation>(
                    makeVegetationPopulation(o.side, o.mixed, rolling));
                if (o.asset >= 0) {
                    auto specimen = std::make_shared<VegetationPopulation>();
                    specimen->cells.push_back({{}, 0, 1});
                    specimen->instances.push_back({{0, 0, 0, 1}, 0, .5f, 173, uint32_t(o.asset)});
                    options.vegetation.population = specimen;
                }
            } else
                options.vegetation.fixture = fixture;
            if (o.quick)
                options.shadowCapacity.resolution = 128;
            Renderer r(options);
            target |= r.deviceName().find("RTX 3060") != std::string::npos;
            meta << "device=" << r.deviceName() << "\nbindless=" << r.materialStats().bindless
                 << "\nrolling=" << rolling << "\nfixture_renderer_load_ms="
                 << std::chrono::duration<double, std::milli>(Clock::now() - begin).count()
                 << "\nshadow_resolution=" << options.shadowCapacity.resolution << "\n";
            MeshMaterial ground;
            ground.baseColour = {.10, .17, .035, 1};
            if (library) {
                ground.baseColour = {1, 1, 1, 1};
                ground.colourTexture =
                    r.uploadTexture(readTexture(std::filesystem::path(OPENWORLD_VEGETATION_LIBRARY_DIR) /
                                                (o.rgba ? "ground_rgba.dds" : "ground.dds")));
                for (auto &v : fixture->ground.vertices)
                    v.uv *= .25f;
            }
            ground.doubleSided = true;
            auto groundMaterial = r.createMaterial(ground);
            MeshMaterial black = ground;
            black.baseColour = {0, 0, 0, 1};
            black.unlit = true;
            black.colourTexture = {};
            auto blackMaterial = r.createMaterial(black);
            MeshInstance groundInstance;
            groundInstance.mesh = r.uploadMesh(fixture->ground);
            groundInstance.material = groundMaterial;
            auto groundHandle = r.createInstance(groundInstance);
            if (o.canopy) {
                MeshInstance canopy;
                canopy.mesh = r.uploadMesh(makeQuad());
                canopy.material = groundMaterial;
                canopy.position.local = {-7.5, 10, o.side * .15 - 6};
                canopy.rotation = glm::angleAxis(-1.57079632679f, glm::vec3(1, 0, 0));
                canopy.scale = {4, 4, 1};
                r.createInstance(canopy);
            }
            SceneSettings scene;
            scene.terrainVisible = false;
            scene.output.ev100 = o.exposure;
            scene.vegetation.depthPrepass = o.prepass;
            scene.visibility = VisibilityMode::Indirect;
            scene.vegetation.density = o.density;
            scene.vegetation.shadowDistance = o.grassShadowDistance;
            scene.gtao.mode = o.gtao;
            scene.taa.enabled = true;
            scene.shadows.mode = ShadowMode::CSM;
            // Seed upload and query slots before calibration or paired measurements.
            auto first = Clock::now();
            for (int i = 0; i < 3; ++i)
                r.render(cameraAt("close", 0, o.side, rolling), scene);
            r.waitIdle();
            meta << "initial_upload_and_three_frames_ms="
                 << std::chrono::duration<double, std::milli>(Clock::now() - first).count() << '\n';
            groundInstance.material = blackMaterial;
            r.updateInstance(groundHandle, groundInstance);
            std::array<Coverage, 3> reference;
            const std::array<std::string, 3> views = {"close", "grazing", "aerial"};
            for (uint32_t v = 0; v < 3; ++v) {
                reference[v] = coverage(r, cameraAt(views[v], 0, o.side, rolling), scene);
                coverageCsv << (rolling ? "rolling," : "flat,") << views[v] << ",blades,1,1,"
                            << reference[v].fraction << ",0\n";
            }
            float bestWidth = 1, bestDensity = 1;
            double bestError = 1e9;
            if (!library)
                for (float width : {.75f, 1.f, 1.25f, 1.5f})
                    for (float density : {.5f, .75f, 1.f}) {
                        auto calibrated = scene;
                        calibrated.vegetation.representation = 1;
                        calibrated.vegetation.cardWidth = width;
                        calibrated.vegetation.cardDensity = density;
                        double error = 0;
                        for (uint32_t v = 0; v < 3; ++v) {
                            auto c = coverage(r, cameraAt(views[v], 0, o.side, rolling), calibrated);
                            double e = 0;
                            for (uint32_t tile = 0; tile < 16; ++tile)
                                e += std::abs(c.tiles[tile] - reference[v].tiles[tile]) / 16;
                            error += e;
                            coverageCsv << (rolling ? "rolling," : "flat,") << views[v] << ",cards," << width
                                        << ',' << density << ',' << c.fraction << ',' << e << '\n';
                        }
                        if (error < bestError) {
                            bestError = error;
                            bestWidth = width;
                            bestDensity = density;
                        }
                    }
            if (library)
                bestError = 0;
            meta << "calibrated_card_width=" << bestWidth << "\ncalibrated_card_density=" << bestDensity
                 << "\nmean_coverage_tile_error=" << bestError / 3 << '\n';
            if (!library)
                report << "- " << (rolling ? "Rolling" : "Flat") << " fixture: card width " << bestWidth
                       << ", density multiplier " << bestDensity << ", mean absolute tile coverage error "
                       << bestError / 3 << ". "
                       << (bestError / 3 > .05 ? "Coverage match failed the 5 percentage-point gate; "
                                                 "inspect captures before comparing quality."
                                               : "Coverage is within the 5 percentage-point aggregate "
                                                 "gate; silhouettes still require review.")
                       << "\n";
            groundInstance.material = groundMaterial;
            r.updateInstance(groundHandle, groundInstance);
            meta << "calibration_effective_taa=" << r.postStats().taa
                 << "\neffective_material_filtering=" << r.materialStats().effectiveAnisotropy << "\n";
            for (const auto &sequence : sequences) {
                if ((sequence == "rolling") != rolling)
                    continue;
                for (uint32_t representation = 0; representation < 3; ++representation) {
                    if (o.representation != "all" && representations[representation] != o.representation)
                        continue;
                    for (uint32_t comparison = 0; comparison < (representation == 0 ? 1u : 2u);
                         ++comparison) {
                        SceneSettings base = scene;
                        base.vegetation.representation = representation;
                        if (comparison) {
                            base.vegetation.cardWidth = bestWidth;
                            base.vegetation.cardDensity = bestDensity;
                        }
                        std::string comparisonName = comparison ? "coverage" : "count";
                        uint32_t variantCount = o.variants ? 7 : 1;
                        for (uint32_t variant = 0; variant < variantCount; ++variant) {
                            auto settings = base;
                            const char *names[] = {"default", "shadows_off", "taa_off", "strong_wind",
                                                   "low_sun", "prepass",     "gtao"};
                            if (o.profile == "v2")
                                names[5] = "no_prepass";
                            if (variant == 1)
                                settings.shadows.mode = ShadowMode::Off;
                            if (variant == 2)
                                settings.taa.enabled = false;
                            if (variant == 3)
                                settings.vegetation.wind = 2;
                            if (variant == 4)
                                settings.lighting.sunDirection = {-.8f, .15f, .4f};
                            if (variant == 5) {
                                settings.opaqueDepthPrepass = o.profile != "v2";
                                settings.vegetation.depthPrepass = false;
                            }
                            if (variant == 6)
                                settings.gtao.mode = GtaoMode::Half;
                            for (uint32_t repeat = 0; repeat < o.repeats; ++repeat) {
                                std::array<double, 2> pairCpu{}, pairGpu{}, pairP95{};
                                bool pairTiming = true;
                                for (uint32_t order = 0; order < 2; ++order) {
                                    bool enabled = ((order + repeat) % 2) != 0;
                                    settings.vegetation.enabled = enabled;
                                    std::cout << sequence << ' ' << representations[representation] << ' '
                                              << comparisonName << ' ' << names[variant] << " repeat "
                                              << repeat << " enabled " << enabled << std::endl;
                                    auto result = run(r, settings, sequence, o, rolling);
                                    std::vector<double> cpu, gpu, prepare, veg;
                                    uint64_t allocations = 0;
                                    bool timing = true;
                                    std::ostringstream key;
                                    key << sequence << ',' << representations[representation] << ','
                                        << comparisonName << ',' << names[variant] << ',' << repeat << ','
                                        << enabled;
                                    for (const auto &sample : result.samples) {
                                        const auto &v = sample.vegetation;
                                        if (v.overflow)
                                            throw std::runtime_error(
                                                "Vegetation overflow invalidates benchmark");
                                        cpu.push_back(sample.cpu);
                                        prepare.push_back(v.preparationMilliseconds);
                                        bool available =
                                            sample.frameTiming && (!enabled || v.timingsAvailable);
                                        timing &= available;
                                        if (available) {
                                            gpu.push_back(sample.frameGpu);
                                            veg.push_back(v.selectionMilliseconds + v.depthMilliseconds +
                                                          v.colourMilliseconds + v.shadowMilliseconds);
                                        }
                                        allocations += sample.allocations;
                                        raw << key.str() << ',' << sample.id << ',' << sample.cpu << ','
                                            << (available ? sample.frameGpu : -1) << ','
                                            << v.preparationMilliseconds << ',' << v.selectionMilliseconds
                                            << ',' << v.depthMilliseconds << ',' << v.colourMilliseconds
                                            << ',' << v.shadowMilliseconds << ',' << v.visible << ','
                                            << v.triangles << ',' << v.casters << ',' << v.overflow << ','
                                            << v.allocatedBytes << ',' << sample.allocations << ','
                                            << available << ',' << v.casterCandidates << ','
                                            << v.selectionGroups << '\n';
                                    }
                                    summary
                                        << key.str() << ',' << percentile(cpu, .5) << ','
                                        << percentile(cpu, .95) << ',' << (timing ? percentile(gpu, .5) : -1)
                                        << ',' << (timing ? percentile(gpu, .95) : -1) << ','
                                        << percentile(prepare, .5) << ',' << percentile(prepare, .95) << ','
                                        << (timing ? percentile(veg, .5) : -1) << ','
                                        << (timing ? percentile(veg, .95) : -1) << ',' << allocations << ','
                                        << r.vegetationStats().allocatedBytes << '\n';
                                    pairCpu[enabled] = percentile(cpu, .5);
                                    pairGpu[enabled] = percentile(gpu, .5);
                                    pairP95[enabled] = percentile(gpu, .95);
                                    pairTiming &= timing;
                                    meta << "effective_run=" << key.str() << "; taa=" << r.postStats().taa
                                         << "; shadows=" << uint32_t(r.shadowStats().effective)
                                         << "; gtao=" << uint32_t(r.gtaoStats().effective) << '\n';
                                    if (!timing)
                                        meta << "timing_unavailable=" << key.str() << '\n';
                                }
                                pairs << sequence << ',' << representations[representation] << ','
                                      << comparisonName << ',' << names[variant] << ',' << repeat << ','
                                      << pairCpu[1] - pairCpu[0] << ','
                                      << (pairTiming ? pairGpu[1] - pairGpu[0] : -1) << ','
                                      << (pairTiming ? pairP95[1] - pairP95[0] : -1) << ',' << pairTiming
                                      << '\n';
                            }
                        }
                        if (o.captures) {
                            base.vegetation.time = 0;
                            r.resetTemporalHistory();
                            for (uint32_t f = 0; f < 16; ++f)
                                r.render(cameraAt(sequence, 0, o.side, rolling), base);
                            std::string prefix =
                                sequence + "_" + representations[representation] + "_" + comparisonName;
                            capture(r, o.output / (prefix + ".png"));
                            if (sequence == "moving" || sequence == "rolling")
                                motionCapture(r, base, sequence, o, rolling, prefix);
                        }
                    }
                }
            }
            if (r.validationErrors())
                throw std::runtime_error("Vulkan validation errors");
        }
        report << "\nTarget GPU "
               << (target ? "detected; verify Release/full settings and paired "
                            "measurements before acceptance."
                          : "unavailable: RTX 3060 performance acceptance remains "
                            "pending.")
               << "\n\nStreaming, production terrain attachment "
                  "and interaction costs remain outside this fixed-fixture probe. Cards are not "
                  "adopted automatically from coverage or timing alone.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

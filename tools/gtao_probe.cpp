#include "../tests/gtao_fixtures.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
using namespace ow;
namespace {
double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[size_t(p * (values.size() - 1))];
}
void scalar(const std::filesystem::path &path, uint32_t w, uint32_t h, const std::vector<float> &values,
            float scale = 1) {
    std::vector<glm::vec4> pixels;
    pixels.reserve(values.size());
    for (auto x : values)
        pixels.emplace_back(glm::vec3(x * scale), 1);
    ao_fixture::writePpm(path, w, h, pixels);
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument("Usage: gtao_probe OUTPUT_DIR [--quick] [--frames N] [--width N "
                                        "--height N] [--require-validation] [--wide] [--streaming] [--far]");
        std::filesystem::path output = argv[1];
        bool quick = false, validation = false, wide = false, streaming = false, distant = false;
        uint32_t width = 1920, height = 1080, frames = 120;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--quick")
                quick = true;
            else if (a == "--require-validation")
                validation = true;
            else if (a == "--wide")
                wide = true;
            else if (a == "--streaming")
                streaming = true;
            else if (a == "--far")
                distant = true;
            else if ((a == "--frames" || a == "--width" || a == "--height") && i + 1 < argc) {
                auto n = std::stoul(argv[++i]);
                if (!n || n > 16384)
                    throw std::invalid_argument("Invalid count/extent");
                if (a == "--frames")
                    frames = n;
                else if (a == "--width")
                    width = n;
                else
                    height = n;
            } else
                throw std::invalid_argument("Unknown probe argument");
        }
        if (distant && streaming)
            throw std::invalid_argument("--far uses mesh fixtures; do not combine with --streaming");
        if (quick) {
            width = 320;
            height = 180;
            if (frames == 120)
                frames = 8;
        }
        std::filesystem::create_directories(output);
        std::ofstream meta(output / "metadata.txt"), csv(output / "timings.csv");
        meta << "extent=" << width << 'x' << height << "\nframes=" << frames << "\nstreaming=" << streaming
             << "\nfar=" << distant << "\nradius_m=1\nwarmup=16\n";
#ifdef NDEBUG
        meta << "build=Release\n";
#else
        meta << "build=Debug\n";
#endif
        csv << "fixture,mode,cpu_median_ms,cpu_p95_ms,scene_median_ms,scene_p95_ms,"
               "depth_median_ms,depth_p95_ms,prepare_median_ms,prepare_p95_ms,"
               "evaluate_median_ms,evaluate_p95_ms,filter_median_ms,filter_p95_ms,"
               "ao_bytes,compact,timestamps_available\n";
        csv << std::setprecision(8);
        for (bool corner : {false, true}) {
            RendererOptions o;
            o.ui = false;
            o.validation = validation;
            o.requireValidation = validation;
            o.shaderDirectory = OPENWORLD_SHADER_DIR;
            o.width = width;
            o.height = height;
            o.forceWideGtao = wide;
            o.terrainGrid = {quick ? 32u : 128u, .5f};
            o.terrainCapacity = o.terrainGrid.cells;
            o.lod = {true, quick ? 8u : 32u, 64};
            o.streaming.enabled = o.streaming.hierarchical = streaming;
            o.streaming.maximumDistance = quick ? 128 : 8192;
            o.streaming.cacheDirectory = output / (corner ? "relief-cache" : "plain-cache");
            o.streaming.diagnosticFrameSeconds = 1. / 120;
            Renderer r(o);
            meta << "device=" << r.deviceName() << "\nvalidation=" << r.validationEnabled() << '\n';
            ao_fixture::Scene fixture(r);
            if (!corner)
                fixture.removeWall();
            auto camera = ao_fixture::camera();
            auto s = ao_fixture::settings();
            if (distant) {
                // Preserve screen coverage while moving every visible surface beyond
                // the 200 m AO fade. This exposes work spent on white visibility.
                fixture.floor.scale = fixture.wall.scale = glm::vec3(1000);
                r.updateInstance(fixture.floorInstance, fixture.floor);
                if (corner)
                    r.updateInstance(fixture.wallInstance, fixture.wall);
                camera.local *= 1000;
            }
            if (streaming) {
                r.removeInstance(fixture.floorInstance);
                r.removeInstance(fixture.wallInstance);
                s.terrainVisible = true;
                s.viewRadius = quick ? 2 : 64;
                TerrainSettings terrain;
                terrain.heightScale = corner ? 40 : 0;
                terrain.wavelength = 256;
                r.setTerrainSettings(terrain);
                camera.local = {o.terrainGrid.extent() * .5, corner ? 65. : 3., o.terrainGrid.extent() * .5};
                camera.pitch = -.6f;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
                s.gtao.mode = GtaoMode::Off;
                bool settled = false;
                uint32_t quiet = 0;
                while (std::chrono::steady_clock::now() < deadline) {
                    r.render(camera, s);
                    const auto &st = r.terrainStreamingStats();
                    quiet = !st.pending && !st.detailPending && !st.ioJobs ? quiet + 1 : 0;
                    if (quiet >= 16) {
                        settled = true;
                        break;
                    }
                }
                if (!settled)
                    throw std::runtime_error("Streaming fixture did not settle within 120 seconds");
            }
            std::vector<glm::vec2> flightHeights;
            if (streaming)
                flightHeights = r.readStreamedTileHeights({0, 0, 0});
            const auto followSurface = [&](Camera &c) {
                if (!streaming)
                    return;
                const auto n = o.terrainGrid.cells;
                double x = c.local.x / o.terrainGrid.spacing, z = c.local.z / o.terrainGrid.spacing;
                uint32_t ix = std::min(uint32_t(x), n - 1), iz = std::min(uint32_t(z), n - 1);
                float fx = float(x - ix), fz = float(z - iz);
                const auto at = [&](uint32_t x, uint32_t z) {
                    return flightHeights[size_t(z) * (n + 1) + x].y;
                };
                c.local.y = glm::mix(glm::mix(at(ix, iz), at(ix + 1, iz), fx),
                                     glm::mix(at(ix, iz + 1), at(ix + 1, iz + 1), fx), fz) +
                            3;
            };
            followSurface(camera);
            const char *names[] = {"off", "off_prepass", "full", "half"};
            for (uint32_t mode = 0; mode < 4; ++mode) {
                s.gtao.mode = mode < 2 ? GtaoMode::Off : mode == 2 ? GtaoMode::Full : GtaoMode::Half;
                s.opaqueDepthPrepass = mode == 1;
                std::array<std::vector<double>, 6> times;
                for (auto &v : times)
                    v.reserve(frames);
                for (uint32_t n = 0; n < 16 + frames; ++n) {
                    auto c = camera;
                    c.yaw += .03f * std::sin(float(n) * .07f);
                    c.local.x += .25 * std::sin(double(n) * .03);
                    c.local.z += .25 * std::sin(double(n) * .05);
                    followSurface(c);
                    auto start = std::chrono::steady_clock::now();
                    r.render(c, s);
                    auto elapsed =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                            .count();
                    if (n < 16)
                        continue;
                    times[0].push_back(elapsed);
                    const auto &a = r.gtaoStats();
                    times[1].push_back(r.renderingStats().sceneMilliseconds);
                    times[2].push_back(r.visibilityStats().depthMilliseconds);
                    times[3].push_back(a.timingsAvailable ? a.prepareMilliseconds : 0);
                    times[4].push_back(a.timingsAvailable ? a.evaluateMilliseconds : 0);
                    times[5].push_back(a.timingsAvailable ? a.filterMilliseconds : 0);
                }
                if (!r.visibilityStats().candidates)
                    throw std::runtime_error("Probe has no drawable candidates");
                if (mode >= 2 && r.gtaoStats().effective == GtaoMode::Off)
                    throw std::runtime_error("Requested probe GTAO unavailable");
                std::string fixtureName = streaming ? (corner ? "relief" : "flat")
                                          : distant ? (corner ? "far_corner" : "far_plane")
                                                    : (corner ? "corner" : "plane");
                std::string base = fixtureName + "_" + names[mode];
                csv << fixtureName << ',' << names[mode];
                for (auto &v : times)
                    csv << ',' << percentile(v, .5) << ',' << percentile(v, .95);
                csv << ',' << r.gtaoStats().allocatedBytes << ',' << r.gtaoStats().compact << ','
                    << (r.renderingStats().timingsAvailable && (mode < 2 || r.gtaoStats().timingsAvailable))
                    << '\n';
                csv.flush();
                r.render(camera, s);
                r.capture(output / (base + ".ppm"));
                if (mode >= 2) {
                    auto a = r.readGtaoSnapshot();
                    ao_fixture::writePpm(output / (base + "_ao.ppm"), a.width, a.height, a.pixels);
                    scalar(output / (base + "_depth.ppm"), a.workWidth, a.workHeight, a.depth, 1.f / 20);
                    scalar(output / (base + "_raw.ppm"), a.workWidth, a.workHeight, a.raw);
                    for (auto d : {GtaoDiagnostic::Normals, GtaoDiagnostic::Edges}) {
                        s.gtao.diagnostic = d;
                        r.render(camera, s);
                        r.capture(output /
                                  (base + (d == GtaoDiagnostic::Normals ? "_normals.ppm" : "_edges.ppm")));
                    }
                    s.gtao.diagnostic = GtaoDiagnostic::Shaded;
                }
                if (r.validationErrors())
                    throw std::runtime_error("Probe validation errors");
            }
        }
        std::cout << "GTAO probe written to " << output << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

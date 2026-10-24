#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "review_view.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
using namespace ow;
namespace {
using Clock = std::chrono::steady_clock;
double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[size_t(p * (v.size() - 1))];
}
// Small owned terrain-shaped mesh. Each strip is an independent draw, so hills
// can occlude other strips; a single valley-sized bound would never be
// rejected.
MeshData strip(bool valley, float x0, float x1, float z0, float z1) {
    auto height = [&](float x, float z) {
        return valley ? 26.f * std::exp(-std::pow((x - 28.f - 12.f * std::sin(z * .035f)) / 12.f, 2.f)) +
                            20.f * std::exp(-std::pow((x + 28.f - 9.f * std::sin(z * .04f)) / 13.f, 2.f))
                      : 0.f;
    };
    MeshData data;
    constexpr uint32_t n = 8;
    for (uint32_t z = 0; z <= n; ++z)
        for (uint32_t x = 0; x <= n; ++x) {
            float px = x0 + (x1 - x0) * x / n, pz = z0 + (z1 - z0) * z / n;
            float dx = (height(px + .1f, pz) - height(px - .1f, pz)) / .2f;
            float dz = (height(px, pz + .1f) - height(px, pz - .1f)) / .2f;
            auto normal = glm::normalize(glm::vec3(-dx, 1, -dz));
            auto tangent = glm::normalize(glm::vec3(1, dx, 0));
            data.vertices.push_back({{px, height(px, pz), pz}, normal, {tangent, 1}, {px * .1f, pz * .1f}});
        }
    for (uint32_t z = 0; z < n; ++z)
        for (uint32_t x = 0; x < n; ++x) {
            uint32_t a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            data.indices.insert(data.indices.end(), {a, d, c, a, b, d});
        }
    return data;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            throw std::invalid_argument("Usage: visibility_probe OUTPUT_DIR [--quick] [--frames N] [--width "
                                        "N --height N] [--require-validation] [--fixed] [--streaming]");
        std::filesystem::path output = argv[1];
        bool quick = false, validation = false, fixed = false, streaming = false;
        std::optional<ReviewView> review;
        double flightSpeed = 30;
        std::filesystem::path shaders = OPENWORLD_SHADER_DIR;
        uint32_t frames = 120, width = 1920, height = 1080;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--review-view" && i + 1 < argc)
                review = loadReviewView(argv[++i]);
            else if (arg == "--flight-speed" && i + 1 < argc) {
                flightSpeed = std::stod(argv[++i]);
                if (!std::isfinite(flightSpeed) || flightSpeed < 0 || flightSpeed > 10000)
                    throw std::invalid_argument("Invalid flight speed");
            } else if (arg == "--shader-directory" && i + 1 < argc)
                shaders = argv[++i];
            else if (arg == "--quick")
                quick = true;
            else if (arg == "--require-validation")
                validation = true;
            else if (arg == "--fixed")
                fixed = true;
            else if (arg == "--streaming")
                streaming = true;
            else if ((arg == "--frames" || arg == "--width" || arg == "--height") && i + 1 < argc) {
                auto n = std::stoul(argv[++i]);
                if (!n || n > 16384)
                    throw std::invalid_argument("Invalid probe size/count");
                if (arg == "--frames")
                    frames = uint32_t(n);
                else if (arg == "--width")
                    width = uint32_t(n);
                else
                    height = uint32_t(n);
            } else
                throw std::invalid_argument("Unknown visibility probe argument");
        }
        if (quick) {
            width = 320;
            height = 180;
            if (frames == 120)
                frames = 8;
        }
        if (review) {
            streaming = review->startup.streaming.enabled;
            width = review->startup.width;
            height = review->startup.height;
        }
        std::filesystem::create_directories(output);
        std::ofstream metadata(output / "metadata.txt");
        metadata << "width=" << width << "\nheight=" << height << "\nsamples=" << frames
                 << "\nquick=" << quick << "\nstreaming=" << streaming << "\nworkload="
                 << (streaming ? "procedural terrain flight" : "owned analytic valley/plain mesh flight")
                 << "\nreview_view=" << bool(review) << "\nflight_speed=" << flightSpeed << "\n";
#ifdef NDEBUG
        metadata << "build=Release\n";
#else
        metadata << "build=Debug\n";
#endif
        std::ofstream csv(output / "timings.csv");
        csv << "landscape,mode,prepass,cpu_median_ms,cpu_p95_ms,scene_median_ms,"
               "scene_p95_ms,depth_median_ms,"
               "depth_p95_ms,pyramid_median_ms,pyramid_p95_ms,cull_median_ms,cull_"
               "p95_ms,colour_median_ms,"
               "colour_p95_ms,candidates,submitted,frustum,occluded,visibility_"
               "bytes,timestamps,completed_frame,completed_mode,depth_terrain_"
               "triangles,depth_mesh_triangles,colour_terrain_triangles,colour_"
               "mesh_triangles\n";
        csv << std::setprecision(8);
        for (bool valley : {false, true}) {
            if (review && valley)
                break;
            RendererOptions o;
            o.ui = false;
            o.validation = validation;
            o.requireValidation = validation;
            o.shaderDirectory = shaders;
            o.width = width;
            o.height = height;
            o.forceFixedIndirect = fixed;
            o.terrainGrid = {quick ? 32u : 128u, .5f};
            o.terrainCapacity = o.terrainGrid.cells;
            o.lod = {true, quick ? 8u : 32u, 64};
            o.streaming.enabled = o.streaming.hierarchical = streaming;
            o.streaming.maximumDistance = quick ? 128 : 8192;
            o.streaming.cacheDirectory = output / (valley ? "valley-cache" : "plain-cache");
            o.streaming.diagnosticFrameSeconds = 1. / 120.;
            if (review)
                applyReviewStartup(o, *review);
            Renderer r(o);
            metadata << "device=" << r.deviceName() << "\nvalidation=" << r.validationEnabled() << "\n";
            SceneSettings scene;
            scene.shadows.mode = ow::ShadowMode::Off;
            scene.gtao.mode = ow::GtaoMode::Off;
            scene.terrainVisible = streaming;
            scene.viewRadius = quick ? 2 : 64;
            TerrainSettings terrain;
            terrain.heightScale = valley ? 400 : 0;
            terrain.wavelength = 1024;
            r.setTerrainSettings(terrain);
            std::vector<MeshHandle> geometry;
            MeshMaterial material;
            material.baseColour = {.3f, .45f, .2f, 1};
            material.doubleSided = true;
            auto mat = r.createMaterial(material);
            if (!streaming && !review) {
                for (int z = 0; z < 12; ++z)
                    for (int x = 0; x < 8; ++x) {
                        auto mesh = r.uploadMesh(strip(valley, float(x * 16 - 64), float(x * 16 - 48),
                                                       float(-z * 16), float(-z * 16 - 16)));
                        geometry.push_back(mesh);
                        MeshInstance instance;
                        instance.mesh = mesh;
                        instance.material = mat;
                        r.createInstance(instance);
                    }
                auto sphere = r.uploadMesh(makeSphere(6, 8));
                geometry.push_back(sphere);
                for (int z = 0; z < 20; ++z)
                    for (int x = 0; x < 16; ++x) {
                        MeshInstance instance;
                        instance.mesh = sphere;
                        instance.material = mat;
                        instance.position.local = {double(x * 6 - 45), 2, double(-z * 8)};
                        instance.scale = glm::vec3(2);
                        r.createInstance(instance);
                    }
            }
            Camera camera;
            camera.local = streaming
                               ? glm::dvec3(o.terrainGrid.extent() * .5, 120, o.terrainGrid.extent() * .5)
                               : glm::dvec3(0, 8, 16);
            camera.pitch = streaming ? -.3f : -.1f;
            if (review) {
                camera = review->camera;
                scene = review->scene;
                terrain = review->terrain;
                r.requestTerrainBatch(terrainNeighbourhood({review->terrainOrigin, terrain}, !streaming));
                r.setTerrainLodReviewState(review->lod);
            }
            std::vector<glm::vec2> flightHeights;
            if (streaming) {
                uint32_t quiet = 0;
                auto start = Clock::now();
                while (quiet < 60) {
                    r.render(camera, scene);
                    const auto &t = r.terrainStreamingStats();
                    quiet = !t.pending && !t.detailPending && !t.ioJobs ? quiet + 1 : 0;
                    if (Clock::now() - start > std::chrono::seconds(120))
                        throw std::runtime_error("Streaming probe failed to settle in 120 seconds");
                }
            }
            // Read once outside measurement; keep this flight above the surface
            // rather than accidentally benchmarking a sky-only high-altitude view.
            if (streaming && !review)
                flightHeights = r.readStreamedTileHeights({0, 0, 0});
            const auto followSurface = [&](Camera &c) {
                if (!streaming || review)
                    return;
                const auto n = o.terrainGrid.cells;
                const double x = c.local.x / o.terrainGrid.spacing, z = c.local.z / o.terrainGrid.spacing;
                const auto ix = std::min(uint32_t(x), n - 1), iz = std::min(uint32_t(z), n - 1);
                const float fx = float(x - ix), fz = float(z - iz);
                const auto at = [&](uint32_t a, uint32_t b) {
                    return flightHeights[size_t(b) * (n + 1) + a].y;
                };
                c.local.y = glm::mix(glm::mix(at(ix, iz), at(ix + 1, iz), fx),
                                     glm::mix(at(ix, iz + 1), at(ix + 1, iz + 1), fx), fz) +
                            8;
            };
            followSurface(camera);
            const auto initial = camera;
            for (uint32_t mode = 0; mode < 4; ++mode) {
                scene.visibility = mode == 0   ? VisibilityMode::Direct
                                   : mode == 3 ? VisibilityMode::HiZ
                                               : VisibilityMode::Indirect;
                scene.opaqueDepthPrepass = mode >= 2;
                camera = initial;
                if (review)
                    r.setTerrainLodReviewState(review->lod);
                for (int i = 0; i < 16; ++i)
                    r.render(camera, scene);
                std::array<std::vector<double>, 6> values;
                for (auto &v : values)
                    v.reserve(frames);
                for (uint32_t i = 0; i < frames; ++i) {
                    // Same deterministic path for every mode, including warm GPU history.
                    camera = initial;
                    const double travel = double(i) * flightSpeed / 120;
                    camera.local.z -=
                        streaming && !review ? std::min(travel, o.terrainGrid.extent() * .4) : travel;
                    if (!review) {
                        camera.local.x += 4 * std::sin(double(i) * .02);
                        camera.yaw = .15f * std::sin(float(i) * .01f);
                    } else
                        camera.update({}, 0, o.terrainGrid);
                    followSurface(camera);
                    auto start = Clock::now();
                    r.render(camera, scene);
                    values[0].push_back(
                        std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                    const auto &v = r.visibilityStats();
                    values[1].push_back(r.renderingStats().sceneMilliseconds);
                    values[2].push_back(v.depthMilliseconds);
                    values[3].push_back(v.pyramidMilliseconds);
                    values[4].push_back(v.cullingMilliseconds);
                    values[5].push_back(v.colourMilliseconds);
                }
                const auto &v = r.visibilityStats();
                if (!v.candidates)
                    throw std::runtime_error("Visibility probe has no drawable candidates");
                csv << (valley ? "valley" : "plain") << ',' << mode << ',' << scene.opaqueDepthPrepass;
                for (const auto &sample : values)
                    csv << ',' << percentile(sample, .5) << ',' << percentile(sample, .95);
                csv << ',' << v.candidates << ',' << v.submitted << ',' << v.frustumRejected << ','
                    << v.occlusionRejected << ',' << v.allocatedBytes << ',' << v.timingsAvailable << ','
                    << v.completedFrame << ',' << uint32_t(v.completedMode) << ',' << v.depthTriangles.terrain
                    << ',' << v.depthTriangles.meshes << ',' << v.colourTriangles.terrain << ','
                    << v.colourTriangles.meshes << '\n';
                csv.flush();
                r.capture(output / ((valley ? std::string("valley-") : std::string("plain-")) +
                                    std::to_string(mode) + ".ppm"));
                if (r.validationErrors())
                    throw std::runtime_error("Visibility probe Vulkan validation failed");
                std::cout << (valley ? "valley" : "plain") << " mode " << mode << ": scene "
                          << percentile(values[1], .5) << " ms, occluded " << v.occlusionRejected << '\n';
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

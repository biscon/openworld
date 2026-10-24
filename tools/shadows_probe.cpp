#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "review_view.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
using namespace ow;
int main(int argc, char **argv) {
    try {
        std::filesystem::path out = argc > 1 ? argv[1] : "/tmp/shadows-review";
        bool quick = false, moving = false, lowSun = false, flight = false, validation = true;
        uint32_t captureEvery = 0, shadowResolution = 2048;
        double viewDistance = 8192;
        std::string scene = "plain";
        std::filesystem::path shaders = OPENWORLD_SHADER_DIR;
        float yaw = 0, flightSpeed = 100;
        std::optional<ReviewView> review;
        uint32_t frames = 120, width = 1920, height = 1080;
        uint32_t extraCasters = 0;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--review-view" && i + 1 < argc)
                review = loadReviewView(argv[++i]);
            else if (a == "--flight-speed" && i + 1 < argc) {
                flightSpeed = std::stof(argv[++i]);
                flight = true;
                if (!std::isfinite(flightSpeed) || flightSpeed < 0 || flightSpeed > 10000)
                    throw std::invalid_argument("Invalid flight speed");
            } else if (a == "--quick") {
                quick = true;
                width = 320;
                height = 180;
                frames = 12;
            } else if (a == "--moving")
                moving = true;
            else if (a == "--flight")
                flight = true;
            else if (a == "--view-distance" && i + 1 < argc)
                viewDistance = std::stod(argv[++i]);
            else if (a == "--no-validation")
                validation = false;
            else if (a == "--capture-every" && i + 1 < argc)
                captureEvery = uint32_t(std::stoul(argv[++i]));
            else if (a == "--shadow-resolution" && i + 1 < argc)
                shadowResolution = uint32_t(std::stoul(argv[++i]));
            else if (a == "--low-sun")
                lowSun = true;
            else if (a == "--scene" && i + 1 < argc)
                scene = argv[++i];
            else if (a == "--width" && i + 1 < argc)
                width = uint32_t(std::stoul(argv[++i]));
            else if (a == "--height" && i + 1 < argc)
                height = uint32_t(std::stoul(argv[++i]));
            else if (a == "--frames" && i + 1 < argc)
                frames = uint32_t(std::stoul(argv[++i]));
            else if (a == "--casters" && i + 1 < argc)
                extraCasters = uint32_t(std::stoul(argv[++i]));
            else if (a == "--shader-directory" && i + 1 < argc)
                shaders = argv[++i];
            else if (a == "--yaw" && i + 1 < argc)
                yaw = std::stof(argv[++i]);
            else
                throw std::invalid_argument("Unknown probe option");
        }
        if (scene != "plain" && scene != "valley" && scene != "streaming" && scene != "mountains")
            throw std::invalid_argument("Scene must be plain, valley, streaming or mountains");
        const bool terrainScene =
            review ? review->scene.terrainVisible : scene == "streaming" || scene == "mountains";
        if (!std::isfinite(viewDistance) || viewDistance < 128 || viewDistance > 16384)
            throw std::invalid_argument("View distance must be 128..16384 metres");
        if (!frames)
            throw std::invalid_argument("Probe requires frames");
        std::filesystem::create_directories(out);
        auto percentile = [](std::vector<double> v, double p) {
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, size_t(p * (v.size() - 1)))];
        };
        std::ofstream csv(out / "timings.csv");
        csv << "mode,scene,moving,flight,low_sun,width,height,cpu_median_ms,cpu_"
               "p95_ms,prepare_p95_ms,raster_"
               "p95_ms,gpu_median_ms,gpu_p95_ms,shadow_bytes,terrain_shadow_bytes,"
               "casters,triangles,overflow,"
               "incomplete\n";
        std::vector<double> offGpu;
        for (uint32_t mode = 0; mode < 2; ++mode) {
            const std::string name = mode ? "csm" : "off";
            RendererOptions o;
            o.ui = false;
            o.validation = validation;
            o.requireValidation = validation;
            o.shadowCapacity.resolution = quick ? 256 : shadowResolution;
            o.streaming.diagnosticFrameSeconds = 1.0 / 60;
            o.width = width;
            o.height = height;
            o.shaderDirectory = shaders;
            o.terrainGrid = {16, .5f};
            o.terrainCapacity = 16;
            o.shadowCapacity.casters = 4096;
            if (extraCasters) {
                if (extraCasters > 65000)
                    throw std::invalid_argument("At most 65000 additional probe casters");
                o.sceneCapacity.instances = extraCasters + 512;
                o.shadowCapacity.casters = 65536;
            }
            if (terrainScene) {
                o.terrainGrid = {quick ? 64u : 256u, .5f};
                o.terrainCapacity = o.terrainGrid.cells;
                o.lod = {true, quick ? 8u : 32u, 64};
                o.normalSubdivisions = quick ? 1 : 2;
                o.streaming.enabled = o.streaming.hierarchical = true;
                o.streaming.diskCache = false;
                o.streaming.maximumDistance = quick ? (scene == "mountains" ? 2048 : 512) : 16384;
                o.streaming.gpuBytes = (quick ? 256 : 512) * terrainMiB;
                o.streaming.hostBytes = 256 * terrainMiB;
                o.shadowCapacity.casters = 65536;
            }
            if (review) {
                applyReviewStartup(o, *review);
                width = o.width;
                height = o.height;
            }
            Renderer r(o);
            SceneSettings s;
            s.terrainVisible = terrainScene;
            s.viewRadius = terrainScene ? (quick ? (scene == "mountains" ? 64 : 4)
                                                 : int(viewDistance / o.terrainGrid.extent()))
                                        : 1;
            if (lowSun)
                s.lighting.sunDirection = glm::normalize(glm::vec3(-1, .03f, .2f));
            if (terrainScene) {
                TerrainSettings terrain;
                if (scene != "mountains") {
                    terrain.heightScale = quick ? 16 : 128;
                    terrain.wavelength = quick ? 128 : 512;
                }
                r.setTerrainSettings(terrain);
            }
            Camera c;
            c.yaw = yaw;
            c.local = {0, 10, 30};
            c.pitch = -.2;
            auto quad = r.uploadMesh(makeQuad()), sphere = r.uploadMesh(makeSphere(8, 16));
            MeshMaterial material;
            material.doubleSided = true;
            auto m = r.createMaterial(material);
            MeshInstance plane;
            plane.mesh = quad;
            plane.material = m;
            plane.position.local = {0, -2, -40};
            plane.scale = {100, 100, 1};
            plane.rotation = glm::angleAxis(-1.5707963f, glm::vec3(1, 0, 0));
            if (!terrainScene)
                r.createInstance(plane);
            if (scene == "valley")
                for (int side : {-1, 1}) {
                    MeshInstance slope = plane;
                    slope.position.local = {side * 90., 25, -40};
                    slope.rotation = glm::angleAxis(side * .45f, glm::vec3(0, 0, 1)) * plane.rotation;
                    r.createInstance(slope);
                }
            if (terrainScene) {
                c.local = quick ? glm::dvec3(0, 80, 20) : glm::dvec3(0, 180, 80);
                c.pitch = quick ? -.65f : -.45f;
                if (scene == "mountains") {
                    c.chunk = quick ? ChunkCoord{60, 32} : ChunkCoord{15, 8};
                    c.local = {0, 1000, 0};
                    c.pitch = -.4f;
                }
            }
            for (int i = 0; i < (review ? 0 : quick ? 32 : 256); ++i) {
                MeshInstance instance;
                instance.mesh = sphere;
                instance.material = m;
                instance.position.local = {(i % 16 - 8) * 3.0, 0, -double(i / 16) * 4};
                if (r.createInstance(instance).slot == ~0u)
                    throw std::runtime_error("Probe instance capacity exhausted");
            }
            if (review) {
                s = review->scene;
                c = review->camera;
                r.requestTerrainBatch(
                    terrainNeighbourhood({review->terrainOrigin, review->terrain}, !o.streaming.enabled));
                r.setTerrainLodReviewState(review->lod);
            }
            // Large command spans without overlapping all casters on every cascade.
            // Keep this scaling workload out of the routine correctness tests.
            for (uint32_t i = 0; i < extraCasters; ++i) {
                MeshInstance instance = plane;
                instance.position.local = {5000. + (i % 256) * 4., 0, -5000. - (i / 256) * 4.};
                instance.scale = {.25f, .25f, 1};
                if (r.createInstance(instance).slot == ~0u)
                    throw std::runtime_error("Probe instance capacity exhausted");
            }
            std::ofstream(out / "device.txt") << r.deviceName() << "\nValidation: " << validation
                                              << "\nSoftware Vulkan timings do not establish RTX 3060 "
                                                 "performance.\n";
            // Settle owned streaming coverage before timed runs, without changing
            // its normal generation/transfer budgets.
            bool settled = !o.streaming.enabled;
            if (o.streaming.enabled)
                for (uint32_t i = 0; i < 2000; ++i) {
                    r.render(c, s);
                    auto stats = r.terrainStreamingStats();
                    if (i > 24 && stats.pending == 0 && stats.shadowMissing == 0) {
                        settled = true;
                        break;
                    }
                }
            std::ofstream(out / "coverage.txt")
                << (settled ? "Settled coverage" : "Incomplete coverage after bounded warm-up") << '\n';
            s.shadows.mode = mode ? ShadowMode::CSM : ShadowMode::Off;
            if (!mode)
                std::ofstream(out / "settings.txt")
                    << "scene=" << scene << "\nwidth=" << width << "\nheight=" << height
                    << "\nterrain_cells=" << o.terrainGrid.cells
                    << "\nterrain_spacing=" << o.terrainGrid.spacing
                    << "\nnormal_subdivisions=" << o.normalSubdivisions << "\nview_radius=" << s.viewRadius
                    << "\nshadow_resolution=" << o.shadowCapacity.resolution
                    << "\nnear_distance=" << s.shadows.nearDistance
                    << "\nfar_distance_setting=" << s.shadows.farDistance << "\nframes=" << frames
                    << "\nflight_speed=" << flightSpeed << "\nreview_view=" << bool(review)
                    << "\nflight=" << flight << "\nmoving_sun=" << moving << "\nlow_sun=" << lowSun
                    << "\nvalidation=" << validation
                    << "\ndiagnostic_frame_seconds=" << o.streaming.diagnosticFrameSeconds << '\n';
            const Camera base = c;
            const auto baseSun = s.lighting.sunDirection;
            std::vector<double> cpu, prepare, raster, gpu;
            cpu.reserve(frames);
            prepare.reserve(frames);
            raster.reserve(frames);
            gpu.reserve(frames);
            std::ofstream trace(out / (name + "_frames.csv"));
            trace << "frame,cpu_ms,gpu_ms,prepare_ms,raster_ms,generation_ms,casters,"
                     "triangles,incomplete";
            for (uint32_t j = 0; j < shadowCascades; ++j)
                trace << ",cascade" << j << "_ms";
            trace << '\n';
            for (uint32_t i = 0; i < frames + 24; ++i) {
                const double t = double(i >= 24 ? i - 24 : 0) / 60;
                c = base;
                if (flight) {
                    c.local.x += t * flightSpeed;
                    c.local.z -= t * flightSpeed * .4;
                    c.yaw += float(.2 * std::sin(t * .3));
                    c.update({}, 0, o.terrainGrid);
                }
                if (moving)
                    s.lighting.sunDirection =
                        glm::normalize(glm::vec3(std::sin(t * .12), .25 + std::abs(std::cos(t * .12)), .3));
                else
                    s.lighting.sunDirection = baseSun;
                const auto start = std::chrono::steady_clock::now();
                r.render(c, s);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                        .count();
                if (i < 24)
                    continue;
                const auto stats = r.shadowStats();
                const auto rendering = r.renderingStats();
                if (!rendering.timingsAvailable || (mode && !stats.timingsAvailable))
                    throw std::runtime_error("GPU timestamps unavailable; cannot report shadow performance");
                const double total = rendering.frameMilliseconds;
                cpu.push_back(ms);
                prepare.push_back(mode ? stats.preparationMilliseconds : 0);
                raster.push_back(mode ? stats.totalMilliseconds : 0);
                gpu.push_back(total);
                uint64_t triangles = 0;
                for (auto n : stats.triangles)
                    triangles += n;
                trace << i - 24 << ',' << ms << ',' << total << ',' << prepare.back() << ',' << raster.back()
                      << ',' << r.terrainStreamingStats().generationMilliseconds << ','
                      << (mode ? stats.casters : 0) << ',' << (mode ? triangles : 0) << ','
                      << (mode ? stats.incomplete : 0);
                for (auto time : stats.rasterMilliseconds)
                    trace << ',' << (mode ? time : 0);
                trace << '\n';
                if (captureEvery && (i - 24) % captureEvery == 0)
                    r.capture(out / (name + "_" + std::to_string(i - 24) + ".ppm"));
            }
            r.capture(out / (name + ".ppm"));
            auto stats = r.shadowStats();
            uint64_t triangles = 0;
            for (auto n : stats.triangles)
                triangles += n;
            csv << name << ',' << scene << ',' << moving << ',' << flight << ',' << lowSun << ',' << width
                << ',' << height << ',' << percentile(cpu, .5) << ',' << percentile(cpu, .95) << ','
                << percentile(prepare, .95) << ',' << percentile(raster, .95) << ',' << percentile(gpu, .5)
                << ',' << percentile(gpu, .95) << ',' << stats.allocatedBytes << ','
                << r.terrainStreamingStats().shadowBytes << ',' << (mode ? stats.casters : 0) << ','
                << (mode ? triangles : 0) << ',' << (mode ? stats.overflow : 0) << ','
                << (mode ? stats.incomplete : 0) << '\n';
            if (!mode)
                offGpu = gpu;
            else {
                for (size_t i = 0; i < gpu.size(); ++i)
                    gpu[i] -= offGpu[i];
                std::ofstream(out / "incremental.txt")
                    << "Paired full-frame GPU delta (including generation): median " << percentile(gpu, .5)
                    << " ms; p95 " << percentile(gpu, .95)
                    << " ms. GPU metrics describe completed submissions, with the same "
                       "frame delay in each "
                       "run.\n";
            }
            if (r.validationErrors())
                throw std::runtime_error("Shadow probe validation errors");
        }
        std::cout << "Shadow probe: " << out << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

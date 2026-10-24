#include "review_view.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>

using namespace ow;
using Clock = std::chrono::steady_clock;

// Explicit, expensive review of a supplied bookmark. Not a routine test fixture.
int main(int argc, char **argv) {
    try {
        if (argc < 3 || argc > 6)
            throw std::invalid_argument(
                "Usage: terrain_ground_probe OUTPUT REVIEW [HEIGHT [DISTANCE [FPS]]]");
        const std::filesystem::path output = argv[1];
        std::filesystem::create_directories(output);
        auto view = loadReviewView(argv[2]);
        RendererOptions o;
        applyReviewStartup(o, view);
        o.ui = false;
        o.validation = o.requireValidation = true;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.height = argc > 3 ? std::stoul(argv[3]) : 1080;
        if (!o.height || o.height > 2160)
            throw std::invalid_argument("Height must be 1..2160");
        o.width = uint32_t(std::round(double(o.height) * view.startup.width / view.startup.height));
        o.renderHeight = o.height;
        const double distance = argc > 4 ? std::stod(argv[4]) : o.streaming.maximumDistance;
        const int fps = argc > 5 ? std::stoi(argv[5]) : 30;
        if (distance < 512 || distance > 16384 || fps < 0 || fps > 120)
            throw std::invalid_argument("Distance must be 512..16384 m, FPS 0..120 (0: static captures)");
        o.streaming.maximumDistance = distance;
        // Keep curvature, but omit the expensive distant bank in a reduced coverage run.
        if (distance < view.startup.streaming.maximumDistance) {
            o.streaming.horizonMaximumDistance = distance;
            o.streaming.horizonBytes = 0;
        }
        o.streaming.cacheDirectory = output / "cache";
        o.streaming.diagnosticFrameSeconds = 1. / (fps ? fps : 60);
        Renderer r(o);
        auto camera = view.camera;
        auto scene = view.scene;
        scene.viewRadius = int(distance / o.terrainGrid.extent());
        scene.freezeLod = false;
        scene.wireframe = scene.normalView = scene.lodColours = false;
        scene.shadows.mode = ShadowMode::Off;
        scene.gtao.mode = GtaoMode::Off;
        scene.taa.enabled = false;
        scene.bloom.intensity = 0;
        scene.opaqueDepthPrepass = true;
        r.setTerrainSettings(view.terrain);
        auto settle = [&] {
            const auto start = Clock::now();
            int quiet = 0, frame = 0;
            while (quiet < 12) {
                const auto &before = r.terrainStreamingStats();
                auto work = scene;
                if (frame++ % 30 && (before.pending || before.detailPending || before.ioJobs))
                    work.terrainVisible = false;
                r.render(camera, work);
                const auto &st = r.terrainStreamingStats();
                quiet = st.pending || st.detailPending || st.ioJobs ? 0 : quiet + 1;
                if (frame % 120 == 0)
                    std::cerr << "settling " << st.pending << " coverage, " << st.detailPending
                              << " detail\n";
                if (Clock::now() - start > std::chrono::seconds(360))
                    throw std::runtime_error("Ground review convergence deadline");
            }
        };
        settle();
        const uint64_t memory = r.terrainAllocatedBytes();
        std::ofstream info(output / "review.txt");
        info << std::setprecision(12) << "device=" << r.deviceName() << "\nresolution=" << r.renderWidth()
             << 'x' << r.renderHeight() << " coverage=" << distance << " fps=" << fps
             << "\ngpuBytes=" << r.terrainStreamingStats().gpuBytes
             << " hostBytes=" << r.terrainStreamingStats().hostBytes << '\n';
        auto capture = [&](const std::string &name) {
            r.render(camera, scene);
            r.capture(output / (name + ".ppm"));
        };
        // One bookmark, both quality policies, each useful surface diagnostic.
        for (bool quality : {false, true}) {
            scene.terrainLodQuality = quality;
            const std::string name = quality ? "quality" : "distance";
            scene.terrainMaterial.enabled = true;
            capture(name + "-textured");
            scene.terrainMaterial.enabled = false;
            capture(name + "-plain");
            scene.wireframe = true;
            capture(name + "-wireframe");
            scene.wireframe = false;
            info << name << " triangles=" << r.terrainRenderStats().triangles
                 << " replacementPixels=" << r.terrainRenderStats().maximumTransitionPixels
                 << " budgetDeferred=" << r.terrainRenderStats().budgetLimited
                 << " groundMissing=" << r.terrainRenderStats().protectedMissing << '\n';
        }
        scene.terrainMaterial.enabled = true;
        scene.freezeLod = true;
        r.render(camera, scene);
        view.camera = camera;
        view.scene = scene;
        view.lod = r.terrainLodReviewState();
        view.startup = o;
        saveReviewView(output / "settled.json", view);
        r.setTerrainLodReviewState(view.lod);
        r.render(camera, scene);
        if (r.terrainLodReviewState().patches != view.lod.patches)
            throw std::runtime_error("Frozen bookmark did not reproduce partition");
        scene.freezeLod = false;
        if (fps == 0) {
            info << "validationErrors=" << r.validationErrors() << '\n';
            if (r.validationErrors() || memory != r.terrainAllocatedBytes())
                throw std::runtime_error("Static ground replay validation/memory failure");
            std::cout << "Static ground review completed: " << output << '\n';
            return 0;
        }

        // Read fine terrain once, outside the timed path, to follow actual ground.
        std::map<std::pair<int64_t, int64_t>, std::vector<glm::vec2>> ground;
        for (auto t : r.readTerrainResidency())
            if (t.resident && t.key.level == 0)
                ground.emplace(std::make_pair(t.key.x, t.key.z), r.readStreamedTileHeights(t.key));
        auto height = [&](Camera c) {
            c.update({}, 0, o.terrainGrid);
            const auto it = ground.find({c.chunk.x, c.chunk.z});
            if (it == ground.end())
                throw std::runtime_error("Review path exceeds initial fine ground samples");
            double x = c.local.x / o.terrainGrid.spacing, z = c.local.z / o.terrainGrid.spacing;
            const auto ix = std::min(uint32_t(x), o.terrainGrid.cells - 1),
                       iz = std::min(uint32_t(z), o.terrainGrid.cells - 1);
            const float fx = float(x - ix), fz = float(z - iz);
            auto h = [&](uint32_t dx, uint32_t dz) {
                return it->second[(iz + dz) * (o.terrainGrid.cells + 1) + ix + dx][scene.eroded ? 1 : 0];
            };
            return fx >= fz ? h(0, 0) * (1 - fx) + h(1, 0) * (fx - fz) + h(1, 1) * fz
                            : h(0, 0) * (1 - fz) + h(1, 1) * fx + h(0, 1) * (fz - fx);
        };
        const auto startCamera = camera;
        const double clearance = std::max(1.8, camera.local.y - height(camera));
        info << "clearance=" << clearance << '\n';
        auto heading = glm::normalize(glm::dvec2(camera.forward().x, camera.forward().z));
        std::ofstream csv(output / "motion.csv");
        csv << "quality,speed,frame,offset,chunkX,chunkZ,cpuMs,gpuMs,triangles,transitionPixels,"
               "groundMissing,budgetDeferred,pending,detailPending\n";
        for (bool quality : {false, true})
            for (double speed : {2., 6., 30.}) {
                scene.terrainLodQuality = quality;
                for (int frame = 0; frame <= fps * 8; ++frame) {
                    const double t = double(frame) / fps, offset = speed * (t < 4 ? t : 8 - t);
                    camera = startCamera;
                    camera.local.x += heading.x * offset;
                    camera.local.z += heading.y * offset;
                    camera.update({}, 0, o.terrainGrid);
                    camera.local.y = height(camera) + clearance;
                    const auto start = Clock::now();
                    r.render(camera, scene);
                    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                    const auto &st = r.terrainStreamingStats();
                    const auto &lod = r.terrainRenderStats();
                    csv << quality << ',' << speed << ',' << frame << ',' << offset << ',' << camera.chunk.x
                        << ',' << camera.chunk.z << ',' << ms << ',' << r.renderingStats().frameMilliseconds
                        << ',' << lod.triangles << ',' << lod.maximumTransitionPixels << ','
                        << lod.protectedMissing << ',' << lod.budgetLimited << ',' << st.pending << ','
                        << st.detailPending << '\n';
                    if (frame % fps == 0)
                        r.capture(output /
                                  (std::string(quality ? "quality" : "distance") + "-walk-" +
                                   std::to_string(int(speed)) + "-" + std::to_string(frame / fps) + ".ppm"));
                }
                settle();
                csv.flush();
                info.flush();
                std::cerr << "completed ground path " << speed << " m/s quality=" << quality << '\n';
            }
        info << "validationErrors=" << r.validationErrors() << '\n';
        if (r.validationErrors() || memory != r.terrainAllocatedBytes())
            throw std::runtime_error("Ground replay validation/memory failure");
        std::cout << "Ground review completed: " << output << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

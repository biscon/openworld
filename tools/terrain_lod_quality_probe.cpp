#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace ow;
// Separate, expensive visual review. No content assets or existing caches.
void detailReview(const std::filesystem::path &output, const std::filesystem::path &shaders, uint32_t cells,
                  int setting) {
    if ((setting != 0 && setting != 1) || (cells != 64 && cells != 128 && cells != 256))
        throw std::invalid_argument("Detail review supports 64/128/256 cells and setting 0/1");
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = shaders;
    o.width = 640;
    o.height = 360;
    o.terrainGrid = {cells, .5f};
    o.terrainCapacity = cells;
    o.lod = {true, 32, 64};
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 2048;
    o.streaming.diagnosticFrameSeconds = 1. / 60;
    TerrainSettings t;
    if (setting) {
        t.erosionWavelength = 1133;
        t.erosionStrength = .674f;
        t.creaseRounding = .270f;
    }
    Camera camera;
    camera.chunk = setting ? ChunkCoord{-20, 86} : ChunkCoord{10, 7};
    camera.local = {64, 800, 64};
    camera.pitch = -.45f;
    {
        auto probe = o;
        probe.streaming.enabled = false;
        probe.lod.enabled = false;
        probe.terrainGrid = {8, 16};
        probe.terrainCapacity = 8;
        Renderer sample(probe);
        const auto terrain = sample.sampleTerrainGeography(probe.terrainGrid, {camera.chunk, t});
        camera.local.y = terrain[4 * 9 + 4].detail.w + 240;
    }
    camera.rebaseGrid({256, .5f}, o.terrainGrid);
    Renderer r(o);
    r.setTerrainSettings(t);
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.viewRadius = 16;
    s.shading = ShadingMode::Legacy;
    const auto memory = r.terrainAllocatedBytes();
    auto settle = [&] {
        const auto start = std::chrono::steady_clock::now();
        int quiet = 0, frame = 0;
        double nextReport = 10;
        while (quiet < 60) {
            // Generate without repeatedly rasterizing the same expensive software
            // view. Periodic terrain frames update geometry demand; the final
            // sixty frames always validate the actual settled terrain view.
            auto work = s;
            const auto &before = r.terrainStreamingStats();
            if (frame++ % 30 && (before.pending || before.detailPending || before.ioJobs))
                work.terrainVisible = false;
            r.render(camera, work);
            const auto &st = r.terrainStreamingStats();
            quiet = st.pending || st.detailPending || st.ioJobs ? 0 : quiet + 1;
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= nextReport) {
                std::cerr << "review " << elapsed << "s pending=" << st.pending
                          << " detail=" << st.detailPending << " selected=" << st.detailSelected
                          << " ready=" << st.detailReady << '\n';
                nextReport += 10;
            }
            if (elapsed > 300)
                throw std::runtime_error("Detail review convergence deadline");
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const double seconds = settle();
    std::ofstream log(output / "detail.txt");
    log << std::setprecision(12) << "cells=" << cells << " spacing=" << o.terrainGrid.spacing
        << " seed=" << t.seed << " region=" << t.regionWavelength << " mountain=" << t.wavelength
        << " height=" << t.heightScale << " erosion=" << t.erosionWavelength << ',' << t.erosionStrength
        << ',' << t.creaseRounding << "\nchunk=" << camera.chunk.x << ',' << camera.chunk.z
        << " local=" << camera.local.x << ',' << camera.local.y << ',' << camera.local.z
        << " pitch=" << camera.pitch << " yaw=" << camera.yaw << "\nsettleSeconds=" << seconds
        << " memory=" << memory << '\n';
    s.freezeLod = true;
    int frame = 0;
    // The view stays fixed. Advance/reverse only the LOD observer to expose
    // changes in the same terrain pixels, independent of view motion.
    for (double offset : {0., 32., 64., 96., 128., 96., 64., 32., 0.}) {
        auto observer = camera;
        observer.local.z -= offset;
        observer.update({}, 0, o.terrainGrid);
        r.setTerrainLodObserver(observer);
        for (int n = 0; n < 2; ++n)
            r.render(camera, s);
        const auto prefix = "detail-" + std::to_string(frame++);
        r.capture(output / (prefix + "-lit.ppm"));
        s.normalView = true;
        r.render(camera, s);
        r.capture(output / (prefix + "-normals.ppm"));
        s.normalView = false;
        const auto &st = r.terrainStreamingStats();
        const auto &lod = r.terrainRenderStats();
        log << prefix << " observerOffset=" << offset << " triangles=" << lod.triangles
            << " scale=" << lod.qualityScale << " detail=" << st.detailEligible << ',' << st.detailSelected
            << ',' << st.detailReady << '\n';
    }
    // A real forward/reverse flight also exercises residency. Save transient
    // frames separately from the final settled fixed-view captures.
    s.freezeLod = false;
    for (int n = 0; n < 120; ++n) {
        camera.local.z += n < 60 ? -2. : 2.;
        camera.update({}, 0, o.terrainGrid);
        r.render(camera, s);
        if (n % 20 == 0)
            r.capture(output / ("flight-" + std::to_string(n) + ".ppm"));
    }
    const auto recovery = settle();
    r.capture(output / "recovered.ppm");
    log << "recoverySeconds=" << recovery << " validation=" << r.validationErrors() << '\n';
    if (r.terrainAllocatedBytes() != memory || r.validationErrors())
        throw std::runtime_error("Detail review memory/validation failure");
    std::cout << "Detail review completed: " << output << " settle=" << seconds << " recovery=" << recovery
              << '\n';
}
int main(int argc, char **argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "/tmp/terrain-lod-quality";
        std::filesystem::create_directories(output);
        if (argc > 2 && std::string(argv[2]) == "--capacity") {
            std::ofstream log(output / "capacity.txt");
            for (double distance : {8192., 16384.}) {
                RendererOptions o;
                o.ui = false;
                o.requireValidation = true;
                o.shaderDirectory = OPENWORLD_SHADER_DIR;
                o.width = o.height = 32;
                o.terrainGrid = {256, .5f};
                o.terrainCapacity = 256;
                o.lod = {true, 32, 64};
                o.streaming.enabled = o.streaming.hierarchical = true;
                o.streaming.diskCache = false;
                o.streaming.maximumDistance = distance;
                Renderer r(o);
                const auto &st = r.terrainStreamingStats();
                log << "distance=" << distance << " supported=" << st.maximumRadius * 128
                    << " gpu=" << st.gpuBytes << " host=" << st.hostBytes << " coverage=" << st.capacity
                    << " detail=" << st.detailCapacity << " validation=" << r.validationErrors() << std::endl;
                if (st.maximumRadius * 128 < distance || st.gpuBytes > o.streaming.gpuBytes ||
                    st.hostBytes > o.streaming.hostBytes || r.validationErrors())
                    throw std::runtime_error("Capacity review budget/validation failure");
            }
            return 0;
        }
        if (argc > 2 && std::string(argv[2]) == "--detail") {
            if (argc > 6)
                throw std::invalid_argument("Usage: probe OUTPUT --detail [SHADERS [CELLS [SETTING:0|1]]]");
            detailReview(output, argc > 3 ? argv[3] : OPENWORLD_SHADER_DIR,
                         argc > 4 ? uint32_t(std::stoul(argv[4])) : 256, argc > 5 ? std::stoi(argv[5]) : 0);
            return 0;
        }
        for (bool reference : {true, false}) {
            RendererOptions o;
            o.ui = false;
            o.requireValidation = true;
            o.shaderDirectory = OPENWORLD_SHADER_DIR;
            o.width = 640;
            o.height = 360;
            o.terrainGrid = {64, .5f};
            o.terrainCapacity = 64;
            o.lod = {true, 16, 64};
            o.streaming.enabled = o.streaming.hierarchical = true;
            o.streaming.diskCache = false;
            o.streaming.maximumDistance = 512;
            o.streaming.diagnosticFrameSeconds = 1. / 60;
            o.streaming.unfilteredHeightReference = reference;
            Renderer r(o);
            TerrainSettings t;
            t.erosionWavelength = 1133;
            t.erosionStrength = .674;
            t.creaseRounding = .270;
            r.setTerrainSettings(t);
            Camera c;
            c.chunk = {-80, 344};
            c.local = {16, 800, 16};
            c.pitch = -.65f;
            SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
            s.gtao.mode = ow::GtaoMode::Off;
            s.viewRadius = 16;
            s.terrainLodQuality = !reference;
            const auto start = std::chrono::steady_clock::now();
            int stable = 0;
            while (stable < 30) {
                r.render(c, s);
                const auto &st = r.terrainStreamingStats();
                stable = st.pending || st.detailPending || st.ioJobs ? 0 : stable + 1;
                if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > 180)
                    throw std::runtime_error("Ridge review convergence deadline");
            }
            const auto name = reference ? "unfiltered" : "filtered";
            r.capture(output / (std::string(name) + ".ppm"));
            std::ofstream info(output / (std::string(name) + ".txt"));
            info << std::setprecision(12) << "seed=" << t.seed << " region=" << t.regionWavelength
                 << " mountain=" << t.wavelength << " height=" << t.heightScale
                 << " erosionWavelength=" << t.erosionWavelength << " erosionStrength=" << t.erosionStrength
                 << " rounding=" << t.creaseRounding << "\nchunk=" << c.chunk.x << ',' << c.chunk.z
                 << " extent=" << o.terrainGrid.extent() << " local=" << c.local.x << ',' << c.local.y << ','
                 << c.local.z << " pitch=" << c.pitch << " yaw=" << c.yaw
                 << "\ntriangles=" << r.terrainRenderStats().triangles
                 << " baseline=" << r.terrainRenderStats().baselineTriangles
                 << " maxErrorPixels=" << r.terrainRenderStats().maximumErrorPixels
                 << " memory=" << r.terrainAllocatedBytes() << "\nsettleSeconds="
                 << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << '\n';
            if (r.validationErrors())
                throw std::runtime_error("Ridge review Vulkan validation");
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

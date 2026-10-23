#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    try {
        const bool fill = argc == 4 && std::string(argv[1]) == "--fill";
        if (!fill && argc != 1 && argc != 3 && argc != 5)
            throw std::invalid_argument("Usage: terrain_generation_probe [CHUNK_X CHUNK_Z [RIDGE_STRENGTH BASE_DETAIL]] or --fill RIDGE BASE_DETAIL");
        ow::RendererOptions o;
        o.ui = false;
        if (fill) { o.width = 320; o.height = 200; } // Keep software rasterization from dominating generation.
        o.requireValidation = true;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.terrainGrid = {256, .5f};
        o.terrainCapacity = 256;
        o.lod = {true, 32, 64};
        o.streaming.enabled = o.streaming.hierarchical = true;
        o.streaming.diskCache = false;
        o.streaming.maximumDistance = fill ? 16384 : 128;
        ow::Renderer r(o);
        ow::Camera c;
        if (!fill && argc >= 3) {
            auto coordinate = [](const char *text) {
                size_t end = 0;
                const std::string value(text);
                const auto result = std::stoll(value, &end);
                if (end != value.size())
                    throw std::invalid_argument("Invalid probe coordinate");
                return result;
            };
            c.chunk = {coordinate(argv[1]), coordinate(argv[2])};
        }
        ow::TerrainSettings settings;
        if (argc == 5 || fill) {
            auto scalar = [](const char *text) {
                size_t end = 0;
                const std::string value(text);
                const float result = std::stof(value, &end);
                if (end != value.size()) throw std::invalid_argument("Invalid shape value");
                return result;
            };
            settings.ridgeStrength = scalar(argv[fill ? 2 : 3]);
            settings.baseDetail = scalar(argv[fill ? 3 : 4]);
            r.setTerrainSettings(settings); // Shared validation includes finite values and range.
        }
        c.local = {64, fill ? 450.f : 200.f, 64};
        if (fill) c.chunk = {0, 2};
        std::cout << "Chunk: " << c.chunk.x << ',' << c.chunk.z
                  << "; ridge/detail: " << settings.ridgeStrength << '/' << settings.baseDetail
                  << "; GPU storage: " << r.terrainAllocatedBytes() / double(ow::terrainMiB) << " MiB\n";
        c.pitch = 1.5f; // Exclude terrain drawing cost from this generation probe.
        ow::SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
        s.gtao.mode = ow::GtaoMode::Off;
        s.viewRadius = fill ? 64 : 0;
        std::vector<double> frameTimes;
        frameTimes.reserve(100000);
        uint32_t stable = 0;
        bool coverageReported = false;
        double peakGeneration = 0;
        uint32_t lastDetail = 0, maxDispatches = 0;
        uint64_t dispatches = 0, groups = 0, multiStageFrames = 0, idleFrames = 0;
        uint32_t maximumStages = 0;
        double generationMilliseconds = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 1; frame <= 100000; ++frame) {
            const auto frameStart = std::chrono::steady_clock::now();
            r.render(c, s);
            frameTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
            const auto &stats = r.terrainStreamingStats();
            dispatches += stats.dispatches;
            groups += stats.generationGroups;
            multiStageFrames += stats.generationSegments > 1;
            idleFrames += stats.dispatches == 0;
            maximumStages = std::max(maximumStages, stats.generationSegments);
            generationMilliseconds += stats.generationMilliseconds;
            maxDispatches = std::max(maxDispatches, stats.dispatches);
            if (r.validationErrors())
                throw std::runtime_error("Generation probe validation failure");
            peakGeneration = std::max(peakGeneration, stats.generationMilliseconds);
            if (fill) {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                const bool covered = stats.coverage && !stats.pending;
                const bool settled = covered && !stats.detailPending && !stats.ioJobs && stats.detailReady == stats.detailSelected;
                stable = settled ? stable + 1 : 0;
                if (frame % 1000 == 0 || (covered && !coverageReported) || stable == 30) {
                    std::cout << "fill frame=" << frame << " seconds=" << seconds << " coverage=" << stats.coverage
                              << " pending=" << stats.pending << " detail=" << stats.detail << " detail_pending=" << stats.detailPending
                              << " ready=" << stats.detailReady << '/' << stats.detailSelected
                              << " generation_ms=" << generationMilliseconds << " peak_generation_ms=" << peakGeneration << std::endl;
                    coverageReported |= covered;
                }
                if (stable == 30) {
                    std::sort(frameTimes.begin(), frameTimes.end());
                    std::cout << "complete frames=" << frame << " seconds=" << seconds
                              << " dispatches=" << dispatches << " generation_ms=" << generationMilliseconds
                              << " frame_p50_ms=" << frameTimes[frameTimes.size()/2]
                              << " frame_p95_ms=" << frameTimes[frameTimes.size()*95/100]
                              << " frame_max_ms=" << frameTimes.back() << " gpu_mib=" << stats.gpuBytes/double(ow::terrainMiB)
                              << " host_mib=" << stats.hostBytes/double(ow::terrainMiB)
                              << " groups=" << groups << " multi_stage_frames=" << multiStageFrames
                              << " max_stages=" << maximumStages << " idle_frames=" << idleFrames << std::endl;
                    return 0;
                }
                if (seconds > 600) throw std::runtime_error("Full-view convergence deadline");
            }
            if (!fill && stats.detail != lastDetail) {
                lastDetail = stats.detail;
                std::cout << "detail=" << lastDetail << " frame=" << frame << " seconds="
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                          << " total_dispatches=" << dispatches << " max_dispatches=" << maxDispatches
                          << " completed_generation_ms=" << generationMilliseconds
                          << std::endl;
                if (lastDetail >= 3)
                    return 0;
            }
        }
        throw std::runtime_error("Generation probe convergence deadline");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

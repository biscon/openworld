#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using Clock = std::chrono::steady_clock;
void run(const std::filesystem::path &output, uint32_t cells, int radius, double distance, int lap) {
    ow::RendererOptions options;
    options.ui = false;
    options.requireValidation = true;
    options.shaderDirectory = OPENWORLD_SHADER_DIR;
    options.terrainGrid = {cells, .5f};
    options.terrainCapacity = cells;
    options.lod = {true, std::min(32u, cells), 64};
    options.streaming.enabled = true;
    options.streaming.hierarchical = true;
    options.streaming.maximumDistance = distance;
    options.streaming.cacheDirectory = output / "cache";
    ow::Renderer r(options);
    ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.viewRadius = radius;
    ow::Camera camera;
    camera.reset(options.terrainGrid);
    camera.local = {options.terrainGrid.extent() * .5, 450, options.terrainGrid.extent() * .5};
    const auto allocation = r.terrainAllocatedBytes();
    std::vector<double> times;
    times.reserve(210000);
    std::vector<double> gpu;
    gpu.reserve(210000);
    std::vector<double> generation;
    generation.reserve(210000);
    std::vector<uint64_t> triangleCounts;
    triangleCounts.reserve(210000);
    uint64_t transfers = 0;
    const auto start = Clock::now();
    const auto frame = [&] {
        const auto t = Clock::now();
        r.render(camera, scene);
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t).count());
        transfers += r.terrainStreamingStats().transferBytes;
        gpu.push_back(r.terrainStreamingStats().terrainMilliseconds);
        generation.push_back(r.terrainStreamingStats().generationMilliseconds);
        triangleCounts.push_back(r.terrainRenderStats().triangles);
        if (times.back() > 100)
            std::cout << "Slow frame " << times.size() << ": " << times.back()
                      << " ms, generations=" << r.terrainGenerationCount()
                      << ", dispatches=" << r.terrainStreamingStats().dispatches
                      << ", triangles=" << r.terrainRenderStats().triangles << '\n';
        if (r.terrainAllocatedBytes() != allocation || r.validationErrors())
            throw std::runtime_error("Streaming allocation/validation regression");
    };
    const auto settle = [&] {
        int stable = 0;
        auto quiet = Clock::now();
        for (int i = 0; i < 100000; ++i) {
            frame();
            const auto &s = r.terrainStreamingStats();
            stable = (s.pending == 0 && s.detailPending == 0 && s.ioJobs == 0) ? stable + 1 : 0;
            if (!stable)
                quiet = Clock::now();
            if (stable > 80 && Clock::now() - quiet > std::chrono::seconds(3))
                break;
            if (i % 1000 == 0)
                std::cout << "Progress: " << i << " frames, " << s.coverage << " tiles, " << s.pending << " pending, "
                          << s.detailPending << " detail pending" << std::endl;
        }
        if (stable < 80)
            throw std::runtime_error("Probe convergence deadline");
    };
    settle();
    const auto initialFrames = times.size();
    const auto initialSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    const auto startup = r.terrainStreamingStats();
    std::cout << (lap ? "warm" : "cold") << " initial: " << startup.coverage << " coverage, " << startup.detail
              << " detail, ready in " << initialSeconds << " s, " << startup.generations << " generations, "
              << startup.hits << " hits\n";
    if (lap && startup.generations)
        std::cout << "Warm run generated " << startup.generations << " products not persisted during the cold run\n";
    r.capture(output / (lap ? "warm.ppm" : "cold.ppm"));
    {
        const auto saved=camera;
        camera.local.y=2000;camera.pitch=-.2f;
        frame();r.capture(output/(lap?"warm-vista.ppm":"cold-vista.ppm"));
        camera=saved;
        const auto heights=r.readStreamedHeights(camera.chunk);
        const auto centre=size_t(cells/2)*(cells+1)+cells/2;
        camera.local.y=heights[centre].y+2;camera.pitch=-.08f;
        settle();r.capture(output/(lap?"warm-ground.ppm":"cold-ground.ppm"));
        camera=saved;settle();
    }
    // Measure a repeatable 100 m/s flight separately from initial fill and teleport recovery.
    const auto flightStart = times.size();
    for (int i = 0; i < 300; ++i) {
        camera.local.x += 100.0 / 120.0;
        camera.update({}, 0, options.terrainGrid);
        frame();
    }
    const auto flightEnd = times.size();
    // Cross boundaries in both directions, turn, change radius, then teleport
    // with work pending.
    for (int i = 0; i < 180; ++i) {
        camera.local.x += options.terrainGrid.extent() * .025 * (i < 90 ? 1 : -1);
        camera.update({}, 0, options.terrainGrid);
        camera.yaw += .008f;
        scene.viewRadius = i < 60    ? radius
                           : i < 100 ? 0
                                     : std::min(ow::terrainMaximumRadius(options.terrainGrid),
                                                int(distance / options.terrainGrid.extent()));
        frame();
    }
    camera.chunk = {-1000000, 1000000};
    for (int i = 0; i < 12; ++i)
        frame();
    camera.chunk = {};
    scene.viewRadius = radius;
    settle();
    r.capture(output / (lap ? "warm-return.ppm" : "cold-return.ppm"));
    const auto &s = r.terrainStreamingStats();
    std::ofstream csv(output / (lap ? "warm.csv" : "cold.csv"));
    csv << "frame,phase,cpu_ms,terrain_gpu_ms,generation_gpu_ms,triangles\n";
    for (size_t i = 0; i < times.size(); ++i)
        csv << i << ','
            << (i < initialFrames ? "initial"
                : i < flightEnd   ? "flight"
                                  : "stress")
            << ',' << times[i] << ',' << gpu[i] << ',' << generation[i] << ',' << triangleCounts[i] << '\n';
    std::vector<double> flight(times.begin() + flightStart, times.begin() + flightEnd);
    std::sort(flight.begin(), flight.end());
    std::cout << (lap ? "warm" : "cold") << " flight: ms p50=" << flight[flight.size() / 2]
              << " p99=" << flight[size_t(.99 * (flight.size() - 1))] << " max=" << flight.back() << '\n';
    std::sort(times.begin(), times.end());
    std::sort(gpu.begin(), gpu.end());
    const auto percentile = [&](double p) { return times[size_t(p * (times.size() - 1))]; };
    std::cout << (lap ? "warm" : "cold") << ": " << times.size() << " frames, "
              << std::chrono::duration<double>(Clock::now() - start).count() << " s; ms p50=" << percentile(.5)
              << " p95=" << percentile(.95) << " p99=" << percentile(.99) << " max=" << times.back()
              << "; terrain GPU ms p50=" << gpu[gpu.size() / 2] << " p99=" << gpu[size_t(.99 * (gpu.size() - 1))]
              << "; GPU=" << s.gpuBytes / double(ow::terrainMiB) << " MiB host=" << s.hostBytes / double(ow::terrainMiB)
              << " MiB; transfer=" << transfers / double(ow::terrainMiB) << " MiB; generations=" << s.generations
              << " hits=" << s.hits << " errors=" << s.errors << '\n';
}
// Separate cold trajectories: no initial settling hides the generation backlog.
void coldFlight(const std::filesystem::path &output, uint32_t cells, int radius, double distance,
                double speed, double seconds, double settleLimit) {
    const auto directory = output / (std::to_string(int(speed)) + "mps");
    if (std::filesystem::exists(directory / "cache"))
        throw std::invalid_argument("Cold flight requires a fresh output directory");
    std::filesystem::create_directories(directory);
    ow::RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.terrainGrid = {cells, .5f};
    o.terrainCapacity = cells;
    o.lod = {true, std::min(32u, cells), 64};
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.maximumDistance = distance;
    o.streaming.cacheDirectory = directory / "cache";
    ow::Renderer r(o);
    ow::Camera c;
    c.reset(o.terrainGrid);
    c.local = {o.terrainGrid.extent() * .5, 450, o.terrainGrid.extent() * .5};
    ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.viewRadius = radius;
    const auto memory = r.terrainAllocatedBytes();
    std::ofstream csv(directory / "flight.csv");
    csv << "seconds,phase,cpu_ms,near_ready,near_requested,detail_pending,cancellations,preemptions,discarded_groups,obsolete_groups,skipped_exports,coverage_pending,io_jobs,coverage_resident,detail_resident,generations\n";
    std::vector<double> flightTimes;
    double readiness = 0;
    uint64_t frames = 0;
    const auto start = Clock::now();
    auto previous = start;
    int settled = 0;
    double settleSeconds = 0;
    for (;;) {
        const auto now = Clock::now();
        const auto elapsed = std::chrono::duration<double>(now - start).count();
        const auto dt = std::chrono::duration<double>(now - previous).count();
        previous = now;
        const int phase = elapsed < seconds ? 0 : elapsed < 2 * seconds ? 1 : 2;
        if (phase < 2) c.local.x += speed * dt * (phase == 0 ? 1 : -1);
        c.update({}, 0, o.terrainGrid);
        r.render(c, scene);
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
        const auto &stats = r.terrainStreamingStats();
        uint32_t ready = 0, wanted = 0;
        for (int z = -2; z <= 2; ++z)
            for (int x = -2; x <= 2; ++x) {
                const glm::dvec2 lo{double(x) * o.terrainGrid.extent() - c.local.x,
                                    double(z) * o.terrainGrid.extent() - c.local.z};
                const auto d = glm::max(glm::max(lo, -lo - double(o.terrainGrid.extent())), glm::dvec2(0));
                wanted += glm::length(d) <= o.terrainGrid.extent();
            }
        for (const auto &tile : r.readTerrainResidency()) {
            if (!tile.resident || !tile.active || !tile.detailResident || tile.key.level) continue;
            const glm::dvec2 lo{double(tile.key.x - c.chunk.x) * o.terrainGrid.extent() - c.local.x,
                                double(tile.key.z - c.chunk.z) * o.terrainGrid.extent() - c.local.z};
            const auto d = glm::max(glm::max(lo, -lo - double(o.terrainGrid.extent())), glm::dvec2(0));
            ready += glm::length(d) <= o.terrainGrid.extent();
        }
        csv << elapsed << ',' << (phase == 0 ? "forward" : phase == 1 ? "reverse" : "settle") << ','
            << ms << ',' << ready << ',' << wanted << ',' << stats.detailPending << ','
            << stats.detailCancellations << ',' << stats.detailPreemptions << ',' << stats.discardedGroups << ','
            << stats.obsoleteDetailGroups << ',' << stats.skippedExports << ',' << stats.pending << ','
            << stats.ioJobs << ',' << stats.coverage << ',' << stats.detail << ',' << stats.generations << '\n';
        if (phase < 2) {
            flightTimes.push_back(ms);
            readiness += wanted ? double(ready) / wanted : 1;
            ++frames;
        } else {
            settled = !stats.pending && !stats.detailPending && !stats.ioJobs ? settled + 1 : 0;
            if (settled >= 60) { settleSeconds = elapsed - 2 * seconds; break; }
            if (elapsed > 2 * seconds + settleLimit) {
                std::cerr << "Cold flight deadline: coverage=" << stats.coverage << " pending=" << stats.pending
                          << " detail=" << stats.detail << " pending=" << stats.detailPending
                          << " I/O=" << stats.ioJobs << " generations=" << stats.generations
                          << " near=" << ready << '/' << wanted << '\n';
                throw std::runtime_error("Cold flight failed to settle within the configured deadline");
            }
        }
        if (r.validationErrors() || r.terrainAllocatedBytes() != memory)
            throw std::runtime_error("Cold flight validation or fixed-memory failure");
    }
    std::sort(flightTimes.begin(), flightTimes.end());
    const auto &s = r.terrainStreamingStats();
    std::cout << "cold " << speed << " m/s: frames=" << frames << " near ready=" << readiness / frames * 100
              << "% p50=" << flightTimes[flightTimes.size()/2] << " p99="
              << flightTimes[size_t(.99*(flightTimes.size()-1))] << " max=" << flightTimes.back()
              << " ms settle=" << settleSeconds << " s cancellations=" << s.detailCancellations
              << " preemptions=" << s.detailPreemptions << " discarded groups=" << s.discardedGroups
              << " obsolete groups=" << s.obsoleteDetailGroups << " skipped exports=" << s.skippedExports
              << " GPU/host MiB=" << s.gpuBytes / double(ow::terrainMiB) << '/' << s.hostBytes / double(ow::terrainMiB)
              << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "/tmp/openworld-hierarchy-review";
        const auto cells = argc > 2 ? uint32_t(std::stoul(argv[2])) : 256;
        const int radius = argc > 3 ? std::stoi(argv[3]) : 64;
        const double distance = argc > 4 ? std::stod(argv[4]) : 16384;
        if (radius < 0 || radius > ow::terrainMaximumRadius({cells, .5f}))
            throw std::invalid_argument("Probe radius exceeds supported distance");
        std::filesystem::create_directories(output);
        if (argc > 5) {
            const double seconds = std::stod(argv[5]);
            const double settleLimit = argc > 6 ? std::stod(argv[6]) : 60;
            if (!std::isfinite(seconds) || !(seconds > 0))
                throw std::invalid_argument("Flight leg duration must be finite and positive");
            if (!std::isfinite(settleLimit) || !(settleLimit > 0) || settleLimit > 600)
                throw std::invalid_argument("Settling deadline must be in (0, 600] seconds");
            coldFlight(output, cells, radius, distance, 100, seconds, settleLimit);
            coldFlight(output, cells, radius, distance, 400, seconds, settleLimit);
            return 0;
        }
        run(output, cells, radius, distance, 0);
        run(output, cells, radius, distance, 1);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

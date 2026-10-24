#include "renderer.hpp"
#include "terrain_generation_budget.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
using namespace ow;
namespace {
struct Temp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ow-hierarchy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() {
        if (std::uncaught_exceptions()) {
            std::cerr << "Retained fixture " << path << "\n";
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
RendererOptions options(const Temp &t) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    // This checks generation/transfer cancellation and fence lifetimes, not
    // image quality. Keep the real terrain payload but avoid a 1280x800 render per frame.
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 64};
    o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.maximumDistance = 128;
    o.streaming.vertexDiagnostics = true;
    o.streaming.gpuBytes = 40 * terrainMiB;
    o.streaming.hostBytes = 64 * terrainMiB;
    o.streaming.transferBytesPerFrame = 32768;
    o.streaming.cacheDirectory = t.path / "cache";
    return o;
}
void settle(Renderer &r, Camera &c, SceneSettings &s) {
    int stable = 0;
    for (int i = 0; i < 12000 && stable < 30; ++i) {
        r.render(c, s);
        const auto &a = r.terrainStreamingStats();
        stable = !a.pending && !a.detailPending && !a.ioJobs ? stable + 1 : 0;
    }
    require(stable == 30, "cache fixture converges");
}
void cancelDetail(TerrainDetailPhase phase, bool pendingPublication = false) {
    Temp temp;
    auto o = options(temp);
    o.streaming.diagnosticFrameSeconds = 1.0 / 120;
    o.streaming.transferBytesPerFrame = 32768;
    Camera c;
    c.local = {8, 100, 8};
    SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.viewRadius = 8;
    if (phase == TerrainDetailPhase::Upload) {
        auto fill = o;
        fill.streaming.transferBytesPerFrame = 32768;
        Renderer cacheFixture(fill);
        settle(cacheFixture, c, scene);
    }
    Renderer r(o);
    const auto memory = r.terrainAllocatedBytes();
    TerrainDetailJobSnapshot job;
    TerrainTileSnapshot tile;
    bool found = false;
    for (int i = 0; i < 10000; ++i) {
        r.render(c, scene);
        job = r.readTerrainDetailJob();
        if (job.phase != phase || job.cancelled)
            continue;
        if (phase == TerrainDetailPhase::Export &&
            (pendingPublication ? !job.publicationPending
                                : job.publicationPending || !r.terrainStreamingStats().cacheReadbackBytes))
            continue;
        if (phase == TerrainDetailPhase::DirectCopy && !r.terrainStreamingStats().directBytes)
            continue;
        if (phase == TerrainDetailPhase::Upload && job.progress <= 0)
            continue;
        const auto tiles = r.readTerrainResidency();
        const auto at = std::find_if(tiles.begin(), tiles.end(), [&](auto t) { return t.handle == job.handle; });
        if (at != tiles.end() && at->resident && at->active && at->heightFade > .1f) {
            tile = *at;
            found = true;
            break;
        }
    }
    require(found, "cancellation fixture reached requested detail phase on "
                   "active coarse tile");
    const auto original = c;
    // Dependency/fade retention keeps coarse coverage valid while detail demand
    // leaves.
    // Leave the full normal-demand range even if the sampled job belongs to a
    // tile ahead of the camera. Eight tiles can still leave that tile eligible;
    // which tile reaches Upload first depends on driver/worker timing.
    c.chunk.x += int64_t(16) << tile.key.level;
    r.render(c, scene);
    auto tiles = r.readTerrainResidency();
    auto at = std::find_if(tiles.begin(), tiles.end(), [&](auto t) { return t.handle == job.handle; });
    require(at != tiles.end() && at->resident, "cancel detail while coarse handle remains valid");
    require(!at->detailWanted && at->detailRevision != job.revision, "demand invalidates detail revision only");
    auto cancelled = r.readTerrainDetailJob();
    if (cancelled.handle == job.handle && cancelled.revision == job.revision &&
        cancelled.phase != TerrainDetailPhase::Idle) {
        require(cancelled.cancelled && cancelled.retireAfter == job.retireAfter && cancelled.groups == job.groups,
                "cancelled scratch retains fence without recording obsolete groups "
                "or copies");
        require(cancelled.completed < cancelled.retireAfter, "scratch retained only while outstanding fence exists");
    }
    // Returning immediately cannot revive the cancelled revision.
    c = original;
    for (int i = 0; i < 4; ++i) {
        r.render(c, scene);
        const auto next = r.readTerrainDetailJob();
        require(next.phase == TerrainDetailPhase::Idle || !(next.handle == job.handle) ||
                    next.revision != job.revision || next.cancelled,
                "cancel latch survives returning demand");
        tiles = r.readTerrainResidency();
        at = std::find_if(tiles.begin(), tiles.end(), [&](auto t) { return t.handle == job.handle; });
        if ((phase != TerrainDetailPhase::Export || pendingPublication) && at != tiles.end())
            require(!at->detailResident, "unpublished cancelled detail never becomes visible");
    }
    int stable = 0;
    for (int i = 0; i < 12000 && stable < 20; ++i) {
        r.render(c, scene);
        const auto &stats = r.terrainStreamingStats();
        require(stats.transferBytes <= 32768, "cancellation fixture sliced transfer budget");
        stable = !stats.pending && !stats.detailPending && !stats.ioJobs ? stable + 1 : 0;
    }
    require(stable == 20, "stationary demand recovers after cancelled detail");
    require(r.terrainStreamingStats().obsoleteDetailGroups == 0,
            "no generation groups submitted after demand withdrawal");
    if (pendingPublication)
        require(r.terrainStreamingStats().discardedPublications > 0, "completed stale publication explicitly rejected");
    require(r.terrainAllocatedBytes() == memory && !r.validationErrors(),
            "cancelled resources reused with fixed memory and valid fences");
}
} // namespace
int main() {
    try {
        cancelDetail(TerrainDetailPhase::Generation);
        cancelDetail(TerrainDetailPhase::DirectCopy);
        cancelDetail(TerrainDetailPhase::Export);
        cancelDetail(TerrainDetailPhase::Export, true);
        cancelDetail(TerrainDetailPhase::Upload);
        std::cout << "Terrain detail cancellation GPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

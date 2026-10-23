#include "renderer.hpp"
#include "test_support.hpp"
#include <chrono>
#include <iostream>
using namespace ow;
namespace {
RendererOptions options(bool streaming, bool hierarchy) {
    RendererOptions o;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.ui = false;
    o.requireValidation = true;
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 8};
    o.visibilityCapacity = 2048;
    o.shadowCapacity.resolution = 128;
    o.streaming.enabled = streaming;
    o.streaming.hierarchical = hierarchy;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 32;
    o.streaming.shadowCasterBytes = 0;
    o.streaming.gpuBytes = 16 * terrainMiB;
    o.streaming.hostBytes = 64 * terrainMiB;
    o.streaming.transferBytesPerFrame = 32768;
    o.streaming.diagnosticFrameSeconds = 1. / 60;
    return o;
}
void run(bool streaming, bool hierarchy) {
    auto o = options(streaming, hierarchy);
    Renderer r(o);
    TerrainSettings terrain;
    terrain.heightScale = 2;
    terrain.erosionStrength = 0;
    terrain.wavelength = 64;
    terrain.regionWavelength = 4096;
    if (streaming)
        r.setTerrainSettings(terrain);
    else
        r.requestTerrain({{}, terrain});
    Camera c;
    c.local = {8, 6, 15};
    c.pitch = -.5;
    SceneSettings s;
    s.viewRadius = 1;
    s.taa.enabled = true;
    s.output.ev100 = 15;
    s.shadows.mode = ShadowMode::Off;
    s.terrainMaterial.enabled = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
    uint32_t frames = 0, valid = 0;
    do {
        r.render(c, s);
        ++frames;
        if (frames % 12 == 0) {
            auto snap = r.readPostSnapshot();
            valid = 0;
            for (auto m : snap.motion) {
                require(std::isfinite(m.x) && std::isfinite(m.y) && std::isfinite(m.z),
                        "Finite terrain motion");
                if (m.w > 0)
                    ++valid;
            }
        }
        if (frames > 24 && valid > 20 &&
            (!streaming ||
             (!r.terrainStreamingStats().pending && !r.terrainStreamingStats().detailPending)))
            break;
    } while (std::chrono::steady_clock::now() < deadline);
    require(valid > 20, "Resident terrain supplies valid previous surface positions");
    auto stable = r.readPostSnapshot();
    double maximum = 0;
    for (auto m : stable.motion)
        if (m.w > 0)
            maximum = std::max(maximum, double(glm::length(glm::vec2(m))));
    require(maximum < .01, "Stationary terrain settles to stationary motion");
    // Continuous observer/camera changes exercise morphs and first-use patch rejection.
    for (int i = 0; i < 12; ++i) {
        c.local.z -= .15;
        c.local.x += .05;
        r.render(c, s);
    }
    auto moving = r.readPostSnapshot();
    valid = 0;
    for (auto m : moving.motion)
        if (m.w > 0) {
            ++valid;
            require(glm::length(glm::vec2(m)) < .2, "Continuous terrain morph produces bounded motion");
        }
    require(valid > 0, "Moving terrain retains unchanged patch correspondence");
    s.visibility = VisibilityMode::HiZ;
    s.gtao.mode = GtaoMode::Half;
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    r.render(c, s);
    require(r.validationErrors() == 0, "Temporal terrain/Hi-Z/AO/shadow synchronization");
    terrain.seed += 1;
    if (streaming)
        r.setTerrainSettings(terrain);
    else
        r.requestTerrain({{}, terrain});
    r.render(c, s);
    auto replaced = r.readPostSnapshot();
    if (streaming)
        require(!r.postStats().historyValid, "World replacement resets streaming history");
    else {
        for (auto m : replaced.motion)
            require(m.w == 0, "Replaced dense heights reject previous geometry");
    }
    std::cout << "Terrain temporal mode " << streaming << '/' << hierarchy << " passed (" << frames
              << " settling frames)\n";
}
} // namespace
int main() {
    try {
        run(false, false);
        run(true, false);
        run(true, true);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

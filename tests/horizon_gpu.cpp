#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
int main() {
    try {
        RendererOptions o;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.requireValidation = true;
        o.ui = false;
        o.width = 96;
        o.height = 96;
        o.shadowCapacity.resolution = 128;
        o.shadowCapacity.casters = 4096;
        o.terrainGrid = {32, .5f};
        o.terrainCapacity = 32;
        o.lod = {true, 8, 64};
        o.streaming.enabled = o.streaming.hierarchical = true;
        o.curvedHorizon = true;
        o.streaming.diskCache = false;
        o.streaming.maximumDistance = 128;
        o.streaming.horizonMaximumDistance = 256;
        o.streaming.horizonBytes = 4 * terrainMiB;
        o.streaming.gpuBytes = 40 * terrainMiB;
        o.streaming.hostBytes = 64 * terrainMiB;
        o.streaming.shadowCasterBytes = terrainMiB;
        o.streaming.transferBytesPerFrame = 32768;
        o.streaming.diagnosticFrameSeconds = 1. / 60;
        Renderer r(o);
        TerrainSettings flat;
        flat.heightScale = 10;
        flat.erosionStrength = .22f;
        r.setTerrainSettings(flat);
        SceneSettings s;
        s.viewRadius = 8;
        s.shadows.mode = ShadowMode::Off;
        s.shading = ShadingMode::Legacy;
        s.background[0] = s.background[2] = 1;
        s.background[1] = 0;
        Camera c;
        c.local = {8, 250, 8};
        c.chunk = {-1, 1000000};
        c.pitch = -1.5707f;
        auto settle = [&] {
            int ready = 0;
            for (int n = 0; n < 4000; ++n) {
                r.render(c, s);
                auto &t = r.terrainStreamingStats();
                require(r.validationErrors() == 0, "Horizon Vulkan validation");
                require(t.transferBytes <= 32768, "Horizon shares transfer budget");
                if (!t.pending && !t.detailPending && !t.ioJobs &&
                    t.horizonReady == t.horizonRequested)
                    ++ready;
                else
                    ready = 0;
                if (ready > 32)
                    return;
            }
            throw std::runtime_error("Horizon fixture did not settle");
        };
        settle();
        require(r.terrainStreamingStats().outerEffectiveRadius == 16,
                "GPU horizon reaches outer radius");
        require(r.terrainStreamingStats().horizonReady > 0, "Coarse outer coverage resident");
        const auto bytes = r.terrainAllocatedBytes();
        for (auto mode : {VisibilityMode::Direct, VisibilityMode::Indirect, VisibilityMode::HiZ}) {
            s.visibility = mode;
            s.opaqueDepthPrepass = true;
            r.render(c, s);
            auto hdr = r.readHdrSnapshot();
            for (auto p : hdr.pixels)
                require(!(p.r > .9f && p.b > .9f && p.g < .1f),
                        "No sky leak at old detailed boundary or coarse seams");
        }
        for (auto tile : r.readTerrainResidency())
            if (tile.key.level > 3 && tile.active)
                require(!tile.detailWanted, "No fine-normal horizon demand");
        c.chunk.x += 9;
        settle();
        s.viewRadius = 4;
        settle();
        require(r.terrainAllocatedBytes() == bytes, "Horizon move and radius change reuse storage");
        s.shading = ShadingMode::Pbr;
        s.atmosphere.mode = AtmosphereMode::Physical;
        s.antialiasing = ow::AntialiasingMode::Taa;
        s.bloom.intensity = .05f;
        s.gtao.mode = GtaoMode::Half;
        s.shadows.mode = ShadowMode::CSM;
        for (int n = 0; n < 6; ++n)
            r.render(c, s);
        require(r.validationErrors() == 0, "Combined horizon atmosphere AO CSM TAA validation");
        r.waitIdle();
        auto limitedOptions = o;
        limitedOptions.streaming.horizonBytes = 1;
        Renderer limited(limitedOptions);
        limited.render(c, s);
        require(limited.terrainStreamingStats().horizonSlots == 0 &&
                    limited.terrainStreamingStats().horizonLimited,
                "Insufficient distant budget reports fallback without consuming near slots");
        require(limited.terrainStreamingStats().maximumRadius ==
                    r.terrainStreamingStats().maximumRadius,
                "Distant budget fallback preserves detailed capacity");
        require(limited.validationErrors() == 0, "Distant budget fallback validation");
        std::cout << "Horizon GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

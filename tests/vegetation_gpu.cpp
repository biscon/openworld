#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
#include <set>
using namespace ow;
namespace {
RendererOptions options(std::shared_ptr<VegetationFixture> fixture) {
    RendererOptions o;
    o.ui = false;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 128;
    o.height = 96;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.requireValidation = true;
    o.shadowCapacity.resolution = 128;
    o.vegetation.fixture = std::move(fixture);
    return o;
}
void compare(const HdrSnapshot &a, const HdrSnapshot &b) {
    require(a.pixels.size() == b.pixels.size(), "matching image extents");
    for (size_t i = 0; i < a.pixels.size(); ++i)
        near(glm::length(a.pixels[i] - b.pixels[i]), 0, .002, "matching colour/depth mask and deformation");
}
std::set<uint32_t> ids(const VegetationSnapshot &s) {
    return {s.identities[0].begin(), s.identities[0].end()};
}
} // namespace
int main() {
    try {
        auto fixture = std::make_shared<VegetationFixture>(makeVegetationFixture(makeVegetationAssets(), 8));
        for (bool fixed : {true, false}) {
            auto o = options(fixture);
            o.forceFixedMaterials = fixed;
            Renderer r(o);
            SceneSettings s;
            s.vegetation.shadowDistance = 25;
            s.terrainVisible = false;
            s.shadows.mode = ShadowMode::Off;
            Camera c;
            c.local = {0, 1.2, 5};
            c.pitch = -.15f;
            for (uint32_t rep = 0; rep < 3; ++rep) {
                s.vegetation.representation = rep;
                s.vegetation.time = .4f;
                s.vegetation.wind = 2;
                s.vegetation.coverage = true;
                s.taa.enabled = false;
                s.shadows.mode = ShadowMode::Off;
                s.opaqueDepthPrepass = false;
                r.render(c, s);
                auto colour = r.readHdrSnapshot();
                s.opaqueDepthPrepass = true;
                r.render(c, s);
                compare(colour, r.readHdrSnapshot());
                auto snap = r.readVegetationSnapshot();
                require(snap.stats.visible > 0, "visible plants");
                require(!snap.stats.overflow, "bounded lists");
                auto full = ids(snap);
                require(full.size() == snap.stats.visible, "one bin per visible plant");
                for (auto id : full)
                    require(id < fixture->instances.size(), "valid compact index");
                s.vegetation.density = .5;
                r.render(c, s);
                auto sparse = ids(r.readVegetationSnapshot());
                require(sparse.size() < full.size(), "density reduction");
                for (auto id : sparse)
                    require(full.count(id), "stable density subset");
                s.vegetation.density = 1;
                s.vegetation.coverage = false;
                s.taa.enabled = true;
                s.shadows.mode = ShadowMode::CSM;
                for (uint32_t f = 0; f < 4; ++f) {
                    s.vegetation.time = .4f + f / 120.f;
                    require(r.render(c, s), "temporal shadow integration");
                }
                snap = r.readVegetationSnapshot();
                bool offscreen = false;
                for (uint32_t view = 1; view < 6; ++view)
                    for (auto id : snap.identities[view])
                        offscreen |= !full.count(id);
                require(offscreen, "offscreen plants retained in independent shadow lists");
                auto post = r.readPostSnapshot();
                uint32_t moving = 0, valid = 0;
                for (auto v : post.motion) {
                    valid += v.w > 0;
                    moving += v.w > 0 && glm::length(glm::vec2(v)) > 1e-6;
                }
                require(valid > 8 && moving > 4, "wind contributes submitted-frame motion");
                r.resetTemporalHistory();
                r.render(c, s);
                post = r.readPostSnapshot();
                for (auto v : post.motion)
                    require(v.w == 0, "reset invalidates plant history");
            }
            // Distances exercise actual middle/far bins, including the hybrid card
            // stage.
            s.shadows.mode = ShadowMode::Off;
            s.taa.enabled = false;
            s.vegetation.representation = 0;
            c.local = {0, 1.2, 30};
            c.pitch = 0;
            r.render(c, s);
            auto middle = r.readVegetationSnapshot();
            require(middle.stats.visible > 0, "middle LOD population");
            for (uint32_t b = 0; b < 12; ++b)
                if (b % 6 != 1)
                    require(middle.counts[b] == 0, "middle blade bin selection");
            c.local.z = 55;
            s.vegetation.representation = 2;
            r.render(c, s);
            auto distant = r.readVegetationSnapshot();
            require(distant.stats.visible > 0, "far hybrid population");
            for (uint32_t b = 0; b < 12; ++b)
                if (b % 6 != 5)
                    require(distant.counts[b] == 0, "distant card bin selection");
            s.vegetation.enabled = false;
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.visible == 0, "disabled vegetation");
            s.vegetation.enabled = true;
            s.vegetation.density = 0;
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.visible == 0, "empty selection");
            s.vegetation.density = 1;
            c.local = {0, 1, 1000};
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.visible == 0, "all cells outside range");
            require(r.validationErrors() == 0, "vegetation validation");
        }
        {
            auto exact = std::make_shared<VegetationFixture>(*fixture);
            exact->instances.resize(128);
            for (auto &p : exact->instances)
                p.rootScale = {0, 0, -4, .5};
            auto o = options(exact);
            Renderer r(o);
            SceneSettings s;
            s.vegetation.shadowDistance = 25;
            s.terrainVisible = false;
            s.shadows.mode = ShadowMode::Off;
            Camera c;
            c.local = {0, .5, 4};
            r.render(c, s);
            auto snap = r.readVegetationSnapshot();
            require(snap.stats.visible == 128 && !snap.stats.overflow, "exact-capacity compact list");
            require(ids(snap).size() == 128, "all exact-capacity identities retained");
        }
        // Deliberate undersizing must clamp indirect counts and never overwrite
        // storage.
        {
            auto o = options(fixture);
            o.vegetation.listCapacity = 7;
            Renderer r(o);
            SceneSettings s;
            s.vegetation.shadowDistance = 25;
            s.terrainVisible = false;
            s.shadows.mode = ShadowMode::Off;
            Camera c;
            c.local = {0, 8, 0};
            c.pitch = -1.55f;
            r.render(c, s);
            auto snap = r.readVegetationSnapshot();
            require(snap.stats.visible == 7 && snap.stats.overflow > 0,
                    "overflow clamps commands and reports loss");
            require(ids(snap).size() == 7, "overflow preserves valid compact identities");
            require(!r.validationErrors(), "overflow validation");
        }
        {
            auto empty = std::make_shared<VegetationFixture>(*fixture);
            empty->instances.clear();
            auto o = options(empty);
            Renderer r(o);
            SceneSettings s;
            s.vegetation.shadowDistance = 25;
            s.terrainVisible = false;
            Camera c;
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.visible == 0, "zero candidate fixture");
        }
        {
            auto o = options(fixture);
            o.vegetation.budgetBytes = 64;
            bool failed = false;
            try {
                Renderer r(o);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            require(failed, "allocation budget rejected before rendering");
        }
        std::cout << "vegetation GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

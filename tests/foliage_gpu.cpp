#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
#include <set>
using namespace ow;
RendererOptions options(std::shared_ptr<VegetationLibrary> library,
                        std::shared_ptr<VegetationPopulation> population, bool fixed) {
    RendererOptions o;
    o.ui = false;
    o.width = 128;
    o.height = 96;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.requireValidation = true;
    o.forceFixedMaterials = fixed;
    o.shadowCapacity.resolution = 128;
    o.vegetation.library = library;
    o.vegetation.population = population;
    return o;
}
float difference(const HdrSnapshot &a, const HdrSnapshot &b) {
    float total = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i)
        total += glm::length(glm::vec3(a.pixels[i] - b.pixels[i]));
    return total / a.pixels.size();
}
int main() {
    try {
        auto library = std::make_shared<VegetationLibrary>(buildVegetationLibrary());
        for (bool fixed : {true, false}) {
            auto population = std::make_shared<VegetationPopulation>(makeVegetationPopulation(8, true));
            population->instances[0].rootScale = {0, 0, 7, 1};
            // Irregular cell ranges and IDs above float's exact integer range.
            population->instances.erase(population->instances.begin() + 127);
            --population->cells[0].count;
            for (size_t i = 1; i < population->cells.size(); ++i)
                --population->cells[i].first;
            for (auto &p : population->instances)
                p.identity += 0xf0000000u;
            Renderer r(options(library, population, fixed));
            SceneSettings s;
            s.terrainVisible = false;
            s.antialiasing = ow::AntialiasingMode::Off;
            s.shadows.mode = ShadowMode::CSM;
            s.vegetation.depthPrepass = false;
            s.vegetation.wind = 2;
            s.vegetation.time = .3f;
            Camera c;
            c.local = {0, 1.1, 5};
            c.pitch = -.12f;
            r.render(c, s);
            auto initial = r.readVegetationSnapshot();
            require(initial.stats.visible > 0 && !initial.stats.overflow,
                    "mixed foliage visible without overflow");
            require(initial.stats.casterCandidates == 8, "only fern/shrub are casting candidates");
            std::set<uint32_t> camera(initial.identities[0].begin(), initial.identities[0].end());
            bool offscreen = false;
            for (uint32_t view = 1; view < 6; ++view)
                for (uint32_t id : initial.identities[view]) {
                    require((id - 0xf0000000u) % 128 < 2, "grass never enters caster lists");
                    offscreen |= !camera.count(id);
                }
            require(offscreen, "independent offscreen foliage casters");
            const float originalRange = s.vegetation.distance;
            s.vegetation.distance = 1;
            r.render(c, s);
            const auto contracted = r.readVegetationSnapshot();
            for (uint32_t view = 1; view < 6; ++view)
                require(contracted.identities[view] == initial.identities[view],
                        "camera range does not remove independently budgeted casters");
            s.vegetation.distance = originalRange;
            r.render(c, s);
            auto colour = r.readHdrSnapshot();
            s.opaqueDepthPrepass = true;
            r.render(c, s);
            require(difference(colour, r.readHdrSnapshot()) < .0005f,
                    "matching foliage depth/colour deformation and mask");
            s.gtao.mode = GtaoMode::Half;
            r.render(c, s);
            require(r.gtaoStats().effective == GtaoMode::Half, "foliage depth feeds GTAO");
            s.gtao.mode = GtaoMode::Off;
            s.antialiasing = ow::AntialiasingMode::Taa;
            for (uint32_t f = 0; f < 4; ++f) {
                s.vegetation.time = .4f + float(f) / 120;
                r.render(c, s);
            }
            auto post = r.readPostSnapshot();
            uint32_t moving = 0;
            for (auto v : post.motion)
                moving += v.w > 0 && glm::length(glm::vec2(v)) > 1e-6f;
            require(moving > 4, "wind contributes submitted motion");
            s.vegetation.forceLod = 1;
            r.render(c, s);
            for (auto v : r.readPostSnapshot().motion)
                require(v.w == 0, "LOD override rejects incompatible correspondence");
            s.vegetation.forceLod = -1;
            s.vegetation.enabled = false;
            r.render(c, s);
            s.vegetation.enabled = true;
            r.render(c, s);
            for (auto v : r.readPostSnapshot().motion)
                require(v.w == 0, "reenabled foliage starts fresh history");
            s.vegetation.time = 0;
            r.render(c, s);
            post = r.readPostSnapshot();
            for (auto v : post.motion)
                require(v.w == 0, "time scrub invalidates history");
            s.antialiasing = ow::AntialiasingMode::Off;
            s.opaqueDepthPrepass = false;
            s.vegetation.time = .2f;
            s.vegetation.transmission = false;
            r.render(c, s);
            auto opaque = r.readHdrSnapshot();
            s.vegetation.transmission = true;
            r.render(c, s);
            require(difference(opaque, r.readHdrSnapshot()) > 1e-5f, "foliage transmission affects lighting");
            // CSM snapping is intentionally anchored to the world light-space
            // texel grid. Isolate the foliage geometry/wind translation contract.
            s.shadows.mode = ShadowMode::Off;
            r.resetTemporalHistory();
            r.render(c, s);
            auto before = r.readHdrSnapshot();
            // Same world camera through a different chunk/local representation.
            c.chunk.x += 100;
            c.local.x -= 400;
            r.render(c, s);
            require(difference(before, r.readHdrSnapshot()) < .0005f,
                    "camera rebase preserves foliage and wind");
            require(!r.validationErrors(), "foliage synchronization validation");
            auto distant = std::make_shared<VegetationPopulation>(*population);
            for (auto &cell : distant->cells)
                cell.origin = translated(cell.origin, {1e12, 0, -1e12});
            // Grid chunks are 4 m; this translation is also an exact multiple of
            // the 256 m wind period. Geometry and animation should be unchanged.
            c.chunk.x += 250000000000ll;
            c.chunk.z -= 250000000000ll;
            Renderer far(options(library, distant, fixed));
            far.render(c, s);
            near(difference(before, far.readHdrSnapshot()), 0, .0005f,
                 "large integer origins preserve local geometry and wind phase");
            require(!far.validationErrors(), "distant foliage synchronization validation");
        }
        {
            auto p = std::make_shared<VegetationPopulation>(makeVegetationPopulation(8, false));
            Renderer r(options(library, p, true));
            SceneSettings s;
            s.terrainVisible = false;
            s.shadows.mode = ShadowMode::CSM;
            Camera c;
            c.local = {0, 2, 5};
            c.pitch = -.3f;
            r.render(c, s);
            auto a = r.readVegetationSnapshot();
            require(a.stats.visible > 0 && a.stats.casters == 0 && a.stats.casterCandidates == 0,
                    "grass receives but never casts");
            s.shadows.mode = ShadowMode::Off;
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.selectionGroups == a.stats.selectionGroups,
                    "CSM adds no grass selection work");
        }
        {
            auto p = std::make_shared<VegetationPopulation>(makeVegetationPopulation(4, true));
            auto o = options(library, p, true);
            o.vegetation.listCapacity = 3;
            Renderer r(o);
            SceneSettings s;
            s.terrainVisible = false;
            Camera c;
            c.local = {0, 5, 0};
            c.pitch = -1.5f;
            r.render(c, s);
            auto snapshot = r.readVegetationSnapshot();
            require(snapshot.stats.visible <= 3 && snapshot.stats.overflow > 0,
                    "multipart overflow safely clamped");
            r.render(c, s);
            require(r.readVegetationSnapshot().identities == snapshot.identities,
                    "overflow retains a stable subset across frames");
            require(!r.validationErrors(), "overflow validation");
        }
        // The range control extends actual foliage beyond the old saved-asset cap.
        for (bool fixed : {false, true}) {
            auto p = std::make_shared<VegetationPopulation>(makeVegetationPopulation(4, true));
            p->cells[0].origin = translated(p->cells[0].origin, {0, 0, -140});
            Renderer r(options(library, p, fixed));
            SceneSettings s;
            s.terrainVisible = false;
            s.shadows.mode = ShadowMode::Off;
            s.vegetation.wind = 2;
            s.vegetation.distance = 80;
            Camera c;
            c.local = {0, 1, 0};
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.visible == 0, "short range excludes far foliage");
            s.vegetation.distance = 200;
            r.render(c, s);
            const auto clear = r.readVegetationSnapshot();
            require(clear.stats.visible > 0, "200 m range reaches beyond saved asset limits");
            auto quad = r.uploadMesh(makeQuad());
            MeshMaterial material;
            material.doubleSided = true;
            auto mh = r.createMaterial(material);
            MeshInstance wall;
            wall.mesh = quad;
            wall.material = mh;
            wall.position.local = {0, 1, -10};
            wall.scale = {20, 20, 1};
            auto wh = r.createInstance(wall);
            s.opaqueDepthPrepass = true;
            r.render(c, s);
            const auto reference = r.readHdrSnapshot();
            const auto unculled = r.readVegetationSnapshot();
            s.vegetation.occlusion = true;
            s.gtao.mode = GtaoMode::Half;
            // Compare with AO enabled on both sides as well as checking its depth
            // path.
            r.render(c, s);
            auto hidden = r.readVegetationSnapshot();
            require(hidden.stats.visible == 0 && unculled.stats.visible > 0,
                    "solid depth actually occludes far vegetation");
            require(r.gtaoStats().effective == GtaoMode::Half, "split depth still supplies GTAO");
            s.gtao.mode = GtaoMode::Off;
            r.render(c, s);
            require(difference(reference, r.readHdrSnapshot()) < .0005f,
                    "vegetation occlusion preserves the visible image");
            r.removeInstance(wh);
            r.render(c, s);
            require(r.readVegetationSnapshot().identities[0] == clear.identities[0],
                    "removing solid occluder restores every visible plant without "
                    "self-occlusion");
            s.vegetation.distance = 80;
            r.render(c, s);
            require(r.readVegetationSnapshot().stats.visible == 0,
                    "live contraction clears indirect commands");
            require(!r.validationErrors(), "range/Hi-Z/GTAO synchronization validation");
        }
        {
            auto p = std::make_shared<VegetationPopulation>();
            Renderer r(options(library, p, true));
            SceneSettings s;
            s.terrainVisible = false;
            r.render({}, s);
            require(r.readVegetationSnapshot().stats.visible == 0, "empty population");
        }
        std::cout << "foliage GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

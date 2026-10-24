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
void grassTransitions() {
    VegetationRecipe recipe;
    recipe.width = .05f;
    recipe.height = 1.4f;
    auto library = std::make_shared<VegetationLibrary>(buildVegetationLibrary(recipe));
    auto population = std::make_shared<VegetationPopulation>();
    population->cells.push_back({{}, 0, 1});
    population->instances.push_back({{0, 0, 0, 2}, .4f, .5f, 0, 0});
    auto o = options(library, population, true);
    o.width = 512;
    o.height = 288;
    Renderer r(o);
    SceneSettings s;
    s.terrainVisible = false;
    s.shadows.mode = ShadowMode::Off;
    s.vegetation.wind = 1;
    s.vegetation.time = .7f;
    s.vegetation.depthPrepass = false;
    s.antialiasing = AntialiasingMode::Off;
    Camera c;
    c.local = {0, 1, 10};
    // The first LOD is exactly the close geometry, including wind and shading.
    r.render(c, s);
    auto near = r.readHdrSnapshot();
    s.vegetation.forceLod = 0;
    r.render(c, s);
    require(difference(near, r.readHdrSnapshot()) < 1e-6f, "near grass unchanged by automatic LOD");
    s.vegetation.forceLod = -1;
    std::set<uint32_t> visited;
    for (double distance : {18., 22., 30., 36., 40., 60.}) {
        c.local.z = distance;
        s.opaqueDepthPrepass = false;
        r.render(c, s);
        const auto colour = r.readHdrSnapshot();
        const auto snapshot = r.readVegetationSnapshot();
        require(snapshot.stats.visible == 1 && snapshot.stats.casters == 0,
                "grass morph retains instance identity");
        for (uint32_t lod = 0; lod < 3; ++lod)
            if (snapshot.counts[lod])
                visited.insert(lod);
        s.opaqueDepthPrepass = true;
        r.render(c, s);
        require(difference(colour, r.readHdrSnapshot()) < .00001f,
                "morph and geometric fade agree in depth and colour");
    }
    require(visited.size() == 3, "fixture crosses both grass LOD bands");
    // Immediately before a bin change, the fine mesh must rasterize the same
    // wind-deformed coverage as the next mesh (no double draw or silhouette pop).
    uint32_t hash = 781;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16;
    const float stagger = (float(hash >> 8) / 16777216.f - .5f) * 4;
    const float focalScale = 288.f / 1080.f * 2;
    s.vegetation.coverage = true;
    for (uint32_t lod = 0; lod < 2; ++lod) {
        const float d =
            15 + (library->assets[0].grass.bands[lod * 2 + 1] + stagger - 15 - .0001f) * focalScale;
        c.local.z = std::sqrt(d * d - 1);
        r.render(c, s);
        const auto morphed = r.readHdrSnapshot();
        require(r.readVegetationSnapshot().counts[lod] == 1, "endpoint fixture is in the retiring bin");
        s.vegetation.forceLod = int32_t(lod + 1);
        r.render(c, s);
        const auto coarse = r.readHdrSnapshot();
        uint32_t changed = 0;
        for (size_t i = 0; i < coarse.pixels.size(); ++i)
            changed += (coarse.pixels[i].x > .5f) != (morphed.pixels[i].x > .5f);
        // Subdivided and unsplit triangles may disagree on an edge pixel due to
        // floating-point projection/subpixel raster precision, even when coplanar.
        require(changed <= 2, "morph endpoint matches the next LOD within two raster edge pixels");
        s.vegetation.forceLod = -1;
    }
    s.vegetation.coverage = false;
    // Moving inside the first morph band must reproject previous geometry, not
    // merely the current mesh under the previous camera matrix.
    s.antialiasing = AntialiasingMode::Taa;
    s.vegetation.wind = 0;
    c.local.z = 18;
    for (int i = 0; i < 3; ++i)
        r.render(c, s);
    c.local.z += .02;
    r.render(c, s);
    uint32_t moving = 0;
    for (auto motion : r.readPostSnapshot().motion) {
        require(std::isfinite(motion.x) && std::isfinite(motion.y), "finite grass morph motion");
        moving += motion.w > 0 && glm::length(glm::vec2(motion)) > 1e-6f;
    }
    require(moving > 4, "morphing grass retains valid submitted motion");
    c.local.z = 40;
    r.render(c, s);
    for (auto motion : r.readPostSnapshot().motion)
        require(motion.w <= 0, "grass bin changes reject incompatible history");
    require(!r.validationErrors(), "grass transition Vulkan validation");
}
int main() {
    try {
        grassTransitions();
        auto library = std::make_shared<VegetationLibrary>(buildVegetationLibrary());
        for (bool fixed : {true, false}) {
            auto population = std::make_shared<VegetationPopulation>(makeVegetationPopulation(8, true));
            // Outside the camera even with the larger combined wind/contact envelope.
            population->instances[0].rootScale = {-3, 0, 7, 1};
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

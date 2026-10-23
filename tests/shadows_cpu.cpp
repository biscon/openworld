#include "shadows.hpp"
#include "terrain_stream.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>
using namespace ow;
int main() {
    try {
        ShadowSettings settings;
        Camera observer;
        TerrainGrid projectionGrid{256, .5f};
        for (auto sun : {glm::dvec3(0, 1, 0), glm::dvec3(.01, .001, 1), glm::dvec3(-.5, .8, .3)}) {
            auto basis = shadowBasis(sun);
            auto n = basis * glm::normalize(sun);
            near(n.x, 0, 1e-10, "Sun basis x");
            near(n.y, 0, 1e-10, "Sun basis y");
            near(n.z, 1, 1e-10, "Sun basis depth");
            near(glm::determinant(basis), 1, 1e-10, "Light basis preserves handedness");
        }
        const auto basisA = shadowBasis({.001, 1, 0}), basisB = shadowBasis({-.001, 1, 0});
        require(glm::length(basisA[0] - basisB[0]) < .003, "No zenith basis discontinuity");
        for (int64_t coordinate : {-1000000000ll, -1ll, 0ll, 1000000000ll}) {
            observer.chunk = {coordinate, -coordinate};
            observer.local = {17, 100, 28};
            auto cascades =
                shadowCascadeLayout(observer, projectionGrid, 16.f / 9, {-.6, .8, .35}, settings, 8192, 2048);
            near(cascades[0].farDistance, 50, 1e-5, "First split");
            near(cascades[4].farDistance, 8192, 1e-5, "Far mountains");
            for (uint32_t i = 1; i < 5; ++i)
                require(cascades[i].nearDistance < cascades[i - 1].farDistance, "Cascade overlap");
            auto turned = observer;
            turned.yaw = 2;
            turned.pitch = .7;
            auto rotated =
                shadowCascadeLayout(turned, projectionGrid, 16.f / 9, {-.6, .8, .35}, settings, 8192, 2048);
            for (uint32_t i = 0; i < 5; ++i)
                near(cascades[i].radius, rotated[i].radius, 1e-10, "Rotation cannot rescale maps");
            auto rebased = observer;
            rebased.chunk.x++;
            rebased.local.x -= projectionGrid.extent();
            auto same =
                shadowCascadeLayout(rebased, projectionGrid, 16.f / 9, {-.6, .8, .35}, settings, 8192, 2048);
            for (uint32_t i = 0; i < 5; ++i)
                near(glm::length(cascades[i].centre - same[i].centre), 0, 1e-8,
                     "World rebasing preserves snapped centre");
            observer.local.x += .01;
            auto translated =
                shadowCascadeLayout(observer, projectionGrid, 16.f / 9, {-.6, .8, .35}, settings, 8192, 2048);
            for (uint32_t i = 0; i < 5; ++i) {
                const auto displacement = translated[i].centre - cascades[i].centre +
                                          glm::dvec2(cascades[i].basis * glm::dvec3(.01, 0, 0));
                const double texel = 2 * cascades[i].radius / 2048;
                // Double precision world phase loses <0.1 mm at these coordinates.
                for (int axis = 0; axis < 2; ++axis)
                    near(displacement[axis] / texel, std::round(displacement[axis] / texel), .002,
                         "Translation moves only whole texels");
            }
        }
        auto shortRange = shadowCascadeLayout(observer, projectionGrid, 1, {0, 1, 0}, settings, 128, 256);
        require(shortRange[4].farDistance == 0 && shortRange[3].farDistance == 128,
                "Short views omit redundant far cascade");
        VisibilityBounds box{{-3, -2, -1}, {3, 2, 1}, true};
        auto basis = shadowBasis({-.6, .8, .35});
        auto transformed = shadowLightBounds(box, basis);
        for (uint32_t i = 0; i < 8; ++i) {
            auto p = basis * glm::dvec3((i & 1) ? 3 : -3, (i & 2) ? 2 : -2, (i & 4) ? 1 : -1);
            require(glm::all(glm::greaterThanEqual(p, glm::dvec3(transformed.lo) - 1e-6)) &&
                        glm::all(glm::lessThanEqual(p, glm::dvec3(transformed.hi) + 1e-6)),
                    "Caster bounds contain every transformed corner");
        }
        {
            TerrainLod lod({64, 1}, 64, {true, 8, 32}, 1);
            std::vector<glm::vec2> bounds(lod.leafCount(), {0, 1});
            lod.setBounds(0, bounds.data());
            Camera camera;
            camera.local = {32, 20, 50};
            camera.pitch = -.5f;
            std::array<ChunkCoord, maxTerrainChunks> chunks{};
            auto vp = camera.viewProjection(1.5f);
            lod.select(chunks, 1, camera, camera, vp, true, true, true);
            auto visible = lod.draws();
            auto before = lod.stats();
            lod.collectCasters(true);
            lod.select(chunks, 1, camera, camera, vp, true, true, true);
            require(lod.draws().size() == visible.size() && lod.stats().triangles == before.triangles,
                    "Shadow collection preserves camera quality selection");
            for (size_t i = 0; i < visible.size(); ++i) {
                const auto &a = visible[i], &b = lod.draws()[i];
                require(a.slot == b.slot && a.x == b.x && a.z == b.z && a.step == b.step && a.mask == b.mask,
                        "Visible topology unchanged by shadow collection");
                require(std::any_of(lod.casterDraws().begin(), lod.casterDraws().end(),
                                    [&](const TerrainPatchDraw &c) {
                                        return c.slot == a.slot && c.x == a.x && c.z == a.z &&
                                               c.level == a.level && c.step == a.step &&
                                               (c.mask & a.mask) == a.mask;
                                    }),
                        "Every visible patch has identical caster geometry and quadrant coverage");
            }
            camera.yaw = 3.14159265f;
            camera.local.z = 100;
            lod.select(chunks, 1, camera, camera, camera.viewProjection(1.5f), true, true, true);
            require(lod.draws().empty() && !lod.casterDraws().empty(), "Offscreen terrain remains a caster");
        }
        TerrainGrid grid{32, .5f};
        auto cameraSlots = terrainHierarchyCapacity(4);
        TerrainStreamer stream(cameraSlots + 4, 4, grid, true, 4);
        Camera camera;
        TerrainTileKey key{100, 100, stream.topLevel()};
        stream.shadowDemand(&key, 1);
        stream.demand(camera, 4, 0, 0, 1);
        int slot = stream.findTile(key);
        require(slot >= int(cameraSlots), "Offscreen demand uses reserved slots");
        require(stream.slot(uint32_t(slot)).shadowOnly, "Shadow demand does not become camera coverage");
        stream.slot(uint32_t(slot)).state = TerrainStreamer::State::Resident;
        stream.slot(uint32_t(slot)).lastUse = 3;
        stream.advance(.1f);
        require(!stream.slot(uint32_t(slot)).visible, "Shadow-only terrain stays out of camera selection");
        stream.shadowDemand(nullptr, 0);
        stream.demand(camera, 4, 1, 3, 1);
        require(stream.slot(uint32_t(slot)).state == TerrainStreamer::State::Retiring,
                "Submitted shadow reader retains terrain");
        stream.demand(camera, 4, 3, 3, 1);
        require(stream.slot(uint32_t(slot)).state == TerrainStreamer::State::Empty,
                "Terrain released after shadow submission completes");
        TerrainStreamer moving(cameraSlots + 1, 4, grid, true, 1);
        moving.demand(camera, 4, 0, 0, 1);
        uint32_t prior = 0;
        while (moving.slot(prior).level != moving.topLevel())
            ++prior;
        auto old = moving.slot(prior).tile();
        moving.slot(prior).state = TerrainStreamer::State::Resident;
        moving.shadowDemand(&old, 1);
        camera.chunk = {1000, 1000};
        moving.demand(camera, 4, 0, 0, 1);
        require(moving.slot(prior).state == TerrainStreamer::State::Retiring,
                "Shadow demand cannot pin a departed camera slot");
        moving.demand(camera, 4, 0, 0, 1);
        require(moving.findTile(old) >= int(cameraSlots),
                "Departed camera caster moves to reserved residency");
        rejects([&] { TerrainStreamer invalid(cameraSlots, 4, grid, true, 1); },
                "Reservation preserves minimum camera capacity");
        rejects([&] { moving.shadowDemand(nullptr, 1); }, "Invalid external demand rejected");
        settings.biasTexels = NAN;
        rejects([&] { validate(settings); }, "Invalid bias rejected");
        std::cout << "shadows cpu passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

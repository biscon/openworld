#include "gtao_fixtures.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>
using namespace ow;
namespace {
void finite(const GtaoSnapshot &s) {
    require(s.pixels.size() == size_t(s.width) * s.height, "Snapshot native extent");
    for (auto p : s.pixels)
        for (int c = 0; c < 4; ++c)
            require(std::isfinite(p[c]) && p[c] >= 0 && p[c] <= 1, "Bounded finite AO");
    for (auto z : s.depth)
        require(std::isfinite(z) && z >= 0, "Finite view depth");
    for (auto o : s.offsets)
        require(o <= 3, "Representative offset bounded");
}
// Independent scalar definition of the original 5x5 filter. Check its observable
// output across image/group edges, not the shader's shared tile or factored paths.
void filterReference(const GtaoSnapshot &s) {
    auto valid = [&](glm::ivec2 p) {
        return p.x >= 0 && p.y >= 0 && p.x < int(s.workWidth) && p.y < int(s.workHeight);
    };
    auto index = [&](glm::ivec2 p) { return size_t(p.y) * s.workWidth + p.x; };
    auto edge = [&](glm::ivec2 p, int direction) {
        if (!valid(p))
            return 0.f;
        auto packed = uint32_t(std::lround(s.edges[index(p)] * 255));
        return float((packed >> (6 - 2 * direction)) & 3) / 3;
    };
    auto path = [&](glm::ivec2 p, glm::ivec2 q, int first) {
        float weight = 1;
        for (int step = 0; step < 2; ++step) {
            int axis = (first + step) % 2;
            while (p[axis] != q[axis]) {
                int direction = p[axis] < q[axis] ? 1 : -1;
                auto next = p;
                next[axis] += direction;
                int side = axis * 2 + (direction > 0);
                weight *= edge(p, side) * edge(next, side ^ 1);
                p = next;
            }
        }
        return weight;
    };
    for (int y = 0; y < int(s.workHeight); ++y)
        for (int x = 0; x < int(s.workWidth); ++x) {
            glm::ivec2 p(x, y);
            double sum = 0, weights = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    auto q = p + glm::ivec2(dx, dy);
                    if (!valid(q))
                        continue;
                    double w =
                        std::exp(-double(dx * dx + dy * dy) / 4) * .5 * (path(p, q, 0) + path(p, q, 1));
                    sum += s.raw[index(q)] * w;
                    weights += w;
                }
            double expected = s.depth[index(p)] > 0 ? sum / weights : 1;
            // Compact storage quantises the result once to R8 UNORM.
            near(s.filtered[index(p)], expected, .5 / 255 + 2e-5,
                 "Tiled filter preserves edge-aware 5x5 visibility");
        }
}
void compare(const HdrSnapshot &a, const HdrSnapshot &b, const char *message) {
    require(a.pixels.size() == b.pixels.size(), "Matching HDR dimensions");
    for (size_t p = 0; p < a.pixels.size(); ++p)
        for (int c = 0; c < 3; ++c)
            near(a.pixels[p][c], b.pixels[p][c], 2e-4 + 3e-3 * std::abs(a.pixels[p][c]), message);
}
HdrSnapshot draw(Renderer &r, const Camera &c, SceneSettings s, GtaoMode mode) {
    s.gtao.mode = mode;
    require(r.render(c, s), "AO render succeeds");
    return r.readHdrSnapshot();
}
GtaoSnapshot snapshot(Renderer &r, const Camera &c, SceneSettings s, GtaoMode mode) {
    s.gtao.mode = mode;
    require(r.render(c, s), "AO render succeeds");
    auto a = r.readGtaoSnapshot();
    finite(a);
    return a;
}
void referenceCheck(const ao_fixture::Scene &scene, const Camera &c, const GtaoSnapshot &full,
                    const GtaoSnapshot &half) {
    auto inv = glm::inverse(c.viewProjection(float(full.width) / full.height));
    double error = 0, halfError = 0;
    uint32_t count = 0;
    for (uint32_t y = 4; y + 4 < full.height; y += 3)
        for (uint32_t x = 4; x + 4 < full.width; x += 3) {
            glm::vec4 q =
                inv * glm::vec4((x + .5f) / full.width * 2 - 1, (y + .5f) / full.height * 2 - 1, 1, 1);
            auto d = glm::normalize(glm::vec3(q) / q.w);
            float distance = 1e9f;
            int nearest = -1;
            for (int t = 0; t < 4; ++t)
                if (ao_fixture::hit(scene.triangles[t], glm::vec3(c.local), d, distance))
                    nearest = t;
            if (nearest < 0)
                continue;
            auto p = glm::vec3(c.local) + distance * d;
            near(full.depth[size_t(y) * full.width + x], glm::dot(p - glm::vec3(c.local), c.forward()), 2e-4,
                 "Prepared depth matches independent ray/geometry view distance");
            // Well observed floor contact region; avoid screen/geometry boundaries
            // and the exact corner.
            if (nearest >= 2 || std::abs(p.x) > .8f || p.z < -1.92f || p.z > -.8f)
                continue;
            auto reference = ao_fixture::reference(scene.triangles, p, {0, 1, 0}, 1);
            auto i = size_t(y) * full.width + x;
            error += std::abs(reference - full.pixels[i].x);
            halfError += std::abs(half.pixels[i].x - full.pixels[i].x);
            ++count;
        }
    require(count >= 20, "Ray reference covers contact region");
    std::cout << "Ray AO MAE=" << error / count << " Half/Full MAE=" << halfError / count
              << " samples=" << count << '\n';
    require(error / count <= .10, "GTAO agrees with ray-sampled contact visibility");
    require(halfError / count <= .05, "Half reconstruction agrees with Full interiors");
}
uint64_t run(uint32_t variant, uint64_t smallBudget = 0) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 129;
    o.height = 97;
    o.terrainGrid = {16, .5f};
    o.terrainCapacity = 16;
    o.sceneCapacity.instances = 32;
    o.sceneCapacity.meshes = 8;
    o.forceNoHiZ = variant == 1;
    o.forceWideGtao = variant == 1;
    o.forceFixedMaterials = variant == 1;
    o.forceFixedIndirect = variant == 1;
    o.forceNoGtao = variant == 2;
    o.gtaoBudgetBytes = variant == 3 ? smallBudget : 512ull * 1024 * 1024;
    if (variant == 3) {
        require(smallBudget > 0, "Budget fixture measures real minimum image requirements");
        o.width = 1025;
        o.height = 513;
    }
    Renderer r(o);
    ao_fixture::Scene fixture(r);
    auto c = ao_fixture::camera();
    auto s = ao_fixture::settings();
    require(r.render(c, s), "Default frame renders without AO");
    require(r.gtaoStats().effective == GtaoMode::Off && !r.gtaoStats().timingsAvailable,
            "Default frame skips AO dispatches");
    if (variant >= 2) {
        auto off = draw(r, c, s, GtaoMode::Off), on = draw(r, c, s, GtaoMode::Half);
        compare(off, on, "Fallback preserves lighting");
        require(r.gtaoStats().effective == GtaoMode::Off, "Fallback disables AO");
        require(r.gtaoStats().fallback == (variant == 2 ? GtaoFallback::Unsupported : GtaoFallback::Budget),
                "Fallback reason reported");
        if (variant == 3) {
            r.resizeOffscreen(1, 1);
            snapshot(r, c, s, GtaoMode::Half);
            require(r.gtaoStats().effective == GtaoMode::Half, "Budget fallback recovers at smaller extent");
            require(r.gtaoStats().allocatedBytes <= o.gtaoBudgetBytes, "Actual requirements fit budget");
            r.resizeOffscreen(o.width, o.height);
            draw(r, c, s, GtaoMode::Half);
            require(r.gtaoStats().fallback == GtaoFallback::Budget, "Budget enforced after repeated resize");
        }
        require(r.validationErrors() == 0, "Fallback validation clean");
        return 0;
    }
    auto full = snapshot(r, c, s, GtaoMode::Full), half = snapshot(r, c, s, GtaoMode::Half);
    referenceCheck(fixture, c, full, half);
    filterReference(full);
    filterReference(half);
    // Independent coverage oracle: compare Half representatives against every
    // native depth texel.
    for (uint32_t y = 0; y < half.workHeight; ++y)
        for (uint32_t x = 0; x < half.workWidth; ++x) {
            float nearest = std::numeric_limits<float>::infinity();
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    if (x * 2 + dx >= full.width || y * 2 + dy >= full.height)
                        continue;
                    float z = full.depth[(y * 2 + dy) * full.width + x * 2 + dx];
                    if (z > 0 && z < nearest) {
                        nearest = z;
                    }
                }
            auto index = size_t(y) * half.workWidth + x;
            near(half.depth[index], std::isfinite(nearest) ? nearest : 0, 1e-5,
                 "Half retains nearest valid native depth");
            uint32_t selectedX = x * 2 + (half.offsets[index] & 1),
                     selectedY = y * 2 + (half.offsets[index] >> 1);
            require(selectedX < full.width && selectedY < full.height,
                    "Representative stays inside odd native extent");
            near(full.depth[size_t(selectedY) * full.width + selectedX], half.depth[index], 1e-5,
                 "Representative identifies the retained depth (including rounded ties)");
        }

    auto on = draw(r, c, s, GtaoMode::Half), off = draw(r, c, s, GtaoMode::Off);
    double reduction = 0;
    for (size_t i = 0; i < on.pixels.size(); ++i)
        reduction += off.pixels[i].x - on.pixels[i].x;
    require(reduction > .1, "Indirect diffuse is actually occluded");
    for (auto visibility : {VisibilityMode::Direct, VisibilityMode::Indirect, VisibilityMode::HiZ}) {
        s.visibility = visibility;
        compare(on, draw(r, c, s, GtaoMode::Half), "AO independent of visibility submission mode");
    }
    s.visibility = VisibilityMode::Indirect;
    // No history: cut away and back, move occluder and restore within the current
    // frame.
    auto cut = c;
    cut.yaw = 2;
    draw(r, cut, s, GtaoMode::Half);
    compare(on, draw(r, c, s, GtaoMode::Half), "Camera cut has no AO history");
    fixture.removeWall();
    auto plane = snapshot(r, c, s, GtaoMode::Full);
    double average = 0;
    uint32_t visible = 0;
    for (size_t i = 0; i < plane.depth.size(); ++i)
        if (plane.depth[i] > 0) {
            require(plane.pixels[i].x >= .95f, "Plane silhouettes do not create dark halos");
            average += plane.pixels[i].x;
            ++visible;
        }
    require(visible > 100 && average / visible > .98, "Open plane remains unoccluded");
    fixture.restoreWall();
    compare(on, draw(r, c, s, GtaoMode::Half), "Restored occluder affects same frame");
    // Shift camera and scene together across a distant/negative world origin.
    c.chunk = {-100000, 100000};
    fixture.floor.position = fixture.wall.position = translated(worldPosition(c, o.terrainGrid), -c.local);
    r.updateInstance(fixture.floorInstance, fixture.floor);
    r.updateInstance(fixture.wallInstance, fixture.wall);
    compare(on, draw(r, c, s, GtaoMode::Half), "AO survives distant coordinate rebase");
    c.chunk = {};
    fixture.floor.position = fixture.wall.position = WorldPosition{};
    r.updateInstance(fixture.floorInstance, fixture.floor);
    r.updateInstance(fixture.wallInstance, fixture.wall);
    s.lighting.environmentEnabled = false;
    s.lighting.sunEnabled = true;
    s.lighting.sunLux = 1;
    LocalLight light;
    light.position.local = {0, 2, 1};
    light.intensity = 2;
    auto lh = r.createLight(light);
    compare(draw(r, c, s, GtaoMode::Off), draw(r, c, s, GtaoMode::Half),
            "Sun and local direct lighting unaffected");
    r.removeLight(lh);
    s = ao_fixture::settings();
    MeshMaterial m;
    m.metallic = 1;
    m.baseColour = {.7, .5, .3, 1};
    m.doubleSided = true;
    auto metal = r.createMaterial(m);
    fixture.setMaterial(metal);
    compare(draw(r, c, s, GtaoMode::Off), draw(r, c, s, GtaoMode::Half), "Environment specular unaffected");
    m.metallic = 0;
    m.baseColour = {0, 0, 0, 1};
    m.emissive = {2, 3, 4};
    auto emissive = r.createMaterial(m);
    fixture.setMaterial(emissive);
    s.lighting.environmentEnabled = false;
    compare(draw(r, c, s, GtaoMode::Off), draw(r, c, s, GtaoMode::Half), "Emissive unaffected");
    m.unlit = true;
    m.baseColour = {.3, .4, .5, 1};
    auto unlit = r.createMaterial(m);
    fixture.setMaterial(unlit);
    compare(draw(r, c, s, GtaoMode::Off), draw(r, c, s, GtaoMode::Half), "Unlit material unaffected");
    fixture.setMaterial(fixture.material);
    r.releaseMaterial(metal);
    r.releaseMaterial(emissive);
    r.releaseMaterial(unlit);
    s = ao_fixture::settings();
    if (variant == 0) {
        auto baselineOff = draw(r, c, s, GtaoMode::Off), baselineOn = draw(r, c, s, GtaoMode::Half);
        // Transparent diffuse must not inherit the opaque corner's visibility.
        auto quad = r.uploadMesh(makeQuad());
        MeshMaterial blend;
        blend.doubleSided = true;
        blend.alpha = AlphaMode::Blend;
        blend.baseColour = {.6, .4, .2, .4};
        auto blendMaterial = r.createMaterial(blend);
        MeshInstance overlay;
        overlay.mesh = quad;
        overlay.material = blendMaterial;
        overlay.position.local = {0, 1.2, 0};
        overlay.scale = {1.5, 1.5, 1};
        auto overlayInstance = r.createInstance(overlay);
        auto blendOff = draw(r, c, s, GtaoMode::Off), blendOn = draw(r, c, s, GtaoMode::Half);
        for (uint32_t y = 30; y < 70; ++y)
            for (uint32_t x = 35; x < 95; ++x) {
                auto i = size_t(y) * o.width + x;
                for (int channel = 0; channel < 3; ++channel)
                    near(blendOn.pixels[i][channel] - .6f * baselineOn.pixels[i][channel],
                         blendOff.pixels[i][channel] - .6f * baselineOff.pixels[i][channel], .001,
                         "Transparent contribution independent of background AO");
            }
        auto withBlend = snapshot(r, c, s, GtaoMode::Full);
        require(withBlend.depth == full.depth, "Transparency never writes AO depth");
        r.removeInstance(overlayInstance);
        r.releaseMaterial(blendMaterial);
        // Checker alpha holes expose the original depth, while opaque texels write
        // nearer depth.
        TextureData texture;
        texture.width = texture.height = 4;
        texture.levels = 3;
        texture.layers = 1;
        texture.format = TextureFormat::Srgb8;
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                texture.bytes.insert(texture.bytes.end(), {255, 255, 255, uint8_t((x + y) % 2 ? 255 : 0)});
        for (uint32_t levelSize : {2u, 1u})
            for (uint32_t i = 0; i < levelSize * levelSize; ++i)
                texture.bytes.insert(texture.bytes.end(), {255, 255, 255, 128});
        auto textureHandle = r.uploadTexture(texture);
        MeshMaterial mask;
        mask.doubleSided = true;
        mask.alpha = AlphaMode::Mask;
        mask.colourTexture = textureHandle;
        auto maskMaterial = r.createMaterial(mask);
        overlay.material = maskMaterial;
        overlayInstance = r.createInstance(overlay);
        auto masked = snapshot(r, c, s, GtaoMode::Full);
        uint32_t changed = 0, holes = 0;
        for (uint32_t y = 30; y < 70; ++y)
            for (uint32_t x = 35; x < 95; ++x) {
                auto i = size_t(y) * o.width + x;
                if (std::abs(masked.depth[i] - full.depth[i]) < 1e-5)
                    ++holes;
                else {
                    require(masked.depth[i] < full.depth[i], "Mask opaque texels write nearer depth");
                    ++changed;
                }
            }
        require(changed > 100 && holes > 100, "Mask fixture contains opaque texels and holes");
        auto maskHalf = snapshot(r, c, s, GtaoMode::Half);
        finite(maskHalf);
        filterReference(masked);
        filterReference(maskHalf);
        r.removeInstance(overlayInstance);
        r.releaseMaterial(maskMaterial);
        r.releaseTexture(textureHandle);
        // Replace submitted resources before their slot is reused; shared
        // mesh/material retirement owns readers.
        overlay.scale = {.015, 1, 1};
        overlay.material = fixture.material;
        overlayInstance = r.createInstance(overlay);
        s.gtao.mode = GtaoMode::Half;
        r.render(c, s);
        r.removeInstance(overlayInstance);
        r.releaseMesh(quad);
        compare(baselineOn, draw(r, c, s, GtaoMode::Half), "Removed thin occluder leaves no stale AO");
        // Material AO below GTAO is retained without multiplying overlapping
        // occlusion.
        MeshMaterial authored;
        authored.doubleSided = true;
        authored.baseColour = {.6, .6, .6, 1};
        authored.ao = .5;
        auto authoredMaterial = r.createMaterial(authored);
        fixture.setMaterial(authoredMaterial);
        auto authoredOff = draw(r, c, s, GtaoMode::Off), authoredOn = draw(r, c, s, GtaoMode::Full);
        uint32_t compared = 0;
        for (size_t i = 0; i < full.pixels.size(); ++i)
            if (full.depth[i] > 0 && full.pixels[i].x > .6) {
                near(authoredOn.pixels[i].x, authoredOff.pixels[i].x, .001,
                     "Overlapping authored AO uses minimum visibility");
                ++compared;
            }
        require(compared > 100, "Authored AO isolation includes lit surfaces");
        fixture.setMaterial(fixture.material);
        r.releaseMaterial(authoredMaterial);
        // Independent Hi-Z capability override must not disable GTAO is covered by
        // variant 1.
    }
    // Mix active and fading depths within the same image. A filter tile must not
    // discard a neighbour just because its centre has crossed a fade threshold.
    fixture.floor.scale = fixture.wall.scale = glm::vec3(40);
    r.updateInstance(fixture.floorInstance, fixture.floor);
    r.updateInstance(fixture.wallInstance, fixture.wall);
    auto transition = c;
    transition.local *= 40;
    s.gtao.radius = 5;
    for (auto mode : {GtaoMode::Full, GtaoMode::Half}) {
        auto a = snapshot(r, transition, s, mode);
        filterReference(a);
        uint32_t fading = 0, occluded = 0;
        for (auto z : a.depth)
            fading += z > 100 && z < 200;
        for (auto p : a.pixels)
            occluded += p.x < .99f;
        require(fading > 100 && occluded > 0, "Mixed fade fixture retains visible local occlusion");
    }
    // Keep substantial screen coverage beyond the fade, including partial groups,
    // even with the maximum radius selected.
    // Debug geometry must remain visible even when shaded visibility skips resolve.
    fixture.floor.scale = fixture.wall.scale = glm::vec3(1000);
    r.updateInstance(fixture.floorInstance, fixture.floor);
    r.updateInstance(fixture.wallInstance, fixture.wall);
    auto far = c;
    far.local *= 1000;
    for (auto mode : {GtaoMode::Full, GtaoMode::Half}) {
        auto faded = snapshot(r, far, s, mode);
        uint32_t surfaces = 0;
        for (auto z : faded.depth)
            surfaces += z > 200;
        require(surfaces > faded.depth.size() / 4, "Far fixture contains substantial geometry");
        for (auto p : faded.pixels)
            near(p.x, 1, 0, "Far surfaces fade to unoccluded");
        filterReference(faded);
        s.gtao.diagnostic = GtaoDiagnostic::Normals;
        auto normals = snapshot(r, far, s, mode);
        uint32_t coloured = 0;
        for (auto p : normals.pixels)
            coloured += p.x < .9f || p.y < .9f || p.z < .9f;
        require(coloured > normals.pixels.size() / 4, "Far normals bypass lighting fade");
        s.gtao.diagnostic = GtaoDiagnostic::Edges;
        auto edges = snapshot(r, far, s, mode);
        auto scale = mode == GtaoMode::Full ? 1u : 2u;
        uint32_t checked = 0;
        for (uint32_t y = 0; y < edges.height; ++y)
            for (uint32_t x = 0; x < edges.width; ++x) {
                auto i = size_t(y / scale) * edges.workWidth + x / scale;
                if (edges.depth[i] <= 0)
                    continue;
                // Half representatives can cover sky; only use known surface pixels.
                if (normals.pixels[size_t(y) * edges.width + x] == glm::vec4(1))
                    continue;
                uint32_t packed = uint32_t(std::lround(edges.edges[i] * 255));
                float sum = 0;
                for (int shift = 0; shift < 8; shift += 2)
                    sum += float((packed >> shift) & 3) / 3;
                near(edges.pixels[size_t(y) * edges.width + x].x, 1 - sum / 4, .5 / 255 + 1e-5,
                     "Far edge diagnostic remains available");
                ++checked;
            }
        require(checked > 100, "Far edge diagnostic covers surfaces");
        s.gtao.diagnostic = GtaoDiagnostic::Shaded;
    }
    fixture.floor.scale = fixture.wall.scale = glm::vec3(1);
    r.updateInstance(fixture.floorInstance, fixture.floor);
    r.updateInstance(fixture.wallInstance, fixture.wall);
    s = ao_fixture::settings();
    uint64_t onePixelBytes = 0;
    for (auto size :
         {glm::uvec2(1, 1), glm::uvec2(1, 33), glm::uvec2(33, 1), glm::uvec2(63, 31), glm::uvec2(129, 97)}) {
        r.resizeOffscreen(size.x, size.y);
        for (auto mode : {GtaoMode::Full, GtaoMode::Half}) {
            auto a = snapshot(r, c, s, mode);
            filterReference(a);
            if (size.x == 1 && size.y == 1 && mode == GtaoMode::Half)
                onePixelBytes = r.gtaoStats().allocatedBytes;
            auto size2 = gtaoExtent(size.x, size.y, mode);
            require(a.workWidth == size2.x && a.workHeight == size2.y, "Working resolution correct");
        }
    }
    // Sky is unoccluded, and denoising cannot preserve a vanished surface.
    c.pitch = 1.4f;
    auto sky = snapshot(r, c, s, GtaoMode::Half);
    for (auto p : sky.pixels)
        near(p.x, 1, 0, "Sky visibility one");
    c = ao_fixture::camera();
    s.wireframe = true;
    draw(r, c, s, GtaoMode::Half);
    require(r.gtaoStats().fallback == GtaoFallback::Wireframe, "Wireframe fallback");
    s.wireframe = false;
    for (auto diagnostic :
         {GtaoDiagnostic::Visibility, GtaoDiagnostic::Raw, GtaoDiagnostic::Normals, GtaoDiagnostic::Edges}) {
        s.gtao.diagnostic = diagnostic;
        auto a = snapshot(r, c, s, GtaoMode::Half);
        s.output.ev100 = 10;
        auto b = snapshot(r, c, s, GtaoMode::Half);
        require(a.pixels == b.pixels, "AO diagnostics independent of exposure");
    }
    require(r.validationErrors() == 0, "GTAO synchronization validation clean");
    return onePixelBytes;
}
void terrain(bool streamed) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 64};
    o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = o.streaming.hierarchical = streamed;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 64;
    o.streaming.gpuBytes = 40 * terrainMiB;
    o.streaming.hostBytes = 64 * terrainMiB;
    o.streaming.diagnosticFrameSeconds = 1. / 60.;
    Renderer r(o);
    TerrainSettings t;
    t.heightScale = 8;
    if (streamed)
        r.setTerrainSettings(t);
    else {
        TerrainRequest request;
        request.settings = t;
        r.requestTerrain(request);
    }
    Camera c;
    c.local = {8, 12, 20};
    c.pitch = -.35f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.viewRadius = 1;
    s.gtao.mode = GtaoMode::Half;
    auto settle = [&] {
        int stable = 0;
        for (int i = 0; i < 2000 && stable < 8; ++i) {
            r.render(c, s);
            const auto &st = r.terrainStreamingStats();
            stable = (!streamed || (!st.pending && !st.detailPending && !st.ioJobs)) ? stable + 1 : 0;
        }
        require(stable == 8, "Small AO terrain fixture settles");
    };
    settle();
    require(r.terrainRenderStats().selected > 0, "AO terrain has visible patches");
    s.freezeLod = true;
    s.visibility = VisibilityMode::Direct;
    auto direct = draw(r, c, s, GtaoMode::Half);
    s.visibility = VisibilityMode::HiZ;
    compare(direct, draw(r, c, s, GtaoMode::Half), "Terrain AO uses identical morph/clipping depth in HiZ");
    finite(r.readGtaoSnapshot());
    if (streamed) {
        s.freezeLod = false;
        c.chunk = {-2, 1};
        for (int i = 0; i < 12; ++i) {
            r.render(c, s);
            finite(r.readGtaoSnapshot());
        }
        settle();
    }
    require(r.validationErrors() == 0, "Terrain AO validation clean");
}
} // namespace
int main() {
    try {
        const auto minimumBudget = run(0);
        run(1);
        run(2);
        run(3, minimumBudget);
        terrain(false);
        terrain(true);
        std::cout << "GTAO GPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>
using namespace ow;
namespace {
void compare(const HdrSnapshot &a, const HdrSnapshot &b, const char *message) {
    require(a.pixels.size() == b.pixels.size(), "Shadow snapshot sizes");
    for (size_t i = 0; i < a.pixels.size(); ++i)
        for (int c = 0; c < 3; ++c)
            near(a.pixels[i][c], b.pixels[i][c], 2e-4 + 3e-3 * std::abs(a.pixels[i][c]), message);
}
void meshes(uint32_t fallback) {
    RendererOptions o;
    o.ui = false;
    o.shadowCapacity.resolution = 256;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {16, .5f};
    o.terrainCapacity = 16;
    o.shadowCapacity.casters = 128;
    o.forceManualShadowFiltering = fallback == 1;
    o.forceDirectShadows = fallback == 2;
    o.forceFixedMaterials = fallback == 3;

    Renderer r(o);
    SceneSettings s;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    s.terrainVisible = false;
    s.lighting.environmentEnabled = false;
    Camera c;
    auto quad = r.uploadMesh(makeQuad());
    MeshMaterial m;
    m.doubleSided = true;
    m.baseColour = {.5, .5, .5, 1};
    auto material = r.createMaterial(m);
    MeshInstance wall;
    wall.mesh = quad;
    wall.material = material;
    wall.position.local = {0, 0, -8};
    wall.scale = {5, 5, 1};
    r.createInstance(wall);
    MeshInstance blocker = wall;
    blocker.position.local = {-1, 1, -5};
    blocker.scale = {1, 1, 1};
    auto handle = r.createInstance(blocker);
    s.lighting.sunDirection = glm::normalize(glm::vec3(-.4, .4, 1));
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto off = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    for (int i = 0; i < 19; ++i)
        r.render(c, s);
    auto image = r.readHdrSnapshot();
    size_t dark = 0;
    for (size_t i = 0; i < image.pixels.size(); ++i)
        if (image.pixels[i].x < off.pixels[i].x * .7f && off.pixels[i].x > .01f)
            ++dark;
    require(dark > 10, "Sun blocker creates a shadow");
    require(r.shadowStats().effective == ShadowMode::CSM, "CSM remains active on fallback paths");
    auto snapshot = r.readShadowSnapshot();
    require(snapshot.uniform.options.y == 5, "Four near cascades and a far cascade");
    require(std::any_of(snapshot.depth.begin(), snapshot.depth.end(), [](float d) { return d > 0; }),
            "Depth maps contain casters");
    r.render(c, s);
    compare(image, r.readHdrSnapshot(), "Stationary CSM is deterministic");
    for (int i = 0; i < 3; ++i) {
        s.lighting.sunDirection = glm::normalize(glm::vec3(.2f + i * .2f, .1f + i * .1f, 1));
        r.render(c, s);
        auto moved = r.readShadowSnapshot();
        require(moved.uniform.matrices[0] != snapshot.uniform.matrices[0],
                "Moving sun immediately updates transforms");
        snapshot = std::move(moved);
    }
    r.removeInstance(handle);
    r.render(c, s);
    auto removed = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    compare(removed, r.readHdrSnapshot(), "Removed caster leaves no stale shadow");
    r.resizeOffscreen(65, 49);
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    r.readHdrSnapshot();
    require(r.validationErrors() == 0, "Shadow Vulkan validation");
}
void offscreenAndMaterials() {
    RendererOptions o;
    o.ui = false;
    o.shadowCapacity.resolution = 256;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.shadowCapacity.casters = 32;
    Renderer r(o);
    SceneSettings s;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    s.terrainVisible = false;
    s.lighting.environmentEnabled = false;
    s.visibility = VisibilityMode::HiZ;
    s.lighting.sunDirection = glm::normalize(glm::vec3(-1, .1f, 1));
    Camera c;
    auto quad = r.uploadMesh(makeQuad());
    MeshMaterial m;
    m.doubleSided = true;
    auto material = r.createMaterial(m);
    MeshInstance wall;
    wall.mesh = quad;
    wall.material = material;
    wall.position.local = {0, 0, -8};
    wall.scale = {5, 5, 1};
    auto wallHandle = r.createInstance(wall);
    MeshInstance blocker = wall;
    blocker.position.local = {-6, 0, -4};
    blocker.scale = {.5f, .5f, 1};
    auto handle = r.createInstance(blocker);
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto off = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    for (int i = 0; i < 18; ++i)
        r.render(c, s);
    auto shadow = r.readHdrSnapshot();
    uint32_t dark = 0;
    for (size_t i = 0; i < off.pixels.size(); ++i)
        if (shadow.pixels[i].x < off.pixels[i].x * .8f && off.pixels[i].x > .01f)
            ++dark;
    require(dark > 5, "Camera-culled offscreen caster shadows visible receiver");
    require(r.visibilityStats().frustumRejected > 0, "Offscreen caster is excluded from main view");
    blocker.scale.x = -.5f;
    r.updateInstance(handle, blocker);
    r.render(c, s);
    compare(shadow, r.readHdrSnapshot(), "Mirrored caster preserves geometric shadow");
    r.render(c, s);
    compare(shadow, r.readHdrSnapshot(), "Repeated frame retains mirrored shadow");
    // A transparent receiver must sample its own position, without a camera depth
    // dependency.
    m.alpha = AlphaMode::Blend;
    m.baseColour.a = .5f;
    wall.material = r.createMaterial(m);
    r.updateInstance(wallHandle, wall);
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto transparentOff = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    auto transparentOn = r.readHdrSnapshot();
    uint32_t received = 0;
    for (size_t i = 0; i < transparentOn.pixels.size(); ++i)
        received +=
            transparentOff.pixels[i].x > .01f && transparentOn.pixels[i].x < transparentOff.pixels[i].x * .8f;
    require(received > 5, "Blended surfaces receive offscreen-caster shadows at "
                          "their own positions");
    wall.material = material;
    r.updateInstance(wallHandle, wall);
    m.alpha = AlphaMode::Opaque;
    m.baseColour.a = 1;
    c.chunk = {1000000000ll, -1000000000ll};
    auto origin = worldPosition(c, o.terrainGrid);
    wall.position = translated(origin, {0, 0, -8});
    blocker.position = translated(origin, {-6, 0, -4});
    r.updateInstance(wallHandle, wall);
    r.updateInstance(handle, blocker);
    r.render(c, s);
    auto distant = r.readHdrSnapshot();
    double error = 0;
    for (size_t i = 0; i < shadow.pixels.size(); ++i)
        error += std::abs(shadow.pixels[i].x - distant.pixels[i].x);
    require(error / shadow.pixels.size() < .03,
            "Distant world translation changes only subtexel shadow placement");
    r.removeInstance(handle);
    m.alpha = AlphaMode::Blend;
    m.baseColour.a = .5f;
    blocker.material = r.createMaterial(m);
    blocker.position = translated(origin, {-1, 0, -4});
    auto blend = r.createInstance(blocker);
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto noShadow = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    compare(noShadow, r.readHdrSnapshot(), "Blended geometry receives without casting solid shadows");
    r.removeInstance(blend);
    TextureData texture;
    texture.format = TextureFormat::Srgb8;
    texture.bytes = {255, 255, 255, 0};
    auto transparent = r.uploadTexture(texture);
    m.alpha = AlphaMode::Mask;
    m.baseColour.a = 1;
    m.colourTexture = transparent;
    blocker.material = r.createMaterial(m);
    auto mask = r.createInstance(blocker);
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto hole = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    compare(hole, r.readHdrSnapshot(), "Alpha-mask holes remain holes in shadows");
    s.lighting.sunDirection = glm::normalize(glm::vec3(-1, 0, 1));
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto horizon = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    compare(horizon, r.readHdrSnapshot(), "Horizon shadows fade to unshadowed");
    require(r.readShadowSnapshot().uniform.sun.w == 0, "Horizon coverage explicitly fades to zero");
    s.lighting.sunEnabled = false;
    s.lighting.environmentEnabled = true;
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto environment = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::CSM;
    r.render(c, s);
    compare(environment, r.readHdrSnapshot(), "Shadows preserve environment lighting");
    r.removeInstance(mask);
    require(r.validationErrors() == 0, "Offscreen/material shadow validation");
}

void cascadeRanges() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.shadowCapacity.resolution = 512;
    o.shadowCapacity.casters = 32;
    Renderer r(o);
    Camera camera;
    SceneSettings s;
    s.terrainVisible = false;
    s.shadows.diagnostic = 1;
    s.lighting.sunDirection = glm::normalize(glm::vec3(-.4, .4, 1));
    MeshMaterial material;
    material.doubleSided = true;
    auto m = r.createMaterial(material);
    auto quad = r.uploadMesh(makeQuad());
    MeshInstance wall;
    wall.mesh = quad;
    wall.material = m;
    auto receiver = r.createInstance(wall), caster = r.createInstance(wall);
    for (double distance :
         {20., 49., 50., 51., 149., 150., 151., 399., 400., 401., 999., 1000., 1001., 4000.}) {
        wall.position.local = {0, 0, -distance};
        wall.scale = {float(distance * .4), float(distance * .4), 1};
        r.updateInstance(receiver, wall);
        auto blocker = wall;
        blocker.position.local = {-.08 * distance, .08 * distance, -.8 * distance};
        blocker.scale = {float(distance * .06), float(distance * .06), 1};
        r.updateInstance(caster, blocker);
        s.shadows.mode = ShadowMode::Off;
        r.render(camera, s);
        auto off = r.readHdrSnapshot();
        s.shadows.mode = ShadowMode::CSM;
        r.render(camera, s);
        auto on = r.readHdrSnapshot();
        size_t dark = 0;
        for (size_t i = 0; i < on.pixels.size(); ++i)
            dark += off.pixels[i].x > .99f && on.pixels[i].x < .7f;
        require(dark > 5, "Sun shadows remain present through every cascade "
                          "boundary and far mountains");
        if (distance == 4000) {
            auto depth = r.readShadowSnapshot();
            const size_t first = 4ull * 512 * 512;
            require(
                std::any_of(depth.depth.begin() + first, depth.depth.end(), [](float v) { return v > 0; }),
                "Far cascade contains rasterized geometry");
        }
    }
    require(r.validationErrors() == 0, "Cascade transitions validate");
}
void unsupportedDepth() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 33;
    o.height = 25;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.forceNoShadowDepth = true;
    o.shadowCapacity.resolution = 128;
    o.shadowCapacity.casters = 32;
    Renderer r(o);
    Camera c;
    SceneSettings s;
    s.terrainVisible = false;
    r.render(c, s);
    auto unsupported = r.readHdrSnapshot();
    require(r.shadowStats().effective == ShadowMode::Off &&
                r.shadowStats().fallback == ShadowFallback::UnsupportedDepth,
            "Unavailable sampled D32 explicitly disables shadows");
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    compare(unsupported, r.readHdrSnapshot(), "Unsupported depth retains unshadowed rendering");
    require(r.validationErrors() == 0, "Dummy depth descriptor validates");
}
void pressure() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.shadowCapacity.resolution = 128;
    o.shadowCapacity.casters = 1;
    Renderer r(o);
    SceneSettings s;
    s.terrainVisible = false;
    s.shadows.nearDistance = 100;
    Camera c;
    MeshMaterial material;
    material.doubleSided = true;
    auto m = r.createMaterial(material);
    auto quad = r.uploadMesh(makeQuad());
    MeshInstance wall;
    wall.mesh = quad;
    wall.material = m;
    wall.position.local = {0, 0, -8};
    wall.scale = {5, 5, 1};
    r.createInstance(wall);
    wall.position.local = {-1, 1, -5};
    wall.scale = {1, 1, 1};
    r.createInstance(wall);
    for (int i = 0; i < 4; ++i)
        r.render(c, s);
    require(r.shadowStats().overflow > 0, "Caster exhaustion is reported");
    auto snapshot = r.readShadowSnapshot();
    require(snapshot.uniform.options.z == 0, "Incomplete caster lists are never sampled");
    auto on = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    compare(on, r.readHdrSnapshot(), "Exhausted cascades use unshadowed sunlight");
    require(r.validationErrors() == 0, "Capacity failure validation");
    o.shadowCapacity.bytes = 1;
    rejects([&] { Renderer rejected(o); }, "Insufficient startup shadow budget is rejected");
}
HdrSnapshot commandImage(bool direct, bool manual) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.shadowCapacity.resolution = 256;
    o.shadowCapacity.casters = 512;
    o.forceDirectShadows = direct;
    o.forceManualShadowFiltering = manual;
    Renderer r(o);
    SceneSettings s;
    s.terrainVisible = false;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    s.shadows.diagnostic = 1;
    s.visibility = VisibilityMode::HiZ;
    s.lighting.sunDirection = glm::normalize(glm::vec3(-.4, .5, 1));
    Camera c;
    MeshMaterial material;
    material.doubleSided = true;
    auto m = r.createMaterial(material);
    auto quad = r.uploadMesh(makeQuad());
    MeshInstance instance;
    instance.mesh = quad;
    instance.material = m;
    instance.position.local = {0, 0, -20};
    instance.scale = {20, 20, 1};
    r.createInstance(instance);
    for (int i = 0; i < 193; ++i) {
        instance.position.local = {double(i % 17) - 8, double(i / 17) - 5, -10 - double(i % 3)};
        instance.scale = {.2, .2, 1};
        r.createInstance(instance);
    }
    for (int frame = 0; frame < 4; ++frame)
        r.render(c, s);
    auto result = r.readHdrSnapshot();
    require(r.validationErrors() == 0, "Compact command ranges validate");
    return result;
}
void commandRanges() {
    auto indirect = commandImage(false, false);
    compare(indirect, commandImage(true, false),
            "Compact indirect matches direct rendering, including after Hi-Z "
            "counted draws");
    compare(indirect, commandImage(false, true), "Legacy manual-filter override preserves unified PCF results");
}
// An isolated plane cannot shadow itself. Exercise a grazing light, subtexel
// translations and different cascade footprints with the production PCF path.
void receiverPlanes() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.shadowCapacity.resolution = 256;
    Renderer r(o);
    SceneSettings s;
    s.terrainVisible = false;
    s.shadows.diagnostic = 1;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    MeshMaterial material;
    material.doubleSided = true;
    MeshInstance plane;
    plane.mesh = r.uploadMesh(makeQuad());
    plane.material = r.createMaterial(material);
    plane.scale = {2000, 2000, 1};
    plane.position.local = {0, 0, -80};
    r.createInstance(plane);
    Camera c;
    for (float grazing : {.7f, .1f, .02f}) {
        s.lighting.sunDirection = glm::normalize(glm::vec3(1, .3f, grazing));
        for (double distance : {8., 80., 240.})
            for (double subtexel : {0., .03125, .137}) {
                c.local = {subtexel, subtexel * .7, -80 + distance};
                r.render(c, s);
                auto hdr = r.readHdrSnapshot();
                uint32_t dark = 0;
                for (const auto &pixel : hdr.pixels)
                    dark += pixel.x < .99f;
                if (dark)
                    std::cerr << "Plane grazing=" << grazing << " distance=" << distance
                              << " phase=" << subtexel << " dark=" << dark
                              << " centre=" << hdr.pixels[hdr.pixels.size() / 2].x << '\n';
                require(dark == 0, "Isolated plane stays lit at grazing slopes and "
                                   "subtexel positions");
            }
    }
    require(r.validationErrors() == 0, "Receiver plane Vulkan validation");
}
void steepTerrain() {
    RendererOptions o;
    o.ui = false;
    o.shadowCapacity.resolution = 256;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {64, .5f};
    o.terrainCapacity = 64;
    o.lod = {true, 8, 32};
    o.lod.triangleBudget = 524288; // Bounded draw storage for this smaller 64-cell fixture.
    o.normalSubdivisions = 1;
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 256;
    o.streaming.gpuBytes = 80 * terrainMiB;
    o.streaming.hostBytes = 128 * terrainMiB;
    o.streaming.shadowCasterBytes = 4 * terrainMiB;
    o.shadowCapacity.casters = 32768;
    Renderer r(o);
    TerrainSettings terrain;
    terrain.heightScale = 128;
    terrain.wavelength = 128;
    terrain.regionWavelength = 4096;
    terrain.erosionWavelength = 64;
    r.setTerrainSettings(terrain);
    SceneSettings s;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    s.viewRadius = 4;
    s.shadows.diagnostic = 1;
    s.lighting.sunDirection = {0, 1, 0};
    s.terrainLodQuality = false;
    Camera c;
    c.chunk = {15, -8};
    c.local = {8, 100, 24};
    c.pitch = -1.0f;
    bool ready = false;
    for (uint32_t i = 0; i < 1600; ++i) {
        r.render(c, s);
        auto stats = r.terrainStreamingStats();
        if (i > 24 && stats.pending == 0 && stats.shadowMissing == 0) {
            ready = true;
            break;
        }
    }
    require(ready, "Steep terrain coverage settles");
    auto heights = r.readStreamedHeights(c.chunk);
    float lo = INFINITY, hi = -INFINITY;
    for (auto h : heights) {
        lo = std::min(lo, h.y);
        hi = std::max(hi, h.y);
    }
    std::cout << "Terrain relief " << lo << ".." << hi << "\n";
    c.local.y = hi + 80;
    require(hi - lo > 3, "Owned fixture contains significant relief");
    uint64_t minimumTriangles = ~0ull, maximumTriangles = 0;
    for (double distance : {0., 100., 400.}) {
        Camera observer = c;
        observer.local.z += distance;
        // Select a fresh partition at each observer, then freeze it for the
        // paired shadow comparison. Moving a frozen observer alone preserves keys.
        r.setTerrainLodReviewState({observer, 1, 0, true});
        s.freezeLod = true;
        s.shadows.mode = ShadowMode::Off;
        r.render(c, s);
        auto off = r.readHdrSnapshot();
        s.shadows.mode = ShadowMode::CSM;
        for (uint32_t i = 0; i < 20; ++i)
            r.render(c, s);
        auto on = r.readHdrSnapshot();
        uint32_t terrainPixels = 0, dark = 0;
        for (size_t i = 0; i < off.pixels.size(); ++i)
            if (off.pixels[i].x > .99f) {
                ++terrainPixels;
                if (on.pixels[i].x < .9f)
                    ++dark;
            }
        std::cout << "Overhead terrain LOD " << distance << ": " << dark << '/' << terrainPixels
                  << " false shadows\n";
        require(terrainPixels > 1000, "Regression renders terrain rather than sky");
        require(dark <= 5, "Vertical sun cannot self-occlude a heightfield across camera LODs");
        auto count = r.terrainRenderStats().triangles;
        minimumTriangles = std::min(minimumTriangles, count);
        maximumTriangles = std::max(maximumTriangles, count);
    }
    require(maximumTriangles > minimumTriangles * 2, "Regression changes visible geometry LOD substantially");
    // The stable receiver must still receive genuine terrain occlusion.
    s.lighting.sunDirection = glm::normalize(glm::vec3(-1, .25f, .3f));
    for (uint32_t i = 0; i < 30; ++i)
        r.render(c, s);
    auto oblique = r.readHdrSnapshot();
    s.shadows.mode = ShadowMode::Off;
    r.render(c, s);
    auto unshadowed = r.readHdrSnapshot();
    uint32_t occluded = 0;
    for (size_t i = 0; i < oblique.pixels.size(); ++i)
        if (unshadowed.pixels[i].x > .99f && oblique.pixels[i].x < .5f)
            ++occluded;
    std::cout << "Oblique terrain shadow pixels: " << occluded << '\n';
    require(occluded > 20, "Shared terrain receivers retain real oblique-sun shadows");
    require(r.validationErrors() == 0, "Steep terrain receiver validation");
}
void streamed() {
    RendererOptions o;
    o.ui = false;
    o.shadowCapacity.resolution = 256;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {16, .5f};
    o.terrainCapacity = 16;
    o.lod = {true, 8, 64};
    o.normalSubdivisions = 1;
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 16;
    o.streaming.gpuBytes = 40 * terrainMiB;
    o.streaming.hostBytes = 64 * terrainMiB;
    o.streaming.shadowCasterBytes = 4 * terrainMiB;
    o.shadowCapacity.casters = 4096;
    Renderer r(o);
    TerrainSettings settings;
    settings.heightScale = 4;
    settings.wavelength = 64;
    r.setTerrainSettings(settings);
    SceneSettings s;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    s.viewRadius = 2;
    Camera c;
    c.local = {2, 12, 2};
    c.pitch = -.8f;
    bool ready = false;
    for (int i = 0; i < 1200; ++i) {
        r.render(c, s);
        auto stats = r.terrainStreamingStats();
        if (i > 4 && stats.pending == 0 && stats.shadowMissing == 0 && stats.shadowRequested > 0) {
            ready = true;
            break;
        }
    }
    require(ready, "Bounded offscreen caster demand becomes resident");
    require(r.terrainStreamingStats().shadowSlots > 0, "Additional shadow residency allocated");
    auto previous = r.readShadowSnapshot();
    c.yaw = 1.4f;
    r.render(c, s);
    r.readHdrSnapshot();
    require(r.readShadowSnapshot().uniform.matrices[0] != previous.uniform.matrices[0],
            "Camera turn refits the stable cascade centre");
    c.chunk = {1000, -1000};
    r.render(c, s);
    r.readHdrSnapshot();
    auto teleported = r.readShadowSnapshot();
    require(std::any_of(teleported.coverage.begin(), teleported.coverage.end(),
                        [](uint32_t c) { return c == 0; }),
            "Teleport exposes missing coverage");

    require(r.validationErrors() == 0, "Streamed shadow resource lifetime validation");
}

void terrain() {
    RendererOptions o;
    o.ui = false;
    o.shadowCapacity.resolution = 256;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 64};
    o.shadowCapacity.casters = 512;
    Renderer r(o);
    TerrainRequest request;
    request.settings.heightScale = 8;
    request.settings.wavelength = 64;
    r.requestTerrain(request);
    Camera c;
    c.local = {8, 20, 24};
    c.pitch = -.65f;
    SceneSettings s;
    s.shadows.nearDistance = 100;
    s.shadows.farDistance = 512;
    for (int i = 0; i < 4; ++i)
        r.render(c, s);
    auto image = r.readHdrSnapshot();
    for (auto pixel : image.pixels)
        require(std::isfinite(pixel.x), "Terrain shadow output finite");
    s.eroded = false;
    r.render(c, s);
    r.readHdrSnapshot();
    require(r.shadowStats().casters > 0, "Terrain submits independent casters");
    auto previous = r.readShadowSnapshot();
    s.terrainVisible = false;
    r.render(c, s);
    r.readHdrSnapshot();
    auto hidden = r.readShadowSnapshot();
    require(std::all_of(hidden.depth.begin(), hidden.depth.end(), [](float d) { return d == 0; }),
            "Hiding terrain immediately clears all shadow maps");
    require(r.validationErrors() == 0, "Terrain shadow validation");
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc > 1) {
            if (std::string(argv[1]) == "planes")
                receiverPlanes();
            else if (std::string(argv[1]) == "steep")
                steepTerrain();
            else if (std::string(argv[1]) == "commands")
                commandRanges();
            else if (std::string(argv[1]) == "streamed")
                streamed();
            else
                offscreenAndMaterials();
            return 0;
        }
        for (uint32_t i = 0; i < 4; ++i)
            meshes(i);
        cascadeRanges();
        unsupportedDepth();
        pressure();
        commandRanges();
        offscreenAndMaterials();
        terrain();
        streamed();
        receiverPlanes();
        steepTerrain();
        std::cout << "shadows gpu passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

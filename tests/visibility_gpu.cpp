#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>
using namespace ow;
namespace {
void compare(const HdrSnapshot &a, const HdrSnapshot &b, const char *message) {
    require(a.pixels.size() == b.pixels.size(), "matching extents");
    for (size_t i = 0; i < a.pixels.size(); ++i)
        for (int c = 0; c < 4; ++c)
            near(b.pixels[i][c], a.pixels[i][c], 2e-4 + 3e-3 * std::abs(a.pixels[i][c]), message);
}
HdrSnapshot render(Renderer &r, const Camera &c, SceneSettings s, VisibilityMode mode) {
    s.visibility = mode;
    require(r.render(c, s), "render visibility fixture");
    return r.readHdrSnapshot();
}
void pyramidReference(Renderer &r) {
    auto p = r.readVisibilityPyramid();
    for (uint32_t l = 1; l < p.levels.size(); ++l) {
        uint32_t w = std::max(1u, p.width >> l), h = std::max(1u, p.height >> l);
        std::vector<float> expected(size_t(w) * h, 1);
        // Independent reduction from every base pixel, not the preceding mip.
        for (uint32_t y = 0; y < p.height; ++y)
            for (uint32_t x = 0; x < p.width; ++x) {
                auto i = std::min(y >> l, h - 1) * w + std::min(x >> l, w - 1);
                expected[i] = std::min(expected[i], p.levels[0][y * p.width + x]);
            }
        for (size_t i = 0; i < expected.size(); ++i)
            near(p.levels[l][i], expected[i], 0, "HiZ covers all base pixels including odd tails");
    }
}
void run(uint32_t fallback) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 129;
    o.height = 97;
    o.terrainGrid = {16, .5f};
    o.terrainCapacity = 16;
    o.sceneCapacity.instances = 600;
    o.sceneCapacity.meshes = 8;
    o.visibilityCapacity = fallback == 3 ? 4 : fallback == 6 ? 302 : 1024;
    o.forceFixedIndirect = fallback == 1;
    o.forceSingleIndirect = fallback == 2;
    o.forceDirectVisibility = fallback == 4;
    o.forceNoHiZ = fallback == 5;
    o.forceFixedMaterials = fallback == 1;
    Renderer r(o);
    Camera camera;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.terrainVisible = false;
    s.output.ev100 = 0;
    s.opaqueDepthPrepass = true;
    auto mesh = r.uploadMesh(makeQuad());
    MeshMaterial m;
    m.unlit = true;
    m.baseColour = {.2f, .4f, .7f, 1};
    m.doubleSided = true;
    auto blue = r.createMaterial(m);
    m.baseColour = {.8f, .1f, .05f, 1};
    auto red = r.createMaterial(m);
    MeshInstance occluder;
    occluder.mesh = mesh;
    occluder.material = blue;
    occluder.position.local = {0, 0, -4};
    occluder.scale = {2, 2, 1};
    auto wall = r.createInstance(occluder);
    MeshInstance hidden = occluder;
    hidden.material = red;
    hidden.position.local.z = -8;
    hidden.scale = {.25f, .25f, 1};
    auto object = r.createInstance(hidden);
    std::vector<InstanceHandle> extras;
    for (int i = 0; i < 300; ++i) {
        auto q = hidden;
        q.position.local.x = (i % 15 - 7) * .12;
        q.position.local.y = (i / 15 - 10) * .1;
        extras.push_back(r.createInstance(q));
    }
    const auto reference = render(r, camera, s, VisibilityMode::Direct);
    compare(reference, render(r, camera, s, VisibilityMode::Indirect),
            "Indirect preserves image and record identity");
    for (int i = 0; i < 4; ++i)
        compare(reference, render(r, camera, s, VisibilityMode::HiZ), "Occlusion preserves visible image");
    if (fallback == 3)
        require(r.visibilityStats().fallback == VisibilityFallback::Capacity,
                "Capacity falls back without geometry loss");
    else if (fallback == 4)
        require(r.visibilityStats().effective == VisibilityMode::Direct,
                "Unsupported first instance fallback");
    else if (fallback == 5)
        require(r.visibilityStats().effective == VisibilityMode::Indirect, "Unsupported depth fallback");
    else {
        require(r.visibilityStats().effective == VisibilityMode::HiZ, "HiZ exercised");
        require(r.visibilityStats().occlusionRejected >= 300, "Fully hidden objects really rejected");
        require(r.visibilityStats().submitted >= 1, "Visible occluder retained");
        pyramidReference(r);
    }
    const auto &counts = r.visibilityStats();
    require(counts.countsAvailable && counts.completedFrame > 0 && counts.completedMode == counts.effective,
            "Completed counters retain originating frame and visibility mode");
    require(counts.selectedTriangles.meshes == 604 && counts.selectedTriangles.terrain == 0 &&
                counts.depthTriangles.meshes == 604 && counts.depthTriangles.terrain == 0,
            "Depth includes every selected opaque quad before occlusion");
    require(counts.colourTriangles.meshes == uint64_t(counts.submitted) * 2 &&
                counts.colourTriangles.terrain == 0,
            "Completed mesh triangle counts match actual submitted quads");
    if (counts.effective == VisibilityMode::HiZ)
        require(counts.colourTriangles.total() < counts.depthTriangles.total(),
                "HiZ reduces colour work while retaining depth work");
    // Remove most instances: stale compacted command tails must disappear
    // immediately.
    for (auto h : extras)
        r.removeInstance(h);
    extras.clear();
    // Remove an occluder; the formerly hidden object must appear in this frame.
    r.removeInstance(wall);
    auto direct = render(r, camera, s, VisibilityMode::Direct);
    compare(direct, render(r, camera, s, VisibilityMode::HiZ), "Same-frame reveal after occluder removal");
    require(direct.pixels[(o.height / 2) * o.width + o.width / 2].x > .1f, "Revealed object visible");
    if (fallback == 3)
        require(r.visibilityStats().effective == VisibilityMode::HiZ,
                "Capacity fallback recovers next frame");
    // A subpixel-wide gap straddles the centre sample at this odd resolution.
    auto gapWall = occluder;
    gapWall.scale = {1, 2, 1};
    gapWall.position.local.x = -1.01;
    auto left = r.createInstance(gapWall);
    gapWall.position.local.x = 1.01;
    auto right = r.createInstance(gapWall);
    auto gapReference = render(r, camera, s, VisibilityMode::Direct);
    compare(gapReference, render(r, camera, s, VisibilityMode::HiZ),
            "Thin gaps cannot become solid HiZ occluders");
    require(gapReference.pixels[(o.height / 2) * o.width + o.width / 2].x > .1f,
            "Thin gap exposes background geometry");
    r.removeInstance(left);
    r.removeInstance(right);
    // Masked holes and transparency must not become solid occluders.
    TextureData texture;
    texture.width = texture.height = 4;
    texture.levels = 3;
    texture.layers = 1;
    texture.format = TextureFormat::Srgb8;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            texture.bytes.insert(texture.bytes.end(), {255, 255, 255, uint8_t(x == 1 || x == 2 ? 0 : 255)});
    for (int n : {2, 1})
        for (int i = 0; i < n * n; ++i)
            texture.bytes.insert(texture.bytes.end(), {255, 255, 255, 128});
    m.baseColour = {.2f, .4f, .7f, 1};
    m.alpha = AlphaMode::Mask;
    m.colourTexture = r.uploadTexture(texture);
    occluder.material = r.createMaterial(m);
    wall = r.createInstance(occluder);
    for (auto size :
         {glm::uvec2(129, 97), glm::uvec2(63, 31), glm::uvec2(1, 33), glm::uvec2(33, 1), glm::uvec2(1, 1)}) {
        r.resizeOffscreen(size.x, size.y);
        auto ref = render(r, camera, s, VisibilityMode::Direct);
        compare(ref, render(r, camera, s, VisibilityMode::HiZ), "Masked holes / odd / one-pixel extents");
        if (r.visibilityStats().effective == VisibilityMode::HiZ)
            pyramidReference(r);
    }
    r.resizeOffscreen(129, 97);
    m.alpha = AlphaMode::Blend;
    m.baseColour.a = .35f;
    m.colourTexture = {};
    occluder.material = r.createMaterial(m);
    r.updateInstance(wall, occluder);
    for (float x : {0.f, -1.f, 2.f}) {
        hidden.position.local.x = x;
        hidden.scale = {-.5f, .3f, 1};
        hidden.rotation = glm::angleAxis(.2f, glm::vec3(0, 1, 0));
        r.updateInstance(object, hidden);
        compare(render(r, camera, s, VisibilityMode::Direct), render(r, camera, s, VisibilityMode::HiZ),
                "Sorted transparency and mirrored bounds");
    }
    for (float z : {-.01f, -.1f, -1.f, -8.f, 1.f}) {
        hidden.position.local = {0, 0, z};
        r.updateInstance(object, hidden);
        compare(render(r, camera, s, VisibilityMode::Direct), render(r, camera, s, VisibilityMode::HiZ),
                "Near-plane and behind-camera conservative handling");
    }
    for (float yaw : {1.5f, 3.14f, -1.5f, 0.f}) {
        camera.yaw = yaw;
        compare(render(r, camera, s, VisibilityMode::Direct), render(r, camera, s, VisibilityMode::HiZ),
                "Fast turns without history");
    }
    camera = {};
    hidden.position.local = {0, 0, -8};
    r.updateInstance(object, hidden);
    camera.chunk = {1000000000, -1000000000};
    hidden.position = worldPosition(camera, o.terrainGrid);
    hidden.position = translated(hidden.position, {0, 0, -8});
    r.updateInstance(object, hidden);
    r.removeInstance(wall);
    compare(render(r, camera, s, VisibilityMode::Direct), render(r, camera, s, VisibilityMode::HiZ),
            "Large world rebasing");
    // Resource retirement with queued readers and recycled CPU handles.
    s.visibility = VisibilityMode::HiZ;
    r.render(camera, s);
    r.render(camera, s);
    r.releaseMesh(mesh);
    auto replacement = r.uploadMesh(makeQuad());
    hidden.mesh = replacement;
    r.updateInstance(object, hidden);
    compare(render(r, camera, s, VisibilityMode::Direct), render(r, camera, s, VisibilityMode::HiZ),
            "Mesh replacement retains submitted readers");
    r.removeInstance(object);
    s.visibility = VisibilityMode::HiZ;
    r.render(camera, s);
    r.render(camera, s);
    require(r.validationErrors() == 0, "Visibility Vulkan validation clean");
    std::cout << "Visibility path " << fallback << " passed\n";
}
void terrain() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 129;
    o.height = 97;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 8};
    o.visibilityCapacity = 1024;
    Renderer r(o);
    TerrainRequest t;
    t.settings.heightScale = 5;
    r.requestTerrain(t);
    Camera c;
    c.local = {8, 7, 20};
    c.pitch = -.3f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.opaqueDepthPrepass = true;
    for (int i = 0; i < 4; ++i)
        r.render(c, s);
    for (float z : {20.f, 30.f, 60.f, 100.f}) {
        c.local.z = z;
        compare(render(r, c, s, VisibilityMode::Direct), render(r, c, s, VisibilityMode::Indirect),
                "Terrain indexed records");
        compare(render(r, c, s, VisibilityMode::Direct), render(r, c, s, VisibilityMode::HiZ),
                "Terrain morph depth equality");
    }
    // An owned foreground wall fully hides real terrain patches. Their depth
    // submissions remain, while the colour triangle counter must drop to zero.
    c.local = {8, 7, 20};
    c.pitch = -.3f;
    s.frustumCulling = false;
    s.lod = false;
    MeshMaterial wallMaterial;
    wallMaterial.doubleSided = true;
    MeshInstance wall;
    wall.mesh = r.uploadMesh(makeQuad());
    wall.material = r.createMaterial(wallMaterial);
    wall.position.local = glm::dvec3(c.local) + glm::dvec3(c.forward()) * 2.;
    wall.scale = {10, 10, 1};
    auto wallHandle = r.createInstance(wall);
    const auto covered = render(r, c, s, VisibilityMode::Direct);
    for (int i = 0; i < 4; ++i)
        compare(covered, render(r, c, s, VisibilityMode::HiZ), "Hidden terrain preserves foreground image");
    const auto counts = r.visibilityStats();
    require(counts.depthTriangles.terrain == 2048 && counts.colourTriangles.terrain == 0 &&
                counts.colourTriangles.meshes == 2,
            "HiZ rejects terrain colour triangles behind a foreground occluder");
    r.removeInstance(wallHandle);
    s.wireframe = true;
    s.visibility = VisibilityMode::HiZ;
    r.render(c, s);
    require(r.visibilityStats().fallback == VisibilityFallback::Wireframe, "Wireframe direct path");
    require(r.validationErrors() == 0, "Terrain visibility validation clean");
}
void streaming() {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 97;
    o.height = 65;
    o.terrainGrid = {32, .5f};
    o.terrainCapacity = 32;
    o.lod = {true, 8, 64};
    o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = o.streaming.hierarchical = true;
    o.streaming.diskCache = false;
    o.streaming.maximumDistance = 64;
    o.streaming.gpuBytes = 40 * terrainMiB;
    o.streaming.hostBytes = 64 * terrainMiB;
    o.streaming.diagnosticFrameSeconds = 1. / 60.;
    o.streaming.vertexDiagnostics = true;
    Renderer r(o);
    TerrainSettings t;
    t.heightScale = 8;
    r.setTerrainSettings(t);
    Camera c;
    c.local = {8, 12, 20};
    c.pitch = -.35f;
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.viewRadius = 1;
    s.visibility = VisibilityMode::HiZ;
    auto settle = [&] {
        int stable = 0;
        for (int i = 0; i < 2000 && stable < 60; ++i) {
            r.render(c, s);
            const auto &v = r.terrainStreamingStats();
            stable = !v.pending && !v.detailPending && !v.ioJobs ? stable + 1 : 0;
        }
        require(stable == 60, "Small streamed fixture settles");
    };
    settle();
    require(r.terrainRenderStats().selected > 0, "Streamed visibility fixture contains terrain");
    s.opaqueDepthPrepass = true;
    s.freezeLod = true;
    compare(render(r, c, s, VisibilityMode::Direct), render(r, c, s, VisibilityMode::HiZ),
            "Streamed terrain records and clipping");
    auto positions = r.readTerrainVertexPositions();
    require(!positions.empty(), "Vertex diagnostic still reads original patch ordering");
    // Same observer, abrupt view change, then a move requiring new
    // coverage/refinement.
    c.yaw = 1.8f;
    compare(render(r, c, s, VisibilityMode::Direct), render(r, c, s, VisibilityMode::HiZ),
            "Streamed cut without delayed reveal");
    s.freezeLod = false;
    c.chunk = {-2, 1};
    c.yaw = 0;
    for (int i = 0; i < 20; ++i)
        r.render(c, s);
    settle();
    s.freezeLod = true;
    compare(render(r, c, s, VisibilityMode::Direct), render(r, c, s, VisibilityMode::HiZ),
            "New streamed and parent-fallback patches");
    require(r.validationErrors() == 0, "Streamed visibility and retirement validation clean");
}

} // namespace
int main() {
    try {
        for (uint32_t i = 0; i < 7; ++i)
            run(i);
        terrain();
        streaming();
        std::cout << "Visibility GPU passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

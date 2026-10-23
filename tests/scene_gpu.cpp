#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>
using namespace ow;
namespace {
void compare(const HdrSnapshot &a, const HdrSnapshot &b, const char *message) {
    require(a.pixels.size() == b.pixels.size(), "Matching extents");
    for (size_t i = 0; i < a.pixels.size(); ++i)
        for (int c = 0; c < 4; ++c) {
            float v = a.pixels[i][c];
            near(b.pixels[i][c], v, 2e-4 + 3e-3 * std::abs(v), message);
        }
}
HdrSnapshot draw(Renderer &r, const Camera &c, const SceneSettings &s) {
    require(r.render(c, s), "Render scene");
    return r.readHdrSnapshot();
}
size_t pixel(const Renderer &r, const Camera &c, glm::vec3 p) {
    auto clip = c.viewProjection(float(r.width()) / r.height()) * glm::vec4(p, 1);
    auto n = glm::vec2(clip) / clip.w;
    return size_t((n.y * .5f + .5f) * r.height()) * r.width() + uint32_t((n.x * .5f + .5f) * r.width());
}
HdrSnapshot run(bool fixed) {
    RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.forceFixedMaterials = fixed;
    o.forceTrilinear = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.width = 129;
    o.height = 97;
    o.terrainGrid = {16, .5f};
    o.terrainCapacity = 16;
    o.sceneCapacity.lights = 32;
    o.sceneCapacity.instances = 32;
    o.sceneCapacity.meshes = 8;
    o.sceneCapacity.geometryBytes = 1ull << 20;
    o.sceneCapacity.clusterIndices = 4;
    Renderer r(o);
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.gtao.mode = ow::GtaoMode::Off;
    s.terrainVisible = false;
    s.lighting.sunEnabled = s.lighting.environmentEnabled = false;
    s.output.ev100 = 4;
    std::fill_n(s.background, 3, 0.f);
    Camera c;
    auto quad = r.uploadMesh(makeQuad());
    auto malformed = makeQuad();
    malformed.indices[0] = 999;
    rejects([&] { r.uploadMesh(malformed); }, "Reject mesh index outside vertex storage");
    malformed = makeQuad();
    malformed.vertices[0].uv.x = std::numeric_limits<float>::max();
    rejects([&] { r.uploadMesh(malformed); }, "Reject UV derivatives that can overflow");
    MeshData oversized;
    oversized.vertices.resize(o.sceneCapacity.geometryBytes / sizeof(MeshVertex) + 1);
    oversized.indices = {0, 1, 2};
    const auto allocatedBefore = r.sceneStats().geometryBytes;
    require(r.uploadMesh(oversized).slot == ~0u, "Geometry byte budget rejects oversized resource");
    require(r.sceneStats().geometryBytes == allocatedBefore,
            "Rejected geometry leaves allocation accounting unchanged");

    MeshMaterial m;
    m.baseColour = {.6f, .3f, .1f, 1};
    m.normalStrength = 0;
    m.roughness = .6f;
    auto mh = r.createMaterial(m);
    MeshInstance instance;
    instance.mesh = quad;
    instance.material = mh;
    instance.position.local = {0, 0, -8};
    instance.scale = {5, 4, 1};
    auto ih = r.createInstance(instance);
    auto dark = draw(r, c, s);
    size_t centre = (r.height() / 2) * r.width() + r.width() / 2;
    near(dark.pixels[centre].x, 0, 0, "Unlit scene dark");
    LocalLight l;
    l.position.local = {0, 0, -4};
    l.range = 12;
    l.intensity = 40;
    auto light = r.createLight(l);
    s.localLights = LocalLightMode::BruteForce;
    auto reference = draw(r, c, s);
    require(reference.pixels[centre].x > .005, "Ordinary one-sided mesh receives light");
    MaterialSettings cpu;
    cpu.baseColour = glm::vec3(m.baseColour);
    cpu.roughness = m.roughness;
    cpu.metallic = m.metallic;
    auto expected = directBrdf(cpu, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}) * l.intensity *
                    lightAttenuation(l, {0, 0, -4}) * reference.preExposure;
    near(reference.pixels[centre].x, expected.x, 2e-4 + 3e-3 * expected.x,
         "Point light agrees with independent CPU BRDF");
    s.localLights = LocalLightMode::Clustered;
    compare(reference, draw(r, c, s), "Cluster matches point reference");
    l.type = LightType::Spot;
    l.direction = glm::normalize(glm::vec3(.45f, 0, -1));
    l.innerAngle = .25f;
    l.outerAngle = .8f;
    r.updateLight(light, l);
    auto spot = draw(r, c, s);
    expected = directBrdf(cpu, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}) * l.intensity *
               lightAttenuation(l, {0, 0, -4}) * spot.preExposure;
    near(spot.pixels[centre].x, expected.x, 2e-4 + 3e-3 * expected.x,
         "Spot cone agrees with independent CPU attenuation");
    c.yaw = .4f;
    c.pitch = .13f;
    s.localLights = LocalLightMode::BruteForce;
    auto turned = draw(r, c, s);
    s.localLights = LocalLightMode::Clustered;
    compare(turned, draw(r, c, s), "Rotated camera cluster assignment");
    c = {};
    // Every scenario independently compares pixel lighting, including near-plane
    // crossings and spot cones.
    for (int i = 0; i < 16; ++i) {
        l.position.local = {double(i % 4 - 2) * 2, double((i / 4) % 3 - 1), i % 3 == 0 ? .2 : -double(i + 1)};
        l.range = i % 2 ? 20 : 6;
        l.type = i % 2 ? LightType::Spot : LightType::Point;
        l.direction = {0, 0, -1};
        l.innerAngle = .25f;
        l.outerAngle = 1.2f;
        r.updateLight(light, l);
        s.localLights = LocalLightMode::BruteForce;
        auto a = draw(r, c, s);
        s.localLights = LocalLightMode::Clustered;
        compare(a, draw(r, c, s), "Cluster conservative near/spot/boundary assignment");
    }
    for (uint32_t slice : {1u, 6u, 12u, 20u, 23u})
        for (float side : {.9999f, 1.0001f}) {
            float depth =
                Camera::nearPlane * std::pow(Camera::farPlane / Camera::nearPlane, float(slice) / 24) * side;
            instance.position.local.z = -depth;
            instance.scale = {depth * .6f, depth * .5f, 1};
            r.updateInstance(ih, instance);
            l = LocalLight{};
            l.position.local = {0, 0, -depth * .8};
            l.range = std::min(128000.f, depth * .6f);
            l.sourceRadius = std::min(.05f, l.range * .1f);
            l.intensity = std::min(1e9f, depth * depth * 2);
            r.updateLight(light, l);
            s.localLights = LocalLightMode::BruteForce;
            auto boundary = draw(r, c, s);
            require(boundary.pixels[centre].x > .001, "Depth boundary has visible light");
            s.localLights = LocalLightMode::Clustered;
            compare(boundary, draw(r, c, s), "Light assignment across logarithmic slice boundary");
        }
    instance.position.local.z = -8;
    instance.scale = {5, 4, 1};
    r.updateInstance(ih, instance);
    l.type = LightType::Point;
    l.position.local = {0, 0, -4};
    l.range = 20;
    l.intensity = 40;
    std::vector<LightHandle> extras;
    for (int i = 0; i < 12; ++i) {
        l.colour = {.2f + .05f * i, .8f, .4f};
        extras.push_back(r.createLight(l));
    }
    s.localLights = LocalLightMode::BruteForce;
    auto overlap = draw(r, c, s);
    s.localLights = LocalLightMode::Clustered;
    compare(overlap, draw(r, c, s), "Overflow scans all lights");
    r.render(c, s);
    r.render(c, s);
    require(r.sceneStats().overflowClusters > 0 && r.sceneStats().maximumOccupancy > 4,
            "Overflow diagnostics report pressure");
    for (auto h : extras)
        r.removeLight(h);
    r.removeLight(light);
    auto invalid = light;
    light = r.createLight(l);
    require(!r.updateLight(invalid, l), "Stale GPU light identity rejected");
    r.removeLight(light);
    // Independent alpha compositing expectation: far blue, near red over green
    // opaque.
    m.unlit = true;
    m.baseColour = {0, 1, 0, 1};
    instance.material = r.createMaterial(m);
    r.updateInstance(ih, instance);
    m.alpha = AlphaMode::Blend;
    m.baseColour = {0, 0, 1, .5f};
    auto blue = r.createMaterial(m);
    m.baseColour = {1, 0, 0, .25f};
    auto red = r.createMaterial(m);
    MeshInstance layer = instance;
    layer.material = red;
    layer.position.local.z = -4;
    auto nearLayer = r.createInstance(layer);
    layer.material = blue;
    layer.position.local.z = -6;
    auto farLayer = r.createInstance(layer);
    auto blend = draw(r, c, s);
    glm::vec3 composed(.25f, .375f, .375f);
    for (int k = 0; k < 3; ++k)
        near(blend.pixels[centre][k], composed[k] * blend.preExposure, 2e-4,
             "Sorted premultiplied HDR composition");
    // Reordering depth must reorder draws; opacity does not write depth.
    layer.material = blue;
    layer.position.local.z = -3;
    r.updateInstance(farLayer, layer);
    auto moved = draw(r, c, s);
    near(moved.pixels[centre].z, .5f * moved.preExposure, 2e-4, "Transparent sorting follows motion");
    s.opaqueDepthPrepass = true;
    compare(moved, draw(r, c, s), "Opaque prepass preserves blended composition");
    s.opaqueDepthPrepass = false;
    r.removeInstance(nearLayer);
    r.removeInstance(farLayer);
    for (float opacity : {0.f, 1.f}) {
        m.baseColour = {1, 0, 0, opacity};
        m.alpha = AlphaMode::Blend;
        layer = instance;
        layer.position.local.z = -4;
        layer.material = r.createMaterial(m);
        auto endpoint = r.createInstance(layer);
        auto image = draw(r, c, s);
        near(image.pixels[centre].x, opacity * image.preExposure, 2e-4, "Opacity endpoint red channel");
        near(image.pixels[centre].y, (1 - opacity) * image.preExposure, 2e-4,
             "Opacity endpoint exposes/occludes background");
        r.removeInstance(endpoint);
        r.releaseMaterial(layer.material);
    }
    // Masked colour and depth use the same cutoff, including texels straddling
    // 0.5.
    TextureData texture;
    texture.width = 4;
    texture.height = 1;
    texture.levels = 3;
    texture.format = TextureFormat::Srgb8;
    for (uint8_t alpha : {uint8_t(0), uint8_t(127), uint8_t(128), uint8_t(255)})
        texture.bytes.insert(texture.bytes.end(), {255, 255, 255, alpha});
    for (uint8_t alpha : {uint8_t(64), uint8_t(192), uint8_t(128)})
        texture.bytes.insert(texture.bytes.end(), {255, 255, 255, alpha});
    auto tex = r.uploadTexture(texture);
    MeshMaterial wrongSpace;
    wrongSpace.normalTexture = tex;
    rejects([&] { r.createMaterial(wrongSpace); }, "Reject sRGB data map");
    m.baseColour = {1, 0, 0, 1};
    m.alpha = AlphaMode::Mask;
    m.colourTexture = tex;
    m.cutoff = .5f;
    layer = instance;
    layer.position.local.z = -4;
    layer.scale = {2, 2, 1};
    layer.material = r.createMaterial(m);
    const auto maskedMaterial = layer.material;
    auto masked = r.createInstance(layer);
    auto mask = draw(r, c, s);
    s.opaqueDepthPrepass = true;
    compare(mask, draw(r, c, s), "Alpha-mask depth and colour equality");
    s.opaqueDepthPrepass = false;
    for (float x : {-1.5f, -.5f, .5f, 1.5f}) {
        auto p = pixel(r, c, {x, 0, -4});
        require(x < 0 ? mask.pixels[p].y > mask.pixels[p].x : mask.pixels[p].x > mask.pixels[p].y,
                "Mask cutoff exposes opaque background");
    }
    // Mirrored winding and nonuniform scale preserve the visible mask.
    layer.scale.x = -2;
    r.updateInstance(masked, layer);
    auto mirrored = draw(r, c, s);
    auto left = pixel(r, c, {-1.5f, 0, -4});
    require(mirrored.pixels[left].x > mirrored.pixels[left].y, "Mirrored instance remains front facing");
    r.removeInstance(masked);
    // Back-face normal handling is tested under a local light on the back side.
    m = MeshMaterial{};
    m.normalStrength = 0;
    m.doubleSided = true;
    layer = instance;
    layer.material = r.createMaterial(m);
    layer.position.local.z = -4;
    layer.rotation = glm::angleAxis(3.14159265359f, glm::vec3(0, 1, 0));
    auto back = r.createInstance(layer);
    l = LocalLight{};
    l.position.local = {0, 0, -2};
    l.intensity = 40;
    light = r.createLight(l);
    auto backlit = draw(r, c, s);
    require(backlit.pixels[centre].x > .001, "Double-sided back normal faces its light");
    r.removeInstance(back);
    r.removeLight(light);
    TextureData normalMap;
    normalMap.bytes = {191, 128, 238, 255};
    auto normalTexture = r.uploadTexture(normalMap);
    TextureData ormMap;
    ormMap.bytes = {64, 128, 255, 255};
    auto ormTexture = r.uploadTexture(ormMap);
    m = MeshMaterial{};
    m.baseColour = {.6f, .3f, .1f, 1};
    m.normalTexture = normalTexture;
    m.ormTexture = ormTexture;
    m.metallic = .3f;
    m.roughness = .8f;
    layer = instance;
    layer.position.local.z = -4;
    layer.scale = {2, 3, 1};
    layer.material = r.createMaterial(m);
    auto mapped = r.createInstance(layer);
    l = LocalLight{};
    l.position.local = {2, 0, -2};
    l.intensity = 40;
    light = r.createLight(l);
    for (float sign : {1.f, -1.f}) {
        layer.scale.x = 2 * sign;
        r.updateInstance(mapped, layer);
        auto image = draw(r, c, s);
        float nx = (191.f / 255 * 2 - 1) * sign, ny = 128.f / 255 * 2 - 1;
        glm::vec3 normal(nx, ny, std::sqrt(1 - nx * nx - ny * ny));
        cpu.baseColour = glm::vec3(m.baseColour);
        cpu.roughness = m.roughness * 128 / 255;
        cpu.metallic = m.metallic;
        expected = directBrdf(cpu, normal, {0, 0, 1}, glm::normalize(glm::vec3(2, 0, 2))) * l.intensity *
                   lightAttenuation(l, {-2, 0, -2}) * image.preExposure;
        for (int k = 0; k < 3; ++k)
            near(image.pixels[centre][k], expected[k], 2e-4 + 3e-3 * expected[k],
                 "Normal/ORM mapping and mirrored tangent match CPU BRDF");
    }
    r.removeInstance(mapped);
    r.removeLight(light);
    r.releaseMaterial(layer.material);
    r.releaseTexture(normalTexture);
    r.releaseTexture(ormTexture);
    // Resource changes with actual submitted readers, without diagnostic waits
    // between frames.
    layer = instance;
    layer.material = maskedMaterial;
    auto recycledMesh = r.uploadMesh(makeQuad());
    layer.mesh = recycledMesh;
    auto pending = r.createInstance(layer);
    r.render(c, s);
    r.render(c, s);
    require(r.releaseMesh(recycledMesh), "Release in-flight mesh");
    r.releaseMaterial(mh);
    r.releaseMaterial(maskedMaterial);
    r.releaseTexture(tex);
    r.render(c, s);
    require(r.sceneStats().skippedMeshes == 1, "Released mesh is skipped safely");
    r.removeInstance(pending);
    auto newer = r.uploadMesh(makeQuad());
    require(newer.slot != ~0u && (!r.releaseMesh(recycledMesh)), "Recycled mesh identity safe");
    r.releaseMesh(newer);
    // Translation of camera, objects and lights to large world cells changes no
    // shading.
    m = MeshMaterial{};
    m.normalStrength = 0;
    instance.material = r.createMaterial(m);
    r.updateInstance(ih, instance);
    l = LocalLight{};
    l.position.local = {0, 0, -4};
    l.intensity = 40;
    light = r.createLight(l);
    auto base = draw(r, c, s);
    auto saved = instance.position;
    WorldPosition origin{1000000000, -1000000000, {0, 0, 0}};
    instance.position = translated(origin, saved.local);
    r.updateInstance(ih, instance);
    l.position = translated(origin, l.position.local);
    r.updateLight(light, l);
    // The fixture terrain extent is 8 m, so fixed 1024 m cells correspond to 128
    // chunks.
    c.chunk = {128000000000ll, -128000000000ll};
    compare(base, draw(r, c, s), "Large-world mesh placement invariant");
    c = {};
    r.removeLight(light);
    instance.position = saved;
    r.updateInstance(ih, instance);
    for (auto extent : {glm::uvec2(193, 131), glm::uvec2(129, 97)}) {
        r.resizeOffscreen(extent.x, extent.y);
        s.localLights = LocalLightMode::BruteForce;
        auto a = draw(r, c, s);
        s.localLights = LocalLightMode::Clustered;
        compare(a, draw(r, c, s), "Resize rebuilds cluster grid");
    }
    // Opaque terrain must occlude meshes behind it while meshes in front remain
    // visible.
    r.removeInstance(ih);
    TerrainRequest terrain;
    terrain.settings.heightScale = 0;
    r.requestTerrain(terrain);
    s.terrainVisible = true;
    s.lighting.sunEnabled = true;
    c.local = {4, 10, 4};
    c.pitch = -1.5707963f;
    m = MeshMaterial{};
    m.unlit = true;
    m.baseColour = {1, 0, 0, 1};
    layer = {};
    layer.mesh = quad;
    layer.material = r.createMaterial(m);
    layer.position.local = {4, -1, 4};
    layer.rotation = glm::angleAxis(-1.5707963f, glm::vec3(1, 0, 0));
    layer.scale = {2, 2, 1};
    auto below = r.createInstance(layer);
    auto terrainBase = draw(r, c, s);
    layer.position.local.y = 1;
    r.updateInstance(below, layer);
    auto above = draw(r, c, s);
    require(above.pixels[centre].x > above.pixels[centre].y &&
                glm::length(above.pixels[centre] - terrainBase.pixels[centre]) > .005,
            "Terrain depth occludes behind and reveals front meshes");
    s.opaqueDepthPrepass = true;
    compare(above, draw(r, c, s), "Terrain and mesh depth prepass agree");
    s.opaqueDepthPrepass = false;
    s.lighting.sunEnabled = false;
    r.removeInstance(below);
    l = LocalLight{};
    l.position.local = {4, 3, 4};
    l.intensity = 250;
    light = r.createLight(l);
    s.localLights = LocalLightMode::BruteForce;
    auto terrainLit = draw(r, c, s);
    s.localLights = LocalLightMode::Clustered;
    compare(terrainLit, draw(r, c, s), "Terrain shares clustered local lighting");
    s.opaqueDepthPrepass = true;
    compare(terrainLit, draw(r, c, s), "Terrain local lighting with prepass");
    r.waitIdle();
    require(r.validationErrors() == 0, "Scene lifetime/synchronization validation clean");
    std::cout << "Scene GPU " << (fixed ? "fixed" : "preferred") << " passed on " << r.deviceName() << '\n';
    return blend;
}
} // namespace
int main() {
    try {
        auto preferred = run(false), fixed = run(true);
        compare(preferred, fixed, "Fixed and indexed material paths agree");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

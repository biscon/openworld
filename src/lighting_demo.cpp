#include "lighting_demo.hpp"
#include "mesh_primitives.hpp"
#include <cmath>
namespace ow {
void LightingDemo::clear() {
    for (auto h : lights_)
        renderer_.removeLight(h);
    lights_.clear();
    base_.clear();
    for (auto h : instances_)
        renderer_.removeInstance(h);
    instances_.clear();
    for (auto h : materials_)
        renderer_.releaseMaterial(h);
    materials_.clear();
}
LightingDemo::~LightingDemo() {
    clear();
    renderer_.releaseMesh(sphere_);
    renderer_.releaseMesh(quad_);
    renderer_.releaseTexture(checker_);
}
void LightingDemo::configure(bool enabled, WorldPosition anchor, uint32_t count, bool overlap,
                             uint32_t layers) {
    clear();
    if (!enabled)
        return;
    anchor_ = anchor;
    if (sphere_.slot == ~0u) {
        sphere_ = renderer_.uploadMesh(makeSphere());
        quad_ = renderer_.uploadMesh(makeQuad());
        TextureData texture;
        texture.width = texture.height = 8;
        texture.levels = 4;
        texture.format = TextureFormat::Srgb8;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                texture.bytes.insert(texture.bytes.end(),
                                     {255, 255, 255, uint8_t(((x / 2 + y / 2) % 2) ? 255 : 0)});
        for (uint32_t n : {4u, 2u, 1u})
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n; ++x)
                    texture.bytes.insert(texture.bytes.end(),
                                         {255, 255, 255, uint8_t(n == 1 ? 128 : ((x + y) % 2 ? 255 : 0))});
        checker_ = renderer_.uploadTexture(texture);
    }
    auto material = [&](MeshMaterial m) {
        auto h = renderer_.createMaterial(m);
        materials_.push_back(h);
        return h;
    };
    auto instance = [&](MeshHandle mesh, MaterialHandle m, glm::vec3 offset, glm::vec3 scale) {
        MeshInstance i;
        i.mesh = mesh;
        i.material = m;
        i.position = translated(anchor, offset);
        i.scale = scale;
        instances_.push_back(renderer_.createInstance(i));
    };
    MeshMaterial m;
    m.baseColour = {.5f, .5f, .5f, 1};
    m.roughness = .3f;
    auto opaque = material(m);
    for (int j = 0; j < 8; ++j)
        instance(sphere_, opaque, {float(j % 4) * 3 - 4.5f, float(j / 4) * 3 - 1.5f, -2}, {1, 1, 1});
    m.alpha = AlphaMode::Mask;
    m.colourTexture = checker_;
    m.doubleSided = true;
    auto mask = material(m);
    instance(quad_, mask, {5, 0, 1}, {2, 3, 1});
    m.colourTexture = {};
    m.alpha = AlphaMode::Blend;
    m.baseColour = {.2f, .6f, .9f, .3f};
    auto blend = material(m);
    for (uint32_t j = 0; j < std::min(layers, 16u); ++j)
        instance(quad_, blend, {float(j % 3) - 1, 0, float(j) * .7f}, {2.5f, 3, 1});
    const uint32_t side = std::max(1u, uint32_t(std::ceil(std::sqrt(float(std::min(count, 256u))))));
    for (uint32_t j = 0; j < std::min(count, 4096u); ++j) {
        LocalLight l;
        float x = float(j % side) - (side - 1) * .5f, z = float((j / side) % side) - (side - 1) * .5f,
              y = float(j / (side * side));
        l.position =
            translated(anchor, {x * (overlap ? .08 : 4), overlap ? 3. : 2. + y * 3, z * (overlap ? .08 : 4)});
        l.colour = {.3f + .7f * float((j * 17) % 31) / 30, .3f + .7f * float((j * 11 + 9) % 31) / 30,
                    .3f + .7f * float((j * 7 + 21) % 31) / 30};
        l.range = overlap ? 40 : 9;
        l.intensity = overlap ? 100000.f / std::max(1u, count) : 30000.f;
        l.type = j % 3 == 0 ? LightType::Spot : LightType::Point;
        l.direction = {0, -1, 0};
        l.innerAngle = .5f;
        l.outerAngle = 1.1f;
        auto h = renderer_.createLight(l);
        if (h.slot == ~0u)
            break;
        lights_.push_back(h);
        base_.push_back(l);
    }
}
void LightingDemo::animate(float seconds) {
    for (size_t j = 0; j < lights_.size(); ++j) {
        auto light = base_[j];
        light.position = translated(light.position, {std::sin(seconds + float(j) * .3f) * .8, 0,
                                                     std::cos(seconds + float(j) * .3f) * .8});
        renderer_.updateLight(lights_[j], light);
    }
}
} // namespace ow

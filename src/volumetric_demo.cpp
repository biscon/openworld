#include "volumetric_demo.hpp"
#include "mesh_primitives.hpp"
namespace ow {
VolumetricDemo::~VolumetricDemo() { clear(); }
void VolumetricDemo::clear() {
    for (auto h : instances_)
        renderer_.removeInstance(h);
    for (auto h : volumes_)
        renderer_.removeFogVolume(h);
    for (auto h : lights_)
        renderer_.removeLight(h);
    for (auto h : materials_)
        renderer_.releaseMaterial(h);
    renderer_.releaseMesh(quad_);
    instances_ = {};
    volumes_ = {};
    lights_ = {};
    materials_ = {};
    quad_ = {};
}
void VolumetricDemo::configure(uint32_t preset, WorldPosition anchor) {
    clear();
    if (!preset)
        return;
    FogVolume volume;
    volume.shape = FogShape::Box;
    volume.position = anchor;
    volume.extent = preset == 1 ? glm::vec3(40, 6, 35) : glm::vec3(25, 16, 25);
    volume.extinction = preset == 1 ? .025f : .035f;
    volume.falloff = .3f;
    volumes_[0] = renderer_.createFogVolume(volume);
    volume.shape = FogShape::Sphere;
    volume.position = translated(anchor, {22, -2, -12});
    volume.extent = glm::vec3(15);
    volume.extinction = .02f;
    volumes_[1] = renderer_.createFogVolume(volume);
    // Mist is a medium preset; only the shafts review creates test
    // geometry/lights.
    if (preset == 1)
        return;
    quad_ = renderer_.uploadMesh(makeQuad());
    MeshMaterial material;
    material.doubleSided = true;
    material.baseColour = {.3f, .3f, .32f, 1};
    materials_[0] = renderer_.createMaterial(material);
    material.alpha = AlphaMode::Blend;
    material.baseColour = {.3f, .6f, .7f, .4f};
    materials_[1] = renderer_.createMaterial(material);
    for (uint32_t k = 0; k < 7; ++k) {
        MeshInstance instance;
        instance.mesh = quad_;
        instance.material = materials_[0];
        instance.position = translated(anchor, {(double(k) - 3) * 5, 12, 0});
        instance.scale = {1.5f, 20, 1};
        instance.rotation = glm::angleAxis(glm::radians(-90.f), glm::vec3(1, 0, 0));
        instances_[k] = renderer_.createInstance(instance);
    }
    for (uint32_t k = 0; k < 2; ++k) {
        MeshInstance instance;
        instance.mesh = quad_;
        instance.material = materials_[1];
        instance.position = translated(anchor, {-8. + k * 16, 0, -5. - k * 10});
        instance.scale = {4, 6, 1};
        instances_[7 + k] = renderer_.createInstance(instance);
    }
    LocalLight light;
    light.position = translated(anchor, {-12, 3, 0});
    light.colour = {1, .25f, .08f};
    light.range = 30;
    light.intensity = 40000;
    lights_[0] = renderer_.createLight(light);
    light.position = translated(anchor, {12, 5, 5});
    light.colour = {.1f, .35f, 1};
    light.type = LightType::Spot;
    light.direction = {0, -.1f, -1};
    lights_[1] = renderer_.createLight(light);
}
} // namespace ow

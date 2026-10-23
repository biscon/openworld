#pragma once
#include "mesh_primitives.hpp"
#include "renderer.hpp"
// Owned fixtures use the same public resource/submission API as application
// meshes.
namespace ow::fixtures {
struct Swatches {
    Renderer &renderer;
    MeshHandle sphere, quad;
    std::vector<MaterialHandle> materials;
    std::vector<InstanceHandle> instances;
    explicit Swatches(Renderer &r)
        : renderer(r), sphere(r.uploadMesh(makeSphere())), quad(r.uploadMesh(makeQuad())) {
        const float rough[] = {.045f, .15f, .35f, .6f, 1};
        const glm::vec3 colours[] = {
            {.18f, .18f, .18f}, {.8f, .03f, .03f}, {.03f, .8f, .03f}, {.03f, .03f, .8f}, {.8f, .8f, .8f}};
        for (int i = 0; i < 20; ++i) {
            MeshMaterial m;
            m.doubleSided = true;
            m.baseColour = glm::vec4(.5f, .5f, .5f, 1);
            m.roughness = rough[i % 5];
            m.metallic = i >= 5 && i < 10 ? 1 : 0;
            if (i >= 10) {
                m.baseColour = glm::vec4(colours[i % 5], 1);
                m.roughness = .8f;
            }
            if (i >= 15) {
                m.baseColour = glm::vec4(0);
                m.emissive = glm::vec3(1000 * std::exp2(float(i - 15) * 3));
                m.unlit = true;
            }
            auto material = r.createMaterial(m);
            materials.push_back(material);
            MeshInstance instance;
            instance.mesh = i < 10 ? sphere : quad;
            instance.material = material;
            instance.position.local = {(i % 5 - 2) * 2.5, 3.6 - (i / 5) * 2.4, 0};
            instance.scale = glm::vec3(i < 10 ? 1 : .8f);
            instances.push_back(r.createInstance(instance));
        }
    }
    void clear() {
        for (auto h : instances)
            renderer.removeInstance(h);
        instances.clear();
        for (auto h : materials)
            renderer.releaseMaterial(h);
        materials.clear();
    }
    ~Swatches() {
        clear();
        renderer.releaseMesh(sphere);
        renderer.releaseMesh(quad);
    }
};
} // namespace ow::fixtures

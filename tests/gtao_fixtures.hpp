#pragma once
#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include <array>
#include <cmath>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
namespace ow::ao_fixture {
struct Triangle {
    glm::vec3 a, b, c;
};
inline MeshData plane(bool wall) {
    MeshData m = makeQuad();
    for (auto &v : m.vertices) {
        if (wall) {
            v.position = {v.position.x * 5, (v.position.y + 1) * 2, -2};
            v.normal = {0, 0, 1};
        } else {
            v.position = {v.position.x * 5, 0, -v.position.y * 5};
            v.normal = {0, 1, 0};
        }
    }
    return m;
}
struct Scene {
    Renderer &renderer;
    MeshHandle floorMesh, wallMesh;
    MeshInstance floor, wall;
    InstanceHandle floorInstance, wallInstance;
    MaterialHandle material;
    std::array<Triangle, 4> triangles;
    explicit Scene(Renderer &r) : renderer(r) {
        auto ground = plane(false), back = plane(true);
        floorMesh = r.uploadMesh(ground);
        wallMesh = r.uploadMesh(back);
        MeshMaterial m;
        m.baseColour = {.6, .6, .6, 1};
        m.doubleSided = true;
        material = r.createMaterial(m);
        floor.mesh = floorMesh;
        floor.material = material;
        wall.mesh = wallMesh;
        wall.material = material;
        floorInstance = r.createInstance(floor);
        wallInstance = r.createInstance(wall);
        for (uint32_t t = 0; t < 4; ++t) {
            const auto &mesh = t < 2 ? ground : back;
            auto i = (t % 2) * 3;
            triangles[t] = {mesh.vertices[mesh.indices[i]].position,
                            mesh.vertices[mesh.indices[i + 1]].position,
                            mesh.vertices[mesh.indices[i + 2]].position};
        }
    }
    void setMaterial(MaterialHandle m) {
        floor.material = wall.material = m;
        renderer.updateInstance(floorInstance, floor);
        renderer.updateInstance(wallInstance, wall);
    }
    void removeWall() {
        renderer.removeInstance(wallInstance);
        wallInstance = {};
    }
    void restoreWall() { wallInstance = renderer.createInstance(wall); }
    ~Scene() {
        renderer.removeInstance(floorInstance);
        renderer.removeInstance(wallInstance);
        renderer.releaseMesh(floorMesh);
        renderer.releaseMesh(wallMesh);
        renderer.releaseMaterial(material);
    }
};
inline Camera camera() {
    Camera c;
    c.local = {0, 1.2, 2};
    c.pitch = -.2f;
    return c;
}
inline SceneSettings settings() {
    SceneSettings s;
    s.shadows.mode = ow::ShadowMode::Off;
    s.terrainVisible = false;
    s.lighting.sunEnabled = false;
    s.lighting.environmentNits = 1;
    s.output.ev100 = 0;
    return s;
}
inline bool hit(const Triangle &t, glm::vec3 origin, glm::vec3 direction, float &distance) {
    auto e1 = t.b - t.a, e2 = t.c - t.a, p = glm::cross(direction, e2);
    float determinant = glm::dot(e1, p);
    if (std::abs(determinant) < 1e-8f)
        return false;
    float inv = 1 / determinant;
    auto v = origin - t.a;
    float u = glm::dot(v, p) * inv;
    if (u < 0 || u > 1)
        return false;
    auto q = glm::cross(v, e1);
    float w = glm::dot(direction, q) * inv;
    if (w < 0 || u + w > 1)
        return false;
    float d = glm::dot(e2, q) * inv;
    if (d <= 1e-4f || d >= distance)
        return false;
    distance = d;
    return true;
}
inline float radicalInverse(uint32_t x) {
    x = (x << 16) | (x >> 16);
    x = ((x & 0x55555555u) << 1) | ((x & 0xaaaaaaaau) >> 1);
    x = ((x & 0x33333333u) << 2) | ((x & 0xccccccccu) >> 2);
    x = ((x & 0x0f0f0f0fu) << 4) | ((x & 0xf0f0f0f0u) >> 4);
    x = ((x & 0x00ff00ffu) << 8) | ((x & 0xff00ff00u) >> 8);
    return float(double(x) * 2.3283064365386963e-10);
}
inline float reference(const std::array<Triangle, 4> &triangles, glm::vec3 p, glm::vec3 n, float radius) {
    constexpr uint32_t samples = 2048;
    uint32_t visible = 0;
    auto tangent =
        glm::normalize(glm::cross(n, std::abs(n.y) < .9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    auto bitangent = glm::cross(n, tangent);
    for (uint32_t i = 0; i < samples; ++i) {
        float u = (i + .5f) / samples, phi = 6.28318530718f * radicalInverse(i);
        auto d = tangent * (std::sqrt(u) * std::cos(phi)) + bitangent * (std::sqrt(u) * std::sin(phi)) +
                 n * std::sqrt(1 - u);
        float distance = radius;
        bool blocked = false;
        for (auto &t : triangles)
            blocked |= hit(t, p + n * 1e-4f, d, distance);
        visible += !blocked;
    }
    return float(visible) / samples;
}
inline void writePpm(const std::filesystem::path &path, uint32_t w, uint32_t h,
                     const std::vector<glm::vec4> &pixels) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << ' ' << h << "\n255\n";
    for (auto p : pixels)
        for (uint32_t c = 0; c < 3; ++c) {
            auto b = uint8_t(std::lround(std::clamp(p[c], 0.f, 1.f) * 255));
            f.write(reinterpret_cast<const char *>(&b), 1);
        }
    if (!f)
        throw std::runtime_error("Cannot write AO capture");
}
} // namespace ow::ao_fixture

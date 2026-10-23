#include "mesh_primitives.hpp"
#include <cmath>
namespace ow {
MeshData makeQuad() {
    MeshData m;
    m.vertices = {{{-1, -1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}},
                  {{1, -1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}},
                  {{1, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1}},
                  {{-1, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}}};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}
MeshData makeSphere(uint32_t rings, uint32_t sectors) {
    if (rings < 2 || sectors < 3 || rings > 256 || sectors > 512)
        throw std::invalid_argument("Invalid sphere tessellation");
    MeshData m;
    for (uint32_t y = 0; y <= rings; ++y)
        for (uint32_t x = 0; x <= sectors; ++x) {
            float theta = float(y) * 3.14159265359f / rings, phi = float(x) * 6.28318530718f / sectors;
            glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
            m.vertices.push_back(
                {n, n, {-std::sin(phi), 0, std::cos(phi), 1}, {float(x) / sectors, float(y) / rings}});
        }
    for (uint32_t y = 0; y < rings; ++y)
        for (uint32_t x = 0; x < sectors; ++x) {
            uint32_t a = y * (sectors + 1) + x, b = a + sectors + 1;
            for (auto i : {a, a + 1, b, a + 1, b + 1, b})
                m.indices.push_back(i);
        }
    return m;
}
} // namespace ow

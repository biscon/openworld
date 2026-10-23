#include "terrain_lod.hpp"
#include "test_support.hpp"
#include "visibility.hpp"
#include <cstdlib>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#include <new>
namespace {
size_t allocations = 0;
}
void *operator new(std::size_t n) {
    ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
int main() {
    try {
        for (float angle : {0.f, .5f, 1.5f})
            for (float scale : {-3.f, .1f, 4.f}) {
                auto m = glm::translate(glm::mat4(1), glm::vec3(123, -40, 700)) *
                         glm::rotate(glm::mat4(1), angle, glm::normalize(glm::vec3(1, 2, 3))) *
                         glm::scale(glm::mat4(1), glm::vec3(scale, .3f, 5));
                glm::vec3 lo(-2, -1, -3), hi(3, 4, 2);
                auto b = ow::transformBounds(lo, hi, m);
                require(b.valid, "Finite transformed bounds");
                for (uint32_t i = 0; i < 8; ++i) {
                    auto p = glm::vec3(
                        m * glm::vec4(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z, 1));
                    require(glm::all(glm::greaterThanEqual(p, b.lo)) && glm::all(glm::lessThanEqual(p, b.hi)),
                            "Transformed corners enclosed");
                }
            }
        auto m = glm::mat4(1);
        m[0][0] = std::numeric_limits<float>::infinity();
        require(!ow::transformBounds({-1, -1, -1}, {1, 1, 1}, m).valid, "Nonfinite bounds fail open");
        ow::TerrainGrid grid{32, .5f};
        ow::TerrainLod lod(grid, 32, {true, 8, 8});
        std::vector<glm::vec2> bounds(lod.leafCount(), {-4, 8});
        lod.setBounds(0, bounds.data());
        ow::Camera c;
        c.local = {8, 4, 18};
        std::array<ow::ChunkCoord, ow::maxTerrainChunks> chunks{};
        lod.select(chunks, 1, c, c, c.viewProjection(1), true, false, false);
        for (auto b : lod.drawBounds())
            require(!b.valid, "Unready terrain bounds render");
        lod.beginHierarchy(c, c, c.viewProjection(1), 2);
        lod.selectTile(0, {}, 0, 15, true, false, {-20, 30});
        require(lod.draws().size() == lod.drawBounds().size(), "Selection carries bounds");
        for (auto b : lod.drawBounds())
            require(b.valid && b.lo.y <= -24 && b.hi.y >= 26, "Parent envelope included in selected bounds");
        const auto before = allocations;
        for (int i = 0; i < 1000; ++i) {
            c.local.z = 18 + i * .01;
            lod.select(chunks, 1, c, c, c.viewProjection(1), true, false, true);
            lod.beginHierarchy(c, c, c.viewProjection(1), 2);
            lod.selectTile(0, {}, 0, 15, true, false, {-20, 30});
        }
        require(before == allocations, "Repeated terrain candidate/bounds preparation does not allocate");
        std::cout << "Visibility CPU passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

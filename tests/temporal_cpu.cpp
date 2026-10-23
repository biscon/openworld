#include "temporal.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
int main() {
    try {
        Camera camera;
        auto vp = camera.viewProjection(16.f / 9);
        glm::vec4 point(2, 1, -20, 1);
        auto base = vp * point;
        for (uint32_t i = 0; i < 16; ++i) {
            auto j = temporalJitter(i);
            require(std::abs(j.x) <= .5f && std::abs(j.y) <= .5f, "Bounded jitter");
            require(j == temporalJitter(i + 8), "Repeating sequence");
            auto raster = jitterProjection(vp, j, {1920, 1080}) * point;
            near((raster.x / raster.w - base.x / base.w) * 960, j.x, 2e-5,
                 "Projection moves by requested pixel x");
            near((raster.y / raster.w - base.y / base.w) * 540, j.y, 2e-5,
                 "Projection moves by requested pixel y");
        }
        TerrainHistory history(1);
        TemporalTerrainKey a, b;
        b.words[0] = 1;
        TemporalTerrainRecord value;
        value.valid.x = 1;
        value.observer.x = 12;
        history.begin(false);
        require(history.append(a, value).valid.x == 0, "First publication rejects history");
        history.submitted();
        history.begin(true);
        value.observer.x = 24;
        near(history.append(a, value).observer.x, 12, 0, "Previous submitted surface retained");
        require(history.append(b, value).valid.x == 0, "Capacity failure rejects history");
        history.begin(true);
        near(history.append(a, value).observer.x, 12, 0, "Unsubmitted frame cannot advance history");
        history.submitted();
        history.begin(true);
        require(history.append(b, value).valid.x == 0, "Changed identity rejects history");
        history.begin(false);
        require(history.append(a, value).valid.x == 0, "Global reset rejects history");
        auto levels = bloomExtents(65, 49);
        require(levels.front() == glm::uvec2(33, 25) && levels.back() == glm::uvec2(2, 1) &&
                    levels.size() == 6,
                "Bounded ceil-halved chain");
        require(bloomExtents(1, 1).size() == 1, "Tiny bloom terminates");
        for (float y : {0.f, .5f, 1.f, 1.5f, 10.f}) {
            float c = bloomContribution(y, 1, .5f);
            require(c >= 0 && c <= 1, "Threshold contribution bounded");
            near(bloomContribution(y * 8, 8, .5f), c, 1e-6, "Threshold pre-exposure invariance");
        }
        near(bloomContribution(.5f - 1e-5f, 1, .5f), bloomContribution(.5f + 1e-5f, 1, .5f), 1e-5,
             "Soft knee continuous");
        rejects([] { validatePost({true, 1}, {}); }, "Reject unbounded history");
        rejects(
            [] {
                BloomSettings b;
                b.radius = 0;
                validatePost({}, b);
            },
            "Reject invalid bloom radius");
        std::cout << "Temporal CPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "camera.hpp"
#include "gtao.hpp"
#include "test_support.hpp"
#include <iostream>
#include <limits>
int main() {
    try {
        using namespace ow;
        require(GtaoSettings{}.mode == GtaoMode::Off, "AO is opt-in by default");
        for (auto size :
             {glm::uvec2(1, 1), glm::uvec2(1, 33), glm::uvec2(129, 97), glm::uvec2(16384, 16384)}) {
            auto h = gtaoExtent(size.x, size.y, GtaoMode::Half);
            require(h.x * 2 >= size.x && h.x * 2 - size.x <= 1 && h.y * 2 >= size.y && h.y * 2 - size.y <= 1,
                    "Ceiling division covers odd tails");
            require(gtaoExtent(size.x, size.y, GtaoMode::Full) == size, "Full preserves native extent");
        }
        Camera c;
        auto vp = c.viewProjection(1.37f);
        for (float z : {.5f, 1.f, 10.f, 100.f, 200.f, 10000.f}) {
            auto clip = vp * glm::vec4(.1f, .1f, -z, 1);
            near(gtaoViewDepth(clip.z / clip.w), z, z * 2e-6, "Reversed projection round trip");
        }
        near(gtaoViewDepth(0), 0, 0, "Clear depth remains sky");
        for (float invalid : {-1.f, 2.f, std::numeric_limits<float>::quiet_NaN()})
            near(gtaoViewDepth(invalid), 0, 0, "Invalid depth is unoccluded");
        float d[] = {0, .2f, .8f, .8f};
        require(gtaoRepresentative(d) == 2, "Nearest valid sample with stable tie");
        float empty[] = {0, 0, 0, 0};
        require(gtaoRepresentative(empty) == 0, "Sky representative is bounded");
        near(gtaoFade(100, 4), 1, 0, "Full strength boundary");
        near(gtaoFade(200, 4), 0, 0, "Distance fade ends");
        near(gtaoFade(1, 1), 0, 0, "Subpixel fade ends");
        near(gtaoFade(150, 4), .5, 1e-6, "Smooth fade midpoint");
        for (int i = 0; i < 100; ++i)
            require(gtaoFade(100 + i, 4) >= gtaoFade(101 + i, 4), "Distance fade monotonic");
        rejects([] { gtaoExtent(0, 10, GtaoMode::Half); }, "Invalid extent rejected");
        for (float r : {0.f, 6.f, std::numeric_limits<float>::infinity()})
            rejects(
                [&] {
                    GtaoSettings s;
                    s.radius = r;
                    validateGtao(s);
                },
                "Invalid radius rejected");
        rejects(
            [] {
                GtaoSettings s;
                s.mode = GtaoMode(99);
                validateGtao(s);
            },
            "Invalid mode rejected");
        std::cout << "GTAO CPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "temporal.hpp"
#include "test_support.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
using namespace ow;
namespace {
bool trackingAllocations = false;
size_t allocationCount = 0;
} // namespace
void *operator new(size_t n) {
    if (trackingAllocations)
        ++allocationCount;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
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
        // Submission order changes with terrain batching. Track distinguishable
        // complete records through reordering, removal, replacement and aborts.
        {
            constexpr uint32_t count = 257;
            TerrainHistory tracked(count), empty(0);
            require(empty.append(a, value).valid.x == 0, "Zero-capacity history rejects safely");
            empty.submitted();
            std::array<uint32_t, count> order;
            for (uint32_t i = 0; i < count; ++i)
                order[i] = i;
            std::mt19937 random(42);
            auto key = [](uint32_t i) {
                TemporalTerrainKey k;
                k.words = {i % 7, i / 7, 3, 4, 5, 6, 7, uint64_t(i) << 40};
                return k;
            };
            auto record = [](uint32_t i, uint32_t frame) {
                TemporalTerrainRecord r;
                r.valid.x = 1;
                r.observer = glm::vec4(i, frame, i * 2, frame + i);
                r.draw[7] = {i, frame, i + frame, i ^ frame};
                r.weights[11] = r.observer;
                return r;
            };
            const auto allocationsBefore = allocationCount;
            trackingAllocations = true;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                std::shuffle(order.begin(), order.end(), random);
                tracked.begin(frame != 0);
                for (auto i : order) {
                    auto previous = tracked.append(key(i), record(i, frame));
                    if (frame == 0)
                        require(previous.valid.x == 0, "New keys reject history");
                    else {
                        auto expected = record(i, frame - 1);
                        require(previous.observer == expected.observer &&
                                    previous.draw[7] == expected.draw[7] &&
                                    previous.weights[11] == expected.weights[11],
                                "Reordered surfaces retrieve the complete prior submitted "
                                "record");
                    }
                }
                require(tracked.append(key(count), value).valid.x == 0, "Full batch rejects overflow");
                tracked.submitted();
            }
            tracked.begin(true);
            tracked.append(key(0), record(0, 99)); // Abandoned frame.
            tracked.begin(true);
            for (uint32_t i = 1; i < count; i += 2)
                require(tracked.append(key(i), record(i, 8)).observer.y == 7,
                        "Abandoned preparation does not change submitted lookup");
            tracked.submitted();
            tracked.begin(true);
            for (uint32_t i = 0; i < count; ++i) {
                auto previous = tracked.append(key(i), record(i, 9));
                require(previous.valid.x == (i % 2 ? 1 : 0),
                        "Removed identities cannot return stale records");
            }
            tracked.submitted();
            tracked.begin(false);
            require(tracked.append(key(17), value).valid.x == 0, "Reset clears the reordered lookup");
            trackingAllocations = false;
            require(allocationCount == allocationsBefore, "Terrain history frame work does not allocate");
            rejects([] { TerrainHistory tooLarge(uint64_t(UINT32_MAX) + 1); },
                    "History index capacity is bounded before allocation");
        }
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
        trackingAllocations = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}

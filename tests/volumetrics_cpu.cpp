#include "horizon.hpp"
#include "test_support.hpp"
#include "volumetrics.hpp"
#include <cstdlib>
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
using namespace ow;
int main() {
    try {
        VolumetricSettings s;
        validateVolumetrics(s);
        auto bad = s;
        bad.distance = 0;
        rejects([&] { validateVolumetrics(bad); }, "Invalid depth accepted");
        bad = s;
        bad.extinction = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { validateVolumetrics(bad); }, "NaN accepted");
        bad = s;
        bad.albedo.x = 2;
        rejects([&] { validateVolumetrics(bad); }, "Unphysical albedo accepted");
        for (float density : {-1.f, 11.f, std::numeric_limits<float>::quiet_NaN()}) {
            bad = s;
            bad.localDensity = density;
            rejects([&] { validateVolumetrics(bad); }, "Invalid local density multiplier accepted");
        }
        for (auto size :
             {glm::uvec2(1, 1), glm::uvec2(1920, 1080), glm::uvec2(3840, 2160), glm::uvec2(16384, 1)}) {
            auto g = volumetricGrid(size.x, size.y, VolumetricQuality::Normal);
            require(g.x >= 1 && g.x <= 192 && g.y >= 1 && g.y <= 108 && g.z == 64, "Bounded grid");
            double prev = -1;
            for (uint32_t k = 0; k <= g.z; ++k) {
                double d = volumetricDepth(k, g.z, 1000);
                require(d > prev, "Monotone depth");
                prev = d;
            }
            near(prev, 1000, 1e-10, "Grid reaches exact far plane");
        }
        auto vacuum = homogeneousTransport({}, {}, 1000);
        near(glm::length(vacuum.transmittance - glm::dvec3(1)), 0, 0, "Vacuum identity");
        const glm::dvec3 e(.001, .002, .003), q(.3, .2, .1);
        auto full = homogeneousTransport(e, q, 100);
        auto split = composeTransport(homogeneousTransport(e, q, 25), homogeneousTransport(e, q, 75));
        near(glm::length(full.scattering - split.scattering), 0, 1e-12, "Ordered segment integration");
        const glm::dvec3 f(.005, .003, .001), r(.02, .07, .06);
        auto mixed = homogeneousTransport(e + f, q + r, 100);
        glm::dvec3 numeric(0), tau(0);
        const double dt = .001;
        for (int k = 0; k < 100000; ++k) {
            numeric += glm::exp(-tau - (e + f) * dt * .5) * (q + r) * dt;
            tau += (e + f) * dt;
        }
        near(glm::length(mixed.scattering - numeric), 0, 1e-8, "Independent overlapping-medium quadrature");
        require(glm::length(composeTransport(homogeneousTransport(e, q, 100), homogeneousTransport(f, r, 100))
                                .scattering -
                            mixed.scattering) > .1,
                "Overlap is not serial fog passes");
        for (auto c : {glm::vec4(0), glm::vec4(6360000, 2000, 1, 0)})
            for (auto p : {glm::dvec3(0), glm::dvec3(4000, -1999, -4000), glm::dvec3(-100, 80, 300)})
                near(glm::length(uncurvePosition(curvePosition(p, c), c) - p), 0, 1e-8, "Curvature inverse");
        FogVolume v;
        validate(v);
        near(fogDensity(v, {}), v.extinction, 1e-10, "Volume centre");
        near(fogDensity(v, {11, 0, 0}), 0, 0, "Volume outside");
        near(fogDensity(v, {9, 0, 0}), v.extinction * .5, 1e-8, "Smooth volume edge");
        v.shape = FogShape::Box;
        v.extent = {2, 4, 8};
        v.rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(0, 1, 0));
        require(fogDensity(v, {6, 0, 0}) > 0 && fogDensity(v, {0, 0, 6}) == 0, "Oriented box density");
        ScenePool<FogVolume, FogVolumeHandle> pool(2);
        auto a = pool.create(v), b = pool.create(v);
        require(pool.create(v).slot == ~0u, "Pool exhaustion");
        pool.remove(a);
        auto c = pool.create(v);
        require(c.slot == a.slot && c.generation != a.generation && !pool.update(a, v), "Stale fog identity");
        const auto before = allocations;
        for (int k = 0; k < 1000; ++k) {
            pool.update(b, v);
            pool.remove(c);
            c = pool.create(v);
        }
        require(before == allocations, "Fog pool allocates during updates");
        std::cout << "Volumetric CPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

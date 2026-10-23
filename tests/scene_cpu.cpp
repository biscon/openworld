#include "scene.hpp"
#include "test_support.hpp"
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
        ScenePool<LocalLight, LightHandle> pool(2);
        LocalLight a, b;
        b.intensity = 20;
        auto x = pool.create(a), y = pool.create(b);
        require(pool.create(a).slot == ~0u, "Pool capacity enforced");
        require(pool.remove(x), "Remove live light");
        require(pool.get(y) && pool.get(y)->intensity == 20, "Swap removal preserves other identity");
        auto recycled = pool.create(b);
        require(recycled.slot == x.slot && recycled.generation != x.generation && !pool.update(x, a),
                "Stale handle cannot mutate recycled slot");
        require(!pool.remove({}), "Invalid removal safe");
        ScenePool<MeshInstance, InstanceHandle> stable(128);
        std::array<InstanceHandle, 128> handles{};
        const auto before = allocations;
        for (auto &h : handles)
            h = stable.create(MeshInstance{});
        for (int cycle = 0; cycle < 100; ++cycle)
            for (auto &h : handles) {
                MeshInstance value;
                value.position.local.x = cycle;
                stable.update(h, value);
                stable.remove(h);
                h = stable.create(value);
            }
        require(allocations == before, "Steady-state scene storage does not allocate");
        WorldPosition origin{1000000000, -1000000000, {1023.875, 4, .125}};
        auto p = translated(origin, {.25, 2, -.25});
        auto d = relativePosition(p, origin);
        near(d.x, .25, 1e-12, "Positive world carry");
        near(d.z, -.25, 1e-12, "Negative world carry");
        Camera camera;
        camera.chunk = {-123456789, 987654321};
        camera.local = {.125, 3, 127.875};
        auto w = worldPosition(camera, {256, .5f});
        camera.chunk.x += 1;
        camera.local.x -= 128;
        auto w2 = worldPosition(camera, {256, .5f});
        require(glm::length(relativePosition(w, w2)) == 0, "Camera rebase leaves scene origin stable");
        LocalLight l;
        l.range = 100;
        l.sourceRadius = .05f;
        validate(l);
        near(lightAttenuation(l, {0, 0, 100}), 0, 0, "Range cutoff");
        near(lightAttenuation(l, {0, 0, 0}), 400, 1e-3, "Finite source radius");
        float nearA = lightAttenuation(l, {0, 0, 1}), farA = lightAttenuation(l, {0, 0, 2});
        near(nearA / farA, 4, 1e-4, "Inverse square field");
        l.type = LightType::Spot;
        l.direction = {0, 0, -1};
        near(lightAttenuation(l, {0, 0, 1}), 0, 0, "Spot back hemisphere dark");
        require(lightAttenuation(l, {0, 0, -1}) > .9, "Spot axis illuminated");
        l.outerAngle = l.innerAngle;
        rejects([&] { validate(l); }, "Reject degenerate cone");
        l = LocalLight{};
        l.range = std::numeric_limits<float>::infinity();
        rejects([&] { validate(l); }, "Reject nonfinite light");
        l = LocalLight{};
        l.direction = glm::vec3(std::numeric_limits<float>::max());
        rejects([&] { validate(l); }, "Reject overflowing direction norm");
        MeshInstance invalidRotation;
        invalidRotation.rotation = glm::quat(std::numeric_limits<float>::max(), 0, 0, 0);
        rejects([&] { validate(invalidRotation); }, "Reject overflowing quaternion norm");
        MeshInstance instance;
        instance.scale.x = 0;
        rejects([&] { validate(instance); }, "Reject singular transform");
        instance.scale = {-2, 3, 4};
        validate(instance);
        MeshMaterial m;
        m.baseColour.a = -1;
        rejects([&] { validate(m); }, "Reject invalid opacity");
        SceneCapacity config;
        auto g = clusterGrid(1920, 1080, config);
        require(g.x == 60 && g.y == 34 && g.z == 24, "Grid covers partial final tiles");
        config.clusterBytesPerFrame = 1 << 20;
        auto small = clusterGrid(3840, 2160, config);
        require(small.tile > 32 &&
                    small.bytes() + uint64_t(config.lights) * 64 + 4096 <= config.clusterBytesPerFrame,
                "Budget adapts grid conservatively");
        require(depthSlice(Camera::nearPlane, 24) == 0 && depthSlice(Camera::farPlane, 24) == 23,
                "Depth endpoints clamped");
        for (uint32_t i = 1; i < 24; ++i) {
            float d0 = Camera::nearPlane * std::pow(Camera::farPlane / Camera::nearPlane, float(i) / 24);
            require(depthSlice(d0 * .999f, 24) == i - 1 && depthSlice(d0 * 1.001f, 24) == i,
                    "Log slice boundaries");
        }
        std::cout << "Scene CPU contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

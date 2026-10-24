#include "test_support.hpp"
#include "vegetation_demo.hpp"
#include <iostream>
#include <limits>
using namespace ow;
int main() {
    try {
        VegetationInteractionInput in;
        VegetationInteractionSettings settings;
        for (uint32_t i = 0; i < 16; ++i)
            require(in.append({}), "contact capacity");
        require(!in.append({}) && in.rejected == 1 && in.count == 16, "bounded shape rejection");
        validate(settings, in);
        auto rejects = [&] {
            bool failed = false;
            try {
                validate(settings, in);
            } catch (const std::invalid_argument &) {
                failed = true;
            }
            require(failed, "invalid interaction rejected");
        };
        in.shapes[0].radius = 0;
        rejects();
        in.shapes[0].radius = .65f;
        in.shapes[0].b.local.x = 65;
        rejects();
        in.shapes[0].b = {};
        in.count = 17;
        rejects();
        in.count = 0;
        settings.recoveryHalfLife = std::numeric_limits<float>::quiet_NaN();
        rejects();
        for (int64_t x : {-1000000000ll, -1ll, 0ll, 1000000000ll}) {
            WorldPosition p;
            p.x = x;
            p.z = -x;
            p.local = {-.01, 32, 1024.23};
            auto origin = vegetationFieldOrigin(p);
            auto d = relativePosition(p, origin);
            near(d.x, 16.24, 1e-6, "negative snap");
            near(d.z, 16.23, 1e-6, "large world snap");
            auto moved = vegetationFieldOrigin(translated(p, {.25, 0, -.25}));
            auto shift = relativePosition(moved, origin);
            near(shift.x, .25, 1e-6, "exact texel shift");
            near(shift.z, -.25, 1e-6, "negative texel shift");
        }
        VegetationSettings s;
        s.proxy.active = true;
        VegetationDemo demo;
        demo.update(s, {}, .1f, false);
        auto generation = s.contacts.reset;
        demo.update(s, {1, 0}, .1f, false);
        near(s.proxy.position.local.x, .3, 1e-6, "proxy movement");
        require(s.contacts.count == 1 && s.contacts.reset == generation, "continuous swept footprint");
        near(relativePosition(s.contacts.shapes[0].b, s.contacts.shapes[0].a).x, .3, 1e-6, "sweep endpoints");
        demo.update(s, {1, 0}, .1f, false);
        near(relativePosition(s.contacts.shapes[0].b, s.contacts.shapes[0].a).x, .6, 1e-6,
             "skipped submission retains swept start");
        demo.submitted(s);
        demo.update(s, {1, 0}, .1f, false);
        near(relativePosition(s.contacts.shapes[0].b, s.contacts.shapes[0].a).x, .3, 1e-6,
             "successful submission commits swept start");
        s.proxy.position = translated(s.proxy.position, {100, 0, 0});
        demo.update(s, {}, .1f, false);
        require(s.contacts.reset != generation, "teleport invalidates trail");
        near(glm::length(relativePosition(s.contacts.shapes[0].a, s.contacts.shapes[0].b)), 0, 1e-6,
             "teleport never stamps bridge");
        s.proxy.route = true;
        demo.update(s, {}, .1f, false);
        demo.submitted(s);
        generation = s.contacts.reset;
        s.proxy.active = false;
        s.proxy.route = false;
        demo.update(s, {}, .1f, false);
        require(s.contacts.active && s.contacts.count == 0 && s.contacts.reset == generation,
                "removing footprint and route permits trail recovery");
        demo.reset();
        demo.update(s, {}, .1f, false);
        require(!s.contacts.active, "explicit reset discards inactive trail");
        std::cout << "Vegetation interaction CPU checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

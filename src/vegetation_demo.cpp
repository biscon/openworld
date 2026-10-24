#include "vegetation_demo.hpp"
namespace ow {
void VegetationDemo::update(VegetationSettings &s, glm::vec2 movement, float dt, bool boost) {
    auto &p = s.proxy;
    if (!initialized_ || glm::length(relativePosition(p.position, previous_)) > 8 ||
        (p.active && p.route != route_)) {
        previous_ = p.position;
        routeCentre_ = p.position;
        phase_ = 0;
        route_ = p.route;
        ++generation_;
        initialized_ = true;
    }
    if (p.active) {
        fieldActive_ = true;
        if (p.route) {
            phase_ += dt * 3 / 4;
            p.position = translated(routeCentre_, {4 * std::sin(phase_), 0, 4 * (std::cos(phase_) - 1)});
        } else {
            float len = glm::length(movement);
            if (len > 1)
                movement /= len;
            p.position =
                translated(p.position, glm::dvec3(movement.x, 0, movement.y) * double(dt * (boost ? 6 : 3)));
        }
    }
    s.contacts = {};
    s.contacts.anchor = p.position;
    s.contacts.active = fieldActive_;
    s.contacts.reset = generation_;
    if (p.active)
        s.contacts.append({previous_, p.position, p.radius, p.strength});
}
} // namespace ow

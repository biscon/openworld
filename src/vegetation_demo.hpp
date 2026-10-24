#pragma once
#include "vegetation.hpp"
namespace ow {
// Application-side proxy. The renderer only consumes bounded transient contacts.
class VegetationDemo {
  public:
    void update(VegetationSettings &, glm::vec2 movement, float dt, bool boost);
    void submitted(const VegetationSettings &s) { previous_ = s.proxy.position; }
    void reset() {
        initialized_ = false;
        fieldActive_ = false;
        ++generation_;
    }

  private:
    WorldPosition previous_, routeCentre_;
    float phase_ = 0;
    uint64_t generation_ = 1;
    bool initialized_ = false, route_ = false, fieldActive_ = false;
};
} // namespace ow

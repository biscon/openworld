#include "vegetation_interaction.hpp"
#include <cmath>
#include <stdexcept>
namespace ow {
void validate(const VegetationInteractionSettings &s, const VegetationInteractionInput &in) {
    if (!std::isfinite(s.recoveryHalfLife) || s.recoveryHalfLife < .1f || s.recoveryHalfLife > 10 ||
        in.count > in.shapes.size())
        throw std::invalid_argument("Invalid vegetation interaction settings");
    validate(in.anchor);
    for (uint32_t i = 0; i < in.count; ++i) {
        const auto &c = in.shapes[i];
        validate(c.a);
        validate(c.b);
        auto span = relativePosition(c.b, c.a);
        if (std::hypot(span.x, span.z) > 64)
            throw std::invalid_argument("Vegetation contact sweep exceeds 64 m; reset on teleport");
        if (!std::isfinite(c.radius) || c.radius < .1f || c.radius > 4 || !std::isfinite(c.strength) ||
            c.strength < 0 || c.strength > 1)
            throw std::invalid_argument("Invalid vegetation contact");
    }
}
WorldPosition vegetationFieldOrigin(WorldPosition p) {
    p.local.x = std::floor(p.local.x * 4) * .25 - 16;
    p.local.z = std::floor(p.local.z * 4) * .25 - 16;
    p.local.y = 0;
    return translated(p, {});
}
} // namespace ow

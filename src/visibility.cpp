#include "visibility.hpp"
#include <cmath>
#include <limits>
namespace ow {
VisibilityBounds transformBounds(glm::vec3 lo, glm::vec3 hi, const glm::mat4 &m) {
    const glm::dmat4 precise(m);
    const auto centre = glm::dvec3(precise * glm::dvec4((glm::dvec3(lo) + glm::dvec3(hi)) * .5, 1));
    const auto half = (glm::dvec3(hi) - glm::dvec3(lo)) * .5;
    const auto radius = glm::abs(glm::dvec3(precise[0])) * half.x +
                        glm::abs(glm::dvec3(precise[1])) * half.y + glm::abs(glm::dvec3(precise[2])) * half.z;
    // Bound float shader multiply/add rounding, including cancellation around a
    // large local mesh origin. A fixed centimetre alone is not enough at scale.
    const auto magnitude = glm::max(glm::abs(glm::dvec3(lo)), glm::abs(glm::dvec3(hi)));
    const auto error =
        glm::dvec3(.01) +
        16. * std::numeric_limits<float>::epsilon() *
            (glm::abs(glm::dvec3(precise[3])) + glm::abs(glm::dvec3(precise[0])) * magnitude.x +
             glm::abs(glm::dvec3(precise[1])) * magnitude.y + glm::abs(glm::dvec3(precise[2])) * magnitude.z);
    VisibilityBounds b{glm::vec3(centre - radius - error), glm::vec3(centre + radius + error), true};
    for (int i = 0; i < 3; ++i)
        b.valid &= std::isfinite(b.lo[i]) && std::isfinite(b.hi[i]) && b.lo[i] <= b.hi[i];
    return b;
}
} // namespace ow

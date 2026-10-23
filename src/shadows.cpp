#include "shadows.hpp"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
namespace ow {
void validate(const ShadowSettings &s) {
    if (uint32_t(s.mode) > 1 || !std::isfinite(s.nearDistance) || s.nearDistance < 1 ||
        s.nearDistance > 16000 || !std::isfinite(s.farDistance) || s.farDistance < 0 ||
        s.farDistance > 16000 || !std::isfinite(s.biasTexels) || s.biasTexels < 0 || s.biasTexels > 8 ||
        !std::isfinite(s.normalBiasTexels) || s.normalBiasTexels < 0 || s.normalBiasTexels > 4 ||
        s.diagnostic > 3)
        throw std::invalid_argument("Invalid CSM settings");
}
glm::dmat3 shadowBasis(glm::dvec3 sun) {
    if (!std::isfinite(glm::dot(sun, sun)) || glm::dot(sun, sun) < 1e-12)
        throw std::invalid_argument("Invalid shadow sun direction");
    sun = glm::normalize(sun);
    // Rotate +Y to the sun. The singularity is below the supported hemisphere.
    const auto q =
        sun.y > -.999999 ? glm::normalize(glm::dquat(1 + sun.y, sun.z, 0, -sun.x)) : glm::dquat(0, 1, 0, 0);
    const auto rotation = glm::mat3_cast(q);
    return glm::transpose(glm::dmat3(rotation[0], -rotation[2], sun));
}
std::array<ShadowCascade, shadowCascades> shadowCascadeLayout(const Camera &camera, TerrainGrid grid,
                                                              float aspect, glm::dvec3 sun,
                                                              const ShadowSettings &settings,
                                                              float farDistance, uint32_t resolution) {
    validate(settings);
    if (!std::isfinite(aspect) || aspect <= 0 || resolution < 16 || !std::isfinite(farDistance) ||
        farDistance < 1)
        throw std::invalid_argument("Invalid CSM projection");
    const double end =
        std::clamp(double(settings.farDistance > 0 ? settings.farDistance : farDistance), 1.0, 16000.0);
    const double near = std::min(double(settings.nearDistance), end);
    const double splits[] = {Camera::nearPlane, near * .05, near * .15, near * .4, near, end};
    const auto basis = shadowBasis(sun);
    const auto forward = glm::dvec3(camera.forward());
    // Keep the global grid phase precise even at the camera's navigation limit;
    // no large absolute coordinate is ever converted to a shader float.
    const long double world[] = {static_cast<long double>(camera.chunk.x) * grid.extent() + camera.local.x,
                                 camera.local.y,
                                 static_cast<long double>(camera.chunk.z) * grid.extent() + camera.local.z};
    std::array<ShadowCascade, shadowCascades> out{};
    const double tangent = std::tan(glm::radians(double(Camera::verticalFovDegrees)) * .5);
    for (uint32_t i = 0; i < shadowCascades; ++i) {
        double a = i ? splits[i] - .1 * (splits[i] - splits[i - 1]) : splits[0], b = splits[i + 1];
        if (i == 4 && end <= near) {
            out[i].farDistance = 0;
            continue;
        }
        a = std::min(a, b * .5);
        const double centre = (a + b) * .5;
        const double radius = std::sqrt((b - centre) * (b - centre) +
                                        b * b * tangent * tangent * (1 + double(aspect) * aspect));
        // Guard for PCF and half-texel centre snapping without clipping the sphere.
        const double padded = std::ceil(radius * 16) / 16 * double(resolution) / (resolution - 6);
        const double texel = 2 * padded / resolution;
        const auto relative = basis * (forward * centre);
        glm::dvec2 snapped;
        for (int axis = 0; axis < 2; ++axis) {
            long double lightOrigin = 0;
            for (int component = 0; component < 3; ++component)
                lightOrigin += world[component] * basis[component][axis];
            const double phase = double(std::remainder(lightOrigin, static_cast<long double>(texel)));
            snapped[axis] = std::round((phase + relative[axis]) / texel) * texel - phase;
        }
        out[i] = {basis, snapped, padded, float(a), float(b)};
    }
    return out;
}
VisibilityBounds shadowLightBounds(const VisibilityBounds &b, const glm::dmat3 &basis) {
    if (!b.valid)
        return {{-32768, -32768, -32768}, {32768, 32768, 32768}, true};
    glm::dvec3 lo(INFINITY), hi(-INFINITY);
    for (uint32_t i = 0; i < 8; ++i) {
        const auto p = basis * glm::dvec3((i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y,
                                          (i & 4) ? b.hi.z : b.lo.z);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    return {glm::vec3(lo), glm::vec3(hi), true};
}
} // namespace ow

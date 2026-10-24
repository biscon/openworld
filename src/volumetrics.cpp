#include "volumetrics.hpp"
#include <cmath>
#include <glm/gtc/quaternion.hpp>
namespace ow {
namespace {
bool bounded(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool colour(glm::vec3 v) { return bounded(v.x, 0, 1) && bounded(v.y, 0, 1) && bounded(v.z, 0, 1); }
} // namespace
void validateVolumetrics(const VolumetricSettings &s) {
    if (uint32_t(s.quality) > 1 || !bounded(s.distance, 50, 4000) || !bounded(s.extinction, 0, 10) ||
        !bounded(s.localDensity, 0, 10) || !bounded(s.height, -1e6f, 1e6f) ||
        !bounded(s.scaleHeight, .1f, 100000) || !colour(s.albedo) || !bounded(s.anisotropy, -.95f, .95f) ||
        !bounded(s.historyWeight, 0, .9f) || s.samples < 1 || s.samples > 2 || s.diagnostic > 4)
        throw std::invalid_argument("Invalid volumetric settings");
}
void validate(const FogVolume &v) {
    validate(v.position);
    float norm = glm::dot(v.rotation, v.rotation);
    if (uint32_t(v.shape) > 1 || !bounded(norm, 1e-12f, 1e12f) || !bounded(v.extent.x, .01f, 1e6f) ||
        !bounded(v.extent.y, .01f, 1e6f) || !bounded(v.extent.z, .01f, 1e6f) || !colour(v.albedo) ||
        !bounded(v.extinction, 0, 10) || !bounded(v.falloff, .001f, 1))
        throw std::invalid_argument("Invalid fog volume");
}
glm::uvec3 volumetricGrid(uint32_t w, uint32_t h, VolumetricQuality q) {
    if (!w || !h || uint32_t(q) > 1)
        throw std::invalid_argument("Invalid fog extent");
    const uint32_t tile = q == VolumetricQuality::Normal ? 12 : 8;
    double x = (uint64_t(w) + tile - 1) / tile, y = (uint64_t(h) + tile - 1) / tile;
    const double scale = std::min({1., (q == VolumetricQuality::Normal ? 192. : 256.) / x,
                                   (q == VolumetricQuality::Normal ? 108. : 144.) / y});
    return {std::max(1u, uint32_t(std::ceil(x * scale))), std::max(1u, uint32_t(std::ceil(y * scale))),
            q == VolumetricQuality::Normal ? 64u : 96u};
}
double volumetricDepth(double cell, uint32_t cells, double distance) {
    return volumetricDepthOffset * std::expm1(std::log1p(distance / volumetricDepthOffset) * cell / cells);
}
glm::dvec3 uncurvePosition(glm::dvec3 p, glm::vec4 c) {
    if (c.z == 0)
        return p;
    const double radial = std::hypot(p.x, p.z), base = double(c.x) + c.y;
    const double y = base + p.y, radius = std::hypot(radial, y);
    const double angle = std::atan2(radial, y);
    const double scale = radial > 1e-12 ? double(c.x) * angle / radial : double(c.x) / radius;
    // Rationalized radius-base subtraction retains local vertical precision.
    return {p.x * scale, (radial * radial + p.y * (2 * base + p.y)) / (radius + base), p.z * scale};
}
double fogDensity(const FogVolume &v, glm::dvec3 point) {
    const auto p = glm::conjugate(glm::normalize(glm::dquat(v.rotation))) * point;
    const auto a = glm::abs(p) / glm::dvec3(v.extent);
    const double d = v.shape == FogShape::Sphere ? glm::length(p) / v.extent.x : std::max({a.x, a.y, a.z});
    const double f = std::clamp((1 - d) / double(v.falloff), 0., 1.);
    return v.extinction * f * f * (3 - 2 * f);
}
AtmosphericTransport homogeneousTransport(glm::dvec3 ext, glm::dvec3 source, double distance) {
    AtmosphericTransport out;
    for (int k = 0; k < 3; ++k) {
        out.transmittance[k] = std::exp(-ext[k] * distance);
        out.scattering[k] = source[k] * (ext[k] > 0 ? -std::expm1(-ext[k] * distance) / ext[k] : distance);
    }
    return out;
}
const char *volumetricFallbackName(VolumetricFallback f) {
    switch (f) {
    case VolumetricFallback::None:
        return "Active";
    case VolumetricFallback::Disabled:
        return "Off";
    case VolumetricFallback::Unsupported:
        return "Unsupported formats/limits";
    case VolumetricFallback::Budget:
        return "Budget exceeded";
    case VolumetricFallback::Diagnostic:
        return "Diagnostic bypass";
    case VolumetricFallback::Empty:
        return "Zero density";
    }
    return "Unknown";
}
} // namespace ow

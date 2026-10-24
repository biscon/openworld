#pragma once
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
namespace ow {
// x: reference radius, y: camera altitude above datum, z: enabled, w: detailed radius.
inline glm::dvec3 curvePosition(glm::dvec3 p, glm::vec4 c) {
    if (c.z == 0)
        return p;
    const double r = std::hypot(p.x, p.z), a = r / c.x, h = p.y + c.y;
    const double k = (1 + h / c.x) * (a > 1e-9 ? std::sin(a) / a : 1);
    return {p.x * k, p.y - 2 * (c.x + h) * std::pow(std::sin(a * .5), 2), p.z * k};
}
inline void curveBounds(glm::vec3 &lo, glm::vec3 &hi, glm::vec4 c) {
    if (c.z == 0)
        return;
    const auto nearest = glm::max(glm::max(lo, -hi), glm::vec3(0));
    const auto farthest = glm::max(glm::abs(lo), glm::abs(hi));
    const double amin = std::hypot(nearest.x, nearest.z) / c.x;
    const double amax = std::hypot(farthest.x, farthest.z) / c.x;
    if (amax > 1.5) {
        const float radius =
            float(std::max(std::abs(double(c.x) + lo.y + c.y), std::abs(double(c.x) + hi.y + c.y)));
        lo = glm::vec3(-radius);
        hi = glm::vec3(radius);
        lo.y -= c.x + c.y;
        hi.y -= c.x + c.y;
        return;
    }
    const double hlo = double(lo.y) + c.y, hhi = double(hi.y) + c.y;
    const auto sinc = [](double a) { return a > 1e-9 ? std::sin(a) / a : 1.; };
    const double kmin = (1 + hlo / c.x) * sinc(amax), kmax = (1 + hhi / c.x) * sinc(amin);
    glm::vec3 outLo = lo, outHi = hi;
    for (int axis : {0, 2}) {
        outLo[axis] = float(std::min(double(lo[axis]) * kmin, double(lo[axis]) * kmax));
        outHi[axis] = float(std::max(double(hi[axis]) * kmin, double(hi[axis]) * kmax));
    }
    outLo.y = float(lo.y - 2 * (c.x + hlo) * std::pow(std::sin(amax * .5), 2));
    outHi.y = float(hi.y - 2 * (c.x + hhi) * std::pow(std::sin(amin * .5), 2));
    const float pad = .02f + glm::length(farthest) * 2e-6f;
    lo = outLo - pad;
    hi = outHi + pad;
}
inline double horizonCoverage(double radius, double low, double high, double flight = 2000) {
    const double ground = radius + low;
    const double d = radius * (std::acos(std::clamp(ground / (radius + high + flight), 0., 1.)) +
                               std::acos(std::clamp(ground / (radius + high), 0., 1.)));
    return d * 1.1;
}
} // namespace ow

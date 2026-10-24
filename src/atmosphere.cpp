#include "atmosphere.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace ow {
void validateAtmosphere(const AtmosphereSettings &s) {
    auto range = [](float v, float a, float b) { return std::isfinite(v) && v >= a && v <= b; };
    auto coefficient = [&](glm::vec3 v) {
        return range(v.x, 0, .001f) && range(v.y, 0, .001f) && range(v.z, 0, .001f);
    };
    if (uint32_t(s.mode) > 2 || uint32_t(s.quality) > 1 || s.diagnostic > 2 ||
        !range(s.datum, -1e6f, 1e6f) || !range(s.planetRadius, 1e6f, 1e8f) ||
        !range(s.atmosphereHeight, 10000, 200000) || !coefficient(s.rayleigh) ||
        !coefficient(s.ozone) || !range(s.rayleighHeight, 100, 30000) ||
        !range(s.mieHeight, 100, 30000) || !range(s.mieScattering, 0, .001f) ||
        !range(s.mieAbsorption, 0, .001f) || !range(s.mieG, -.95f, .95f) ||
        !range(s.ozoneCentre, 0, 100000) || !range(s.ozoneWidth, 100, 100000) ||
        !range(s.aerosolDensity, 0, 20) || !range(s.groundAlbedo, 0, .9f) ||
        !range(s.sunRadius, .001f, .02f) || !range(s.environmentMultiplier, 0, 10) ||
        !range(s.boundaryHaze, 0, 10))
        throw std::invalid_argument("Invalid atmosphere settings");
}
uint64_t atmosphereProfileKey(const AtmosphereSettings &s) {
    uint64_t h = 14695981039346656037ull;
    // Explicit fields: never hash padding or camera/exposure-dependent state.
    const float values[] = {s.datum,
                            s.planetRadius,
                            s.atmosphereHeight,
                            s.rayleigh.x,
                            s.rayleigh.y,
                            s.rayleigh.z,
                            s.rayleighHeight,
                            s.mieScattering,
                            s.mieAbsorption,
                            s.mieHeight,
                            s.mieG,
                            s.ozone.x,
                            s.ozone.y,
                            s.ozone.z,
                            s.ozoneCentre,
                            s.ozoneWidth,
                            s.aerosolDensity,
                            s.groundAlbedo,
                            float(s.multipleScattering)};
    for (float f : values) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        h = (h ^ u) * 1099511628211ull;
    }
    return h;
}
AtmosphericTransport composeTransport(const AtmosphericTransport &a,
                                      const AtmosphericTransport &b) {
    return {a.transmittance * b.transmittance, a.scattering + a.transmittance * b.scattering};
}
namespace {
double endDistance(const AtmosphereSettings &s, glm::dvec3 p, glm::dvec3 d) {
    const double r = s.planetRadius, rt = r + s.atmosphereHeight, b = glm::dot(p, d),
                 c = glm::dot(p, p);
    const double top = -b + std::sqrt(std::max(0., b * b - c + rt * rt));
    double disc = b * b - c + r * r;
    if (disc >= 0) {
        double near = -b - std::sqrt(disc);
        if (near >= 0)
            return std::min(top, near);
    }
    return std::max(0., top);
}
void medium(const AtmosphereSettings &s, glm::dvec3 p, glm::dvec3 &ext, glm::dvec3 &ray,
            double &mie) {
    const double h = std::max(0., glm::length(p) - s.planetRadius);
    ray = glm::dvec3(s.rayleigh) * std::exp(-h / s.rayleighHeight);
    mie = s.mieScattering * s.aerosolDensity * std::exp(-h / s.mieHeight);
    ext = ray +
          glm::dvec3((s.mieScattering + s.mieAbsorption) * s.aerosolDensity *
                     std::exp(-h / s.mieHeight)) +
          glm::dvec3(s.ozone) * std::max(0., 1 - std::abs(h - s.ozoneCentre) / s.ozoneWidth);
}
glm::dvec3 sunlight(const AtmosphereSettings &s, glm::dvec3 p, glm::dvec3 d, uint32_t steps) {
    const double b = glm::dot(p, d), c = glm::dot(p, p), r = s.planetRadius;
    if (b < 0 && b * b - c + r * r > 0)
        return glm::dvec3(0);
    const double dt = endDistance(s, p, d) / steps;
    glm::dvec3 tau(0), ext, ray;
    double mie;
    for (uint32_t i = 0; i < steps; ++i) {
        medium(s, p + d * ((i + .5) * dt), ext, ray, mie);
        tau += ext * dt;
    }
    return glm::exp(-tau);
}
} // namespace
AtmosphericTransport atmosphereReference(const AtmosphereSettings &s, double altitude,
                                         glm::dvec3 ray, glm::dvec3 sun, double distance,
                                         uint32_t steps, uint32_t sunSteps,
                                         double boundaryDistance) {
    validateAtmosphere(s);
    if (!steps || !sunSteps || !std::isfinite(boundaryDistance) || boundaryDistance < 0 ||
        !std::isfinite(altitude) || !std::isfinite(distance) || distance < 0 ||
        !std::isfinite(glm::length(ray)) || glm::length(ray) == 0 ||
        !std::isfinite(glm::length(sun)) || glm::length(sun) == 0)
        throw std::invalid_argument("Invalid atmosphere reference ray");
    ray = glm::normalize(ray);
    sun = glm::normalize(sun);
    glm::dvec3 p(0, s.planetRadius + std::max(.01, altitude), 0);
    double dt = std::min(distance, endDistance(s, p, ray)) / steps;
    double mu = glm::dot(ray, sun), g = s.mieG;
    const double pi = 3.141592653589793;
    double phaseR = 3 * (1 + mu * mu) / (16 * pi),
           phaseM = (1 - g * g) / (4 * pi * std::pow(1 + g * g - 2 * g * mu, 1.5));
    AtmosphericTransport out;
    for (uint32_t i = 0; i < steps; ++i) {
        glm::dvec3 q = p + ray * ((i + .5) * dt), ext, r;
        double m;
        medium(s, q, ext, r, m);
        auto source = (r * phaseR + m * phaseM) * sunlight(s, q, sun, sunSteps);
        if (boundaryDistance > 0 && s.boundaryHaze > 0) {
            auto local = q - p;
            double ramp = std::clamp(
                (std::max(std::abs(local.x), std::abs(local.z)) / boundaryDistance - .65) / .35, 0.,
                1.);
            double density = s.boundaryHaze * .001 * ramp * ramp * (3 - 2 * ramp) *
                             std::exp(-std::max(0., glm::length(q) - s.planetRadius) / 2000);
            auto sky = glm::mix(glm::dvec3(.12, .22, .4), glm::dvec3(.035, .11, .3),
                                std::sqrt(std::clamp(ray.y, 0., 1.)));
            sky = glm::mix(glm::dvec3(.015, .018, .02), sky, std::clamp(sun.y * 4 + .15, 0., 1.)) *
                  .08;
            ext += density;
            source += density * sky;
        }
        auto t = glm::exp(-ext * dt);
        for (int k = 0; k < 3; ++k)
            out.scattering[k] +=
                out.transmittance[k] * source[k] * (ext[k] > 1e-20 ? (1 - t[k]) / ext[k] : dt);
        out.transmittance *= t;
    }
    return out;
}
glm::dvec3 atmosphereMultipleReference(const AtmosphereSettings &s, double altitude,
                                       double sunCosine, uint32_t directions, uint32_t steps) {
    validateAtmosphere(s);
    if (!directions || !steps || !std::isfinite(altitude) || !std::isfinite(sunCosine) ||
        std::abs(sunCosine) > 1)
        throw std::invalid_argument("Invalid multiple scattering reference");
    const double pi = 3.141592653589793;
    glm::dvec3 p(0, s.planetRadius + std::max(1., altitude), 0),
        sun(std::sqrt(1 - sunCosine * sunCosine), sunCosine, 0), sum(0), factor(0);
    for (uint32_t i = 0; i < directions; ++i) {
        double y = 1 - 2 * (i + .5) / directions, az = i * 2.399963229728653;
        glm::dvec3 ray(std::sqrt(1 - y * y) * std::cos(az), y, std::sqrt(1 - y * y) * std::sin(az));
        double end = endDistance(s, p, ray), dt = end / steps;
        glm::dvec3 throughput(1), radiance(0), bounce(0);
        for (uint32_t j = 0; j < steps; ++j) {
            glm::dvec3 q = p + ray * ((j + .5) * dt), ext, r;
            double m;
            medium(s, q, ext, r, m);
            auto t = glm::exp(-ext * dt), source = (r + m) * sunlight(s, q, sun, 128) / (4 * pi);
            for (int k = 0; k < 3; ++k) {
                double integral = ext[k] > 1e-20 ? (1 - t[k]) / ext[k] : dt;
                radiance[k] += throughput[k] * source[k] * integral;
                bounce[k] += throughput[k] * (r[k] + m) * integral;
            }
            throughput *= t;
        }
        auto q = p + ray * end;
        if (glm::length(q) <= s.planetRadius + 1) {
            auto n = glm::normalize(q);
            radiance += throughput * double(s.groundAlbedo) * std::max(0., glm::dot(n, sun)) *
                        sunlight(s, q + n, sun, 128) / pi;
        }
        sum += radiance;
        factor += bounce;
    }
    return sum / double(directions) /
           glm::max(glm::dvec3(.02), glm::dvec3(1) - factor / double(directions));
}
const char *atmosphereFallbackName(AtmosphereFallback f) {
    const char *names[] = {"None", "Disabled", "Unsupported", "Budget", "Diagnostic"};
    return names[std::min(uint32_t(f), 4u)];
}
} // namespace ow

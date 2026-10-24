#pragma once
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
namespace ow {
enum class AtmosphereMode : uint32_t { Off, Analytic, Physical };
enum class AtmosphereQuality : uint32_t { Normal, High };
enum class AtmosphereFallback : uint32_t { None, Disabled, Unsupported, Budget, Diagnostic };
struct AtmosphereSettings {
    AtmosphereMode mode = AtmosphereMode::Off;
    AtmosphereQuality quality = AtmosphereQuality::Normal;
    bool aerialPerspective = true;
    bool multipleScattering = true; // Reference comparison; normally enabled.
    float datum = 0, planetRadius = 6360000, atmosphereHeight = 100000;
    glm::vec3 rayleigh{5.802e-6f, 13.558e-6f, 33.1e-6f};
    float rayleighHeight = 8000;
    float mieScattering = 3.996e-6f, mieAbsorption = .444e-6f, mieHeight = 1200, mieG = .8f;
    glm::vec3 ozone{.650e-6f, 1.881e-6f, .085e-6f};
    float ozoneCentre = 25000, ozoneWidth = 15000;
    float aerosolDensity = 1, groundAlbedo = .1f, sunRadius = .004675f;
    float environmentMultiplier = 1, boundaryHaze = 0;
    uint32_t diagnostic = 0; // 0 shaded, 1 transmittance, 2 scattering
};
struct AtmosphereStats {
    AtmosphereMode effective = AtmosphereMode::Off;
    AtmosphereFallback fallback = AtmosphereFallback::Disabled;
    uint64_t bytes = 0, profileUpdates = 0, skyUpdates = 0, environmentUpdates = 0;
    uint64_t transmittanceUpdates = 0, multipleScatteringUpdates = 0;
    bool timingsAvailable = false;
    double lutMilliseconds = 0, environmentMilliseconds = 0, compositeMilliseconds = 0;
};
struct AtmosphereSnapshot {
    glm::uvec2 skyExtent{};
    uint32_t volumeSize = 0, volumeDepth = 0;
    float aerialStartDistance = 0; // View metres; nonzero when local volumetrics replace the prefix.
    std::vector<glm::vec4> transmittance, multipleScattering, sky, aerialTransmittance,
        aerialScattering;
    std::array<glm::vec4, 9> irradiance{};
};
// Explicit float4 ABI; scattering and IBL are normalized to unit solar
// illuminance.
struct AtmosphereUniform {
    glm::mat4 inverseRaster{1}, current{1}, previous{1};
    glm::vec4 planet{}, rayleigh{}, mie{}, ozone{}, profile{}, sun{}, illumination{}, view{},
        options{}, extent{}, haze{};
};
static_assert(sizeof(AtmosphereUniform) == 368);
struct AtmosphericTransport {
    glm::dvec3 transmittance{1}, scattering{0};
};
void validateAtmosphere(const AtmosphereSettings &);
uint64_t atmosphereProfileKey(const AtmosphereSettings &);
AtmosphericTransport composeTransport(const AtmosphericTransport &near,
                                      const AtmosphericTransport &far);
// Independent slow double precision midpoint reference, unit white sun, no MS
// closure. Positive boundaryDistance enables the optional mixed haze reference.
AtmosphericTransport atmosphereReference(const AtmosphereSettings &, double altitude,
                                         glm::dvec3 ray, glm::dvec3 sun, double distance,
                                         uint32_t steps = 1024, uint32_t sunSteps = 256,
                                         double boundaryDistance = 0);
// Higher-sample angular quadrature of the isotropic multiple-scattering closure.
// This validates the LUT approximation, not full volumetric path tracing.
glm::dvec3 atmosphereMultipleReference(const AtmosphereSettings &, double altitude,
                                       double sunCosine, uint32_t directions = 256,
                                       uint32_t steps = 256);
const char *atmosphereFallbackName(AtmosphereFallback);
} // namespace ow

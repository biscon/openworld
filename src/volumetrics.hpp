#pragma once
#include "atmosphere.hpp"
#include "scene.hpp"
namespace ow {
enum class VolumetricQuality : uint32_t { Normal, High };
enum class VolumetricFallback : uint32_t { None, Disabled, Unsupported, Budget, Diagnostic, Empty };
enum class FogShape : uint32_t { Sphere, Box };
constexpr double volumetricDepthOffset = 16;
struct VolumetricSettings {
    bool enabled = false, temporal = true;
    VolumetricQuality quality = VolumetricQuality::Normal;
    float distance = 1000, extinction = 0, height = 0, scaleHeight = 40;
    float localDensity = 1; // Multiplies authored volumes, independently of height fog.
    glm::vec3 albedo{.95f};
    float anisotropy = .5f, historyWeight = .9f;
    uint32_t samples = 2,
             diagnostic = 0; // shaded, T, S, coverage, history rejection
};
struct VolumetricCapacity {
    uint32_t volumes = 32, lights = 128, clusterIndices = 32;
    uint64_t bytes = 384ull << 20;
};
struct FogVolumeHandle {
    uint32_t slot = ~0u, generation = 0;
};
struct FogVolume {
    WorldPosition position;
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 extent{10}, albedo{.95f};     // Sphere radius is extent.x; box half extents.
    float extinction = .01f, falloff = .2f; // Fraction of radius/half extent.
    FogShape shape = FogShape::Sphere;
};
struct VolumetricStats {
    bool active = false, historyValid = false, timingsAvailable = false;
    VolumetricFallback fallback = VolumetricFallback::Disabled;
    glm::uvec3 grid{1};
    uint64_t bytes = 0;
    uint32_t volumes = 0, selectedLights = 0, excludedLights = 0, overflowClusters = 0;
    uint32_t rejectedCells = 0;
    double assignmentMilliseconds = 0, injectionMilliseconds = 0, temporalMilliseconds = 0,
           integrationMilliseconds = 0, skyMilliseconds = 0, compositeMilliseconds = 0;
};
struct VolumetricSnapshot {
    glm::uvec3 grid{};
    float preExposure = 1;
    std::vector<glm::vec4> extinction, source, transmittance, scattering, sky, skyTransmittance;
};
// Float4-only GPU records; matrices transform camera-relative geographic
// positions.
struct FogVolumeRecord {
    glm::mat4 inverse{1};
    glm::vec4 extent{}, albedoDensity{}, options{};
};
struct VolumetricUniform {
    glm::mat4 inverseViewProjection{1}, current{1}, previous{1};
    glm::vec4 curvature{}, previousCurvature{}, originDelta{}, grid{}, depth{}, height{}, albedo{},
        temporal{}, counts{}, background{}, sun{}, illumination{}, jitter{};
};
static_assert(sizeof(FogVolumeRecord) == 112 && sizeof(VolumetricUniform) == 400);
void validateVolumetrics(const VolumetricSettings &);
void validate(const FogVolume &);
glm::uvec3 volumetricGrid(uint32_t width, uint32_t height, VolumetricQuality);
double volumetricDepth(double cell, uint32_t cells, double distance);
double fogDensity(const FogVolume &, glm::dvec3 relativePoint);
glm::dvec3 uncurvePosition(glm::dvec3, glm::vec4);
AtmosphericTransport homogeneousTransport(glm::dvec3 extinction, glm::dvec3 source, double distance);
const char *volumetricFallbackName(VolumetricFallback);
} // namespace ow

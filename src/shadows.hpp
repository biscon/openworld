#pragma once
#include "scene.hpp"
#include "visibility.hpp"
#include <array>
namespace ow {
constexpr uint32_t shadowCascades = 5, shadowCoverageSize = 32;
enum class ShadowMode : uint32_t { Off, CSM };
enum class ShadowFallback : uint32_t { None, UnsupportedDepth, Capacity, Wireframe };
struct ShadowSettings {
    ShadowMode mode = ShadowMode::CSM;
    float nearDistance = 1000, farDistance = 0; // Zero follows terrain view distance.
    float biasTexels = .75f, normalBiasTexels = .25f;
    uint32_t diagnostic = 0;
};
struct ShadowCapacity {
    uint32_t resolution = 2048, casters = 65536;
    uint64_t bytes = 128ull << 20;
};
struct ShadowStats {
    ShadowMode requested = ShadowMode::Off, effective = ShadowMode::Off;
    ShadowFallback fallback = ShadowFallback::None;
    uint32_t casters = 0, overflow = 0, incomplete = 0;
    std::array<uint32_t, shadowCascades> cascadeCasters{}, triangles{};
    std::array<double, shadowCascades> rasterMilliseconds{};
    uint64_t allocatedBytes = 0;
    double preparationMilliseconds = 0, totalMilliseconds = 0;
    float nearDistance = 0, farDistance = 0;
    bool completed = false, timingsAvailable = false;
};
struct ShadowUniform {
    std::array<glm::mat4, shadowCascades> matrices{};
    std::array<glm::vec4, shadowCascades> ranges{}; // near, far, world texel, inverse depth span
    glm::vec4 forward{0};
    glm::vec4 sun{0};      // direction to sun, horizon fade
    glm::vec4 sampling{0}; // depth bias, normal bias, resolution, diagnostic
    glm::uvec4 options{
        0}; // enabled, active cascades, valid bits 0..4 / coverage bits 8..12, hardware filtering
};
static_assert(sizeof(ShadowUniform) == 464);
struct ShadowCascade {
    glm::dmat3 basis{1};
    glm::dvec2 centre{};
    double radius = 1;
    float nearDistance = 0, farDistance = 0;
};
struct ShadowSnapshot {
    uint32_t resolution = 0;
    ShadowUniform uniform;
    std::vector<float> depth;
    std::vector<uint32_t> coverage;
};
void validate(const ShadowSettings &);
glm::dmat3 shadowBasis(glm::dvec3 sun);
std::array<ShadowCascade, shadowCascades> shadowCascadeLayout(const Camera &, TerrainGrid, float aspect,
                                                              glm::dvec3 sun, const ShadowSettings &,
                                                              float farDistance, uint32_t resolution);
VisibilityBounds shadowLightBounds(const VisibilityBounds &, const glm::dmat3 &);
} // namespace ow

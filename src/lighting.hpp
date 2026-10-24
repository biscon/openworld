#pragma once
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace ow {
enum class ShadingMode : uint32_t { Pbr, Legacy };
enum class ToneMapper : uint32_t { AcesFitted, Agx };
struct MaterialSettings {
    glm::vec3 baseColour{0.40f, 0.46f, 0.38f};
    float roughness = .8f;
    float metallic = 0, ao = 1;
    glm::vec3 emissive{0};
};
struct LightingSettings {
    bool sunEnabled = true, environmentEnabled = true;
    glm::vec3 sunDirection{-0.6f, .8f, .35f}, sunColour{1};
    float sunLux = 100000, environmentNits = 3000, environmentRotation = 0;
};
struct OutputSettings {
    float ev100 = 15, compensation = 0;
    ToneMapper toneMapper = ToneMapper::AcesFitted;
};
struct HdrSnapshot {
    uint32_t width = 0, height = 0;
    float preExposure = 1;
    std::vector<glm::vec4> pixels;
};
struct RenderingStats {
    uint64_t hdrBytes = 0, lightingBytes = 0;
    double sceneMilliseconds = 0, outputMilliseconds = 0;
    double frameMilliseconds = 0; // Uploads/generation through output and UI; excludes presentation wait.
    // Current-frame CPU work, excluding GPU/fence/presentation waits. Streamed terrain only.
    double terrainSelectionMilliseconds = 0, terrainPreparationMilliseconds = 0,
           terrainFinalizeMilliseconds = 0;
    double lightingStartupMilliseconds = 0; // CPU bake, uploads; excludes pipelines.
    bool timingsAvailable = false;
};
// Explicit float4 layout shared with lighting.hlsli. No bools in GPU storage.
struct LightingUniform {
    glm::mat4 viewProjection{1};
    glm::vec4 camera{0}, sunDirection{0}, sunColour{0}, environment{0};
    glm::vec4 baseRoughness{0}, material{0}, emissive{0}, output{0};
    std::array<glm::vec4, 9> irradiance{};
};
static_assert(sizeof(LightingUniform) == 336);
struct EnvironmentBake {
    static constexpr uint32_t cubeSize = 64, levels = 7, lutSize = 128;
    std::vector<glm::vec4> cube; // mip-major, face-major; Vulkan cube face order
    std::vector<glm::vec4> brdf;
    std::array<glm::vec4, 9> irradiance{};
};
float linearToSrgb(float);
float srgbToLinear(float);
float exposure(const OutputSettings &);
glm::vec3 toneMap(glm::vec3, ToneMapper);
glm::vec3 environmentRadiance(glm::vec3 direction);
glm::vec3 cubeDirection(uint32_t face, float u, float v);
std::array<float, 9> shBasis(glm::vec3);
glm::vec3 evaluateIrradiance(const std::array<glm::vec4, 9> &, glm::vec3);
const EnvironmentBake &environmentBake();
glm::vec3 directBrdf(const MaterialSettings &, glm::vec3 n, glm::vec3 v, glm::vec3 l);
} // namespace ow

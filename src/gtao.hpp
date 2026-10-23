#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
namespace ow {
enum class GtaoMode : uint32_t { Off, Half, Full };
enum class GtaoDiagnostic : uint32_t { Shaded, Visibility, Raw, Normals, Edges };
enum class GtaoFallback : uint32_t { None, Unsupported, Budget, Wireframe };
struct GtaoSettings {
    GtaoMode mode = GtaoMode::Off;
    float radius = 1.f;
    GtaoDiagnostic diagnostic = GtaoDiagnostic::Shaded;
};
struct GtaoStats {
    GtaoMode requested = GtaoMode::Off, effective = GtaoMode::Off;
    GtaoFallback fallback = GtaoFallback::None;
    uint64_t allocatedBytes = 0;
    uint32_t width = 0, height = 0;
    bool compact = false, timingsAvailable = false;
    GtaoMode completedMode = GtaoMode::Off;
    double prepareMilliseconds = 0, evaluateMilliseconds = 0, filterMilliseconds = 0;
};
struct GtaoSnapshot {
    uint32_t width = 0, height = 0, workWidth = 0, workHeight = 0;
    GtaoMode mode = GtaoMode::Off;
    GtaoDiagnostic diagnostic = GtaoDiagnostic::Shaded;
    std::vector<glm::vec4> pixels; // Native final visibility (RGB), or selected diagnostic.
    std::vector<float> depth, raw, filtered,
        edges;                     // Working resolution; zero depth means sky.
    std::vector<uint32_t> offsets; // Native representative offset x + 2*y.
};
void validateGtao(const GtaoSettings &);
glm::uvec2 gtaoExtent(uint32_t width, uint32_t height, GtaoMode);
float gtaoViewDepth(float reversedDepth);
float gtaoFade(float viewDepth, float nativeRadiusPixels);
// Returns the first nearest non-sky sample in row-major order, or 0 for all
// sky.
uint32_t gtaoRepresentative(const float reversedDepth[4]);
} // namespace ow

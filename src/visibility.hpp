#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
namespace ow {
enum class VisibilityMode : uint32_t { Direct, Indirect, HiZ };
enum class VisibilityFallback : uint32_t { None, UnsupportedIndirect, UnsupportedHiZ, Capacity, Wireframe };
struct VisibilityTriangles {
    uint64_t terrain = 0, meshes = 0;
    uint64_t total() const { return terrain + meshes; }
};
struct VisibilityStats {
    VisibilityMode requested = VisibilityMode::Indirect, effective = VisibilityMode::Direct;
    VisibilityFallback fallback = VisibilityFallback::None;
    uint32_t candidates = 0, batches = 0, submitted = 0, frustumRejected = 0, occlusionRejected = 0;
    // Opaque/masked draws only; transparent draws remain on the separate blend
    // path.
    VisibilityTriangles selectedTriangles{}, depthTriangles{}, colourTriangles{};
    VisibilityMode completedMode = VisibilityMode::Direct;
    uint64_t completedFrame = 0;
    uint64_t allocatedBytes = 0;
    double depthMilliseconds = 0, pyramidMilliseconds = 0, cullingMilliseconds = 0, colourMilliseconds = 0;
    bool timingsAvailable = false, countsAvailable = false;
};
struct VisibilityPyramidSnapshot {
    uint32_t width = 0, height = 0;
    std::vector<std::vector<float>> levels;
};
struct VisibilityBounds {
    glm::vec3 lo{0}, hi{0};
    bool valid = false;
};
VisibilityBounds transformBounds(glm::vec3 lo, glm::vec3 hi, const glm::mat4 &);
} // namespace ow

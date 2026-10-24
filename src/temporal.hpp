#pragma once
#include "scene.hpp"
#include <array>
#include <stdexcept>
#include <vector>
namespace ow {
struct TaaSettings {
    bool enabled = false;
    float historyWeight = .9f;
    bool thinGeometry = true;
};
struct BloomSettings {
    float intensity = 0, threshold = 10000, knee = .5f, radius = 1;
};
enum class PostFallback : uint32_t { None, Disabled, Unsupported, Budget, Diagnostic };
enum class TemporalReset : uint32_t { None, Initial, Explicit, Extent, CameraCut, Settings };
struct PostStats {
    bool thinGeometry = false;
    PostFallback thinGeometryFallback = PostFallback::Disabled;
    uint64_t thinGeometryBytes = 0;
    bool taa = false, bloom = false, historyValid = false, timingsAvailable = false;
    PostFallback taaFallback = PostFallback::Disabled, bloomFallback = PostFallback::Disabled;
    TemporalReset reset = TemporalReset::Initial;
    uint64_t taaBytes = 0, bloomBytes = 0;
    uint32_t sequence = 0, rejectedSurfaces = 0, trackingOverflow = 0, bloomLevels = 0;
    double taaMilliseconds = 0, bloomMilliseconds = 0;
    std::array<double, 6> bloomDownMilliseconds{}, bloomUpMilliseconds{};
};
struct PostSnapshot {
    uint32_t width = 0, height = 0;
    float preExposure = 1;
    std::vector<glm::vec4> resolved, motion;
    std::vector<uint8_t> thinGeometry;
    std::vector<glm::vec4> thinCoverage; // coverage, confidence, near/far depth in km.
    std::vector<float> reactivity,
        depth; // Negative depth marks rejected history; magnitude is view metres.
    std::vector<std::vector<glm::vec4>> bloom;
    std::vector<glm::uvec2> bloomSizes;
};
struct TemporalView {
    glm::mat4 current{1}, previous{1}, raster{1};
    glm::vec4 curvature{0}, previousCurvature{0};
    glm::vec4 originDelta{0}; // Current origin relative to previous origin.
    glm::vec4 pixels{0};      // width, height, jitter x/y in pixels
    glm::vec4 state{0};       // active, history valid, exposure ratio, terrain record capacity
};
static_assert(sizeof(TemporalView) == 272);
// Separate from the 128-byte draw ABI. Shader only accesses unchanged resident
// heights.
struct TemporalTerrainRecord {
    std::array<glm::uvec4, 8> draw{};
    std::array<glm::vec4, 12> weights{};
    glm::vec4 observer{0}, camera{0};
    glm::vec4 valid{0};
};
static_assert(sizeof(TemporalTerrainRecord) == 368);
struct TemporalTerrainKey {
    std::array<uint64_t, 8> words{};
    bool operator<(const TemporalTerrainKey &b) const { return words < b.words; }
    bool operator==(const TemporalTerrainKey &b) const { return words == b.words; }
};
class TerrainHistory {
    struct Entry {
        TemporalTerrainKey key;
        TemporalTerrainRecord value;
    };
    std::vector<Entry> previous_, pending_;
    std::vector<uint32_t> previousOrder_;
    size_t capacity_;

  public:
    explicit TerrainHistory(size_t capacity) : capacity_(capacity) {
        if (capacity > UINT32_MAX)
            throw std::invalid_argument("Terrain history capacity exceeds index range");
        previous_.reserve(capacity);
        pending_.reserve(capacity);
        previousOrder_.reserve(capacity);
    }
    void begin(bool valid) {
        pending_.clear();
        if (!valid) {
            previous_.clear();
            previousOrder_.clear();
        }
    }
    TemporalTerrainRecord append(const TemporalTerrainKey &, const TemporalTerrainRecord &);
    void submitted();
};
const char *postFallbackName(PostFallback);
const char *temporalResetName(TemporalReset);
void validatePost(const TaaSettings &, const BloomSettings &);
glm::vec2 temporalJitter(uint64_t sequence);
glm::mat4 jitterProjection(glm::mat4, glm::vec2 pixels, glm::uvec2 extent);
glm::mat4 conservativeProjection(glm::mat4, glm::uvec2 extent);
std::vector<glm::uvec2> bloomExtents(uint32_t width, uint32_t height);
float bloomContribution(float luminance, float threshold, float knee);
} // namespace ow

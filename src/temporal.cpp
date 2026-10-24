#include "temporal.hpp"
#include <cmath>
#include <numeric>
namespace ow {
const char *postFallbackName(PostFallback r) {
    switch (r) {
    case PostFallback::None:
        return "available";
    case PostFallback::Disabled:
        return "disabled";
    case PostFallback::Unsupported:
        return "unsupported device";
    case PostFallback::Budget:
        return "resource budget";
    case PostFallback::Diagnostic:
        return "diagnostic view";
    }
    return "unknown";
}
const char *temporalResetName(TemporalReset r) {
    switch (r) {
    case TemporalReset::None:
        return "none";
    case TemporalReset::Initial:
        return "initial frame";
    case TemporalReset::Explicit:
        return "explicit reset";
    case TemporalReset::Extent:
        return "scene resize";
    case TemporalReset::CameraCut:
        return "camera cut";
    case TemporalReset::Settings:
        return "settings change";
    }
    return "unknown";
}
void validatePost(const TaaSettings &t, const BloomSettings &b) {
    if (!std::isfinite(t.historyWeight) || t.historyWeight < 0 || t.historyWeight > .9f ||
        !std::isfinite(b.intensity) || b.intensity < 0 || b.intensity > 2 || !std::isfinite(b.threshold) ||
        b.threshold < 0 || b.threshold > 1e6f || !std::isfinite(b.knee) || b.knee < 0 || b.knee > 1 ||
        !std::isfinite(b.radius) || b.radius < .5f || b.radius > 2)
        throw std::invalid_argument("Invalid TAA/bloom settings");
}
const char *antialiasingName(AntialiasingMode mode) {
    switch (mode) {
    case AntialiasingMode::Off:
        return "Off";
    case AntialiasingMode::Fxaa:
        return "FXAA";
    case AntialiasingMode::Smaa1x:
        return "SMAA 1x";
    case AntialiasingMode::Taa:
        return "TAA";
    }
    return "Invalid";
}
glm::vec2 temporalJitter(uint64_t sequence) {
    auto radical = [](uint32_t n, uint32_t base) {
        float sum = 0, scale = 1;
        while (n) {
            scale /= float(base);
            sum += float(n % base) * scale;
            n /= base;
        }
        return sum - .5f;
    };
    uint32_t n = uint32_t(sequence % 8) + 1;
    return {radical(n, 2), radical(n, 3)};
}
glm::mat4 jitterProjection(glm::mat4 m, glm::vec2 p, glm::uvec2 e) {
    if (!e.x || !e.y)
        throw std::invalid_argument("Empty temporal extent");
    for (int c = 0; c < 4; ++c) {
        m[c][0] += 2 * p.x / float(e.x) * m[c][3];
        m[c][1] += 2 * p.y / float(e.y) * m[c][3];
    }
    return m;
}
glm::mat4 conservativeProjection(glm::mat4 m, glm::uvec2 e) {
    for (int c = 0; c < 4; ++c) {
        m[c][0] /= 1 + 1.f / e.x;
        m[c][1] /= 1 + 1.f / e.y;
    }
    return m;
}
std::vector<glm::uvec2> bloomExtents(uint32_t w, uint32_t h) {
    if (!w || !h)
        throw std::invalid_argument("Empty bloom extent");
    std::vector<glm::uvec2> result;
    for (uint32_t i = 0; i < 6; ++i) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        result.push_back({w, h});
        if (w == 1 && h == 1)
            break;
    }
    return result;
}
float bloomContribution(float y, float t, float knee) {
    float k = t * knee, soft = std::clamp(y - t + k, 0.f, 2 * k);
    soft = soft * soft / std::max(4 * k, 1e-6f);
    return std::max(soft, y - t) / std::max(y, 1e-6f);
}
TemporalTerrainRecord TerrainHistory::append(const TemporalTerrainKey &key,
                                             const TemporalTerrainRecord &value) {
    if (pending_.size() >= capacity_)
        return {};
    auto it = std::lower_bound(previousOrder_.begin(), previousOrder_.end(), key,
                               [&](uint32_t a, const auto &b) { return previous_[a].key < b; });
    auto result = it != previousOrder_.end() && previous_[*it].key == key ? previous_[*it].value
                                                                          : TemporalTerrainRecord{};
    pending_.push_back({key, value});
    return result;
}
void TerrainHistory::submitted() {
    previous_.swap(pending_);
    previousOrder_.resize(previous_.size());
    std::iota(previousOrder_.begin(), previousOrder_.end(), 0u);
    std::sort(previousOrder_.begin(), previousOrder_.end(),
              [&](uint32_t a, uint32_t b) { return previous_[a].key < previous_[b].key; });
}
} // namespace ow

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ow {
struct RenderExtent {
    uint32_t width, height;
};

// Zero requested height selects native rendering. Limits apply to scene images,
// independently of the output. Call only for a nonzero drawable extent.
inline RenderExtent sceneRenderExtent(uint32_t outputWidth, uint32_t outputHeight, uint32_t requestedHeight,
                                      uint32_t maxWidth = 16384, uint32_t maxHeight = 16384) {
    if (!outputWidth || !outputHeight || requestedHeight > 16384 || !maxWidth || !maxHeight)
        throw std::invalid_argument("Invalid render resolution");
    maxWidth = std::min(maxWidth, 16384u);
    maxHeight = std::min(maxHeight, 16384u);
    const double h = requestedHeight ? requestedHeight : outputHeight;
    const double w = std::max(1.0, std::round(h * outputWidth / outputHeight));
    const double scale = std::min({1.0, maxWidth / w, maxHeight / h});
    return {std::clamp(uint32_t(std::round(w * scale)), 1u, maxWidth),
            std::clamp(uint32_t(std::round(h * scale)), 1u, maxHeight)};
}
} // namespace ow

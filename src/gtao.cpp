#include "gtao.hpp"
#include "camera.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace ow {
void validateGtao(const GtaoSettings &s) {
    if (uint32_t(s.mode) > uint32_t(GtaoMode::Full) ||
        uint32_t(s.diagnostic) > uint32_t(GtaoDiagnostic::Edges) || !std::isfinite(s.radius) ||
        s.radius < .1f || s.radius > 5.f)
        throw std::invalid_argument("GTAO requires a valid mode/diagnostic and radius in 0.1..5 m");
}
glm::uvec2 gtaoExtent(uint32_t w, uint32_t h, GtaoMode m) {
    if (!w || !h || w > 16384 || h > 16384 || uint32_t(m) > 2)
        throw std::invalid_argument("Invalid GTAO extent/mode");
    return m == GtaoMode::Half ? glm::uvec2((w + 1) / 2, (h + 1) / 2) : glm::uvec2(w, h);
}
float gtaoViewDepth(float d) {
    if (!std::isfinite(d) || d <= 0 || d > 1)
        return 0;
    constexpr float n = Camera::nearPlane, f = Camera::farPlane;
    return n * f / (n + d * (f - n));
}
float gtaoFade(float z, float r) {
    if (!std::isfinite(z) || !std::isfinite(r) || z <= 0)
        return 0;
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.f, 1.f);
        return x * x * (3 - 2 * x);
    };
    return (1 - smooth((z - 100) / 100)) * smooth((r - 1) / 3);
}
uint32_t gtaoRepresentative(const float d[4]) {
    uint32_t best = 0;
    float nearest = 0;
    for (uint32_t i = 0; i < 4; ++i)
        if (std::isfinite(d[i]) && d[i] > nearest && d[i] <= 1) {
            nearest = d[i];
            best = i;
        }
    return best;
}
} // namespace ow

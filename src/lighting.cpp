#include "lighting.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace ow {
namespace {
constexpr float pi = 3.14159265358979323846f;
float radical(uint32_t b) {
    b = (b << 16) | (b >> 16);
    b = ((b & 0x55555555u) << 1) | ((b & 0xaaaaaaaau) >> 1);
    b = ((b & 0x33333333u) << 2) | ((b & 0xccccccccu) >> 2);
    b = ((b & 0x0f0f0f0fu) << 4) | ((b & 0xf0f0f0f0u) >> 4);
    b = ((b & 0x00ff00ffu) << 8) | ((b & 0xff00ff00u) >> 8);
    return float(b) * 2.3283064365386963e-10f;
}
glm::vec3 halfVector(uint32_t i, uint32_t count, float a) {
    float phi = 2 * pi * float(i) / float(count), y = radical(i);
    float c = std::sqrt((1 - y) / (1 + (a * a - 1) * y)), s = std::sqrt(std::max(0.f, 1 - c * c));
    return {std::cos(phi) * s, std::sin(phi) * s, c};
}
float visibility(float nv, float nl, float a) {
    float a2 = a * a;
    return .5f /
           std::max(nl * std::sqrt(nv * nv * (1 - a2) + a2) + nv * std::sqrt(nl * nl * (1 - a2) + a2), 1e-7f);
}
glm::vec3 transform(glm::vec3 v, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    return {glm::dot(v, a), glm::dot(v, b), glm::dot(v, c)};
}
} // namespace
float linearToSrgb(float x) { return x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1 / 2.4f) - .055f; }
float srgbToLinear(float x) { return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f); }
float exposure(const OutputSettings &s) {
    if (!std::isfinite(s.ev100) || !std::isfinite(s.compensation))
        throw std::invalid_argument("Non-finite exposure");
    return std::exp2(std::clamp(s.compensation, -10.f, 10.f) - std::clamp(s.ev100, 0.f, 24.f)) / 1.2f;
}
glm::vec3 toneMap(glm::vec3 v, ToneMapper mapper) {
    v = glm::max(v, glm::vec3(0));
    if (mapper == ToneMapper::AcesFitted) {
        v = transform(v, {.59719f, .35458f, .04823f}, {.07600f, .90834f, .01566f},
                      {.02840f, .13383f, .83777f});
        v = (v * (v + .0245786f) - .000090537f) / (v * (.983729f * v + .4329510f) + .238081f);
        v = transform(v, {1.60475f, -.53108f, -.07367f}, {-.10208f, 1.10813f, -.00605f},
                      {-.00327f, -.07276f, 1.07602f});
    } else {
        // Filament AgX neutral, with Rec.709/Rec.2020 primary conversions.
        v = transform(v, {0.5448147465f, 0.3737873980f, 0.0813978554f},
                      {0.1404169482f, 0.7541375545f, 0.1054454973f},
                      {0.0888104197f, 0.1788717565f, 0.7323178238f});
        v = glm::clamp((glm::log2(glm::max(v, glm::vec3(1e-10f))) + 12.47393f) / 16.5f, glm::vec3(0),
                       glm::vec3(1));
        v = ((((((-17.86f * v + 78.01f) * v - 126.7f) * v + 92.06f) * v - 28.72f) * v + 4.361f) * v -
             .1718f) *
                v +
            .002857f;
        v = transform(v, {1.1271005818f, -0.1106066431f, -0.0164939387f},
                      {-0.1413297635f, 1.1578237022f, -0.0164939387f},
                      {-0.1413297635f, -0.1106066431f, 1.2519364066f});
        v = glm::pow(glm::max(v, glm::vec3(0)), glm::vec3(2.2f));
        v = transform(v, {1.6604910015f, -0.5876411379f, -0.0728498636f},
                      {-0.1245504738f, 1.1328998970f, -0.0083494231f},
                      {-0.0181507636f, -0.1005788981f, 1.1187296617f});
    }
    return glm::clamp(v, glm::vec3(0), glm::vec3(1));
}
glm::vec3 environmentRadiance(glm::vec3 d) {
    const glm::vec3 sky = glm::vec3(.65f, .8f, 1.f) / .782572f;
    return glm::mix(glm::vec3(.12f, .1f, .08f), sky, .5f + .5f * d.y) +
           glm::vec3(3.f) *
               std::pow(std::max(0.f, glm::dot(d, glm::normalize(glm::vec3(1, .2f, .3f)))), 32.f);
}
glm::vec3 cubeDirection(uint32_t face, float u, float v) {
    const glm::vec3 d[] = {{1, -v, -u}, {-1, -v, u}, {u, 1, v}, {u, -1, -v}, {u, -v, 1}, {-u, -v, -1}};
    return glm::normalize(d[face]);
}
std::array<float, 9> shBasis(glm::vec3 n) {
    return {.2820947918f,
            .4886025119f * n.y,
            .4886025119f * n.z,
            .4886025119f * n.x,
            1.0925484306f * n.x * n.y,
            1.0925484306f * n.y * n.z,
            .3153915653f * (3 * n.z * n.z - 1),
            1.0925484306f * n.x * n.z,
            .5462742153f * (n.x * n.x - n.y * n.y)};
}
glm::vec3 evaluateIrradiance(const std::array<glm::vec4, 9> &sh, glm::vec3 n) {
    auto b = shBasis(n);
    glm::vec3 e(0);
    for (int i = 0; i < 9; i++)
        e += glm::vec3(sh[i]) * b[i];
    return glm::max(e, glm::vec3(0));
}
const EnvironmentBake &environmentBake() {
    static const EnvironmentBake bake = [] {
        EnvironmentBake out;
        out.cube.reserve(32766);
        out.brdf.reserve(EnvironmentBake::lutSize * EnvironmentBake::lutSize);
        double solidAngle = 0;
        for (uint32_t mip = 0; mip < EnvironmentBake::levels; mip++) {
            uint32_t size = EnvironmentBake::cubeSize >> mip;
            float r = float(mip) / float(EnvironmentBake::levels - 1), a = r * r;
            for (uint32_t face = 0; face < 6; face++)
                for (uint32_t y = 0; y < size; y++)
                    for (uint32_t x = 0; x < size; x++) {
                        float u = (x + .5f) / size * 2 - 1, v = (y + .5f) / size * 2 - 1;
                        glm::vec3 n = cubeDirection(face, u, v), colour(0);
                        if (!mip) {
                            colour = environmentRadiance(n);
                            float w = 4.f / (size * size * std::pow(1 + u * u + v * v, 1.5f));
                            solidAngle += w;
                            auto b = shBasis(n);
                            for (int k = 0; k < 9; k++)
                                out.irradiance[k] += glm::vec4(colour * w * b[k], 0);
                        } else {
                            auto up = std::abs(n.z) < .999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
                            auto t = glm::normalize(glm::cross(up, n)), b = glm::cross(n, t);
                            float sum = 0;
                            for (uint32_t i = 0; i < 128; i++) {
                                auto h0 = halfVector(i, 128, a);
                                auto h = t * h0.x + b * h0.y + n * h0.z;
                                auto l = 2 * glm::dot(n, h) * h - n;
                                float nl = std::max(0.f, glm::dot(n, l));
                                colour += environmentRadiance(l) * nl;
                                sum += nl;
                            }
                            colour /= std::max(sum, 1e-6f);
                        }
                        out.cube.emplace_back(colour, 1);
                    }
        }
        for (int k = 0; k < 9; k++)
            out.irradiance[k] *= float(4 * pi / solidAngle) * (k == 0 ? pi : k < 4 ? 2 * pi / 3 : pi / 4);
        for (uint32_t y = 0; y < EnvironmentBake::lutSize; y++)
            for (uint32_t x = 0; x < EnvironmentBake::lutSize; x++) {
                float nv = (x + .5f) / EnvironmentBake::lutSize, r = (y + .5f) / EnvironmentBake::lutSize,
                      a = std::max(r, .045f);
                a *= a;
                glm::vec3 v(std::sqrt(1 - nv * nv), 0, nv);
                glm::vec2 sum(0);
                for (uint32_t i = 0; i < 256; i++) {
                    auto h = halfVector(i, 256, a);
                    float vh = std::max(0.f, glm::dot(v, h));
                    auto l = 2 * vh * h - v;
                    if (l.z > 0) {
                        float g = 4 * l.z * visibility(nv, l.z, a) * vh / std::max(h.z, 1e-6f),
                              f = std::pow(1 - vh, 5.f);
                        sum += glm::vec2((1 - f) * g, f * g);
                    }
                }
                out.brdf.emplace_back(sum / 256.f, 0, 1);
            }
        return out;
    }();
    return bake;
}
glm::vec3 directBrdf(const MaterialSettings &s, glm::vec3 n, glm::vec3 v, glm::vec3 l) {
    float nv = std::max(glm::dot(n, v), 0.f), nl = std::max(glm::dot(n, l), 0.f);
    if (nv <= 0 || nl <= 0 || glm::dot(v + l, v + l) < 1e-10f)
        return glm::vec3(0);
    auto h = glm::normalize(v + l);
    float nh = std::max(glm::dot(n, h), 0.f), vh = std::max(glm::dot(v, h), 0.f);
    float a = std::clamp(s.roughness, .045f, 1.f);
    a *= a;
    float d = a * a / (pi * std::pow(nh * nh * (a * a - 1) + 1, 2.f));
    auto f0 = glm::mix(glm::vec3(.04f), s.baseColour, s.metallic);
    auto f = f0 + (1.f - f0) * std::pow(1 - vh, 5.f);
    return ((1.f - f) * (1 - s.metallic) * s.baseColour / pi + d * visibility(nv, nl, a) * f) * nl;
}
} // namespace ow

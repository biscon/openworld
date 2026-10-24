#include "vegetation.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
namespace ow {
namespace {
uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float random(uint32_t x) {
    return (hash(x) >> 8) * (1.f / 16777216);
}
float field(float x, float z, bool rolling) {
    return rolling ? 1.8f * std::sin(x * .05f) * std::cos(z * .04f) : 0;
}
void strip(MeshData &m, const std::vector<glm::vec3> &centres, const std::vector<float> &widths,
           glm::vec3 right, bool card) {
    auto first = uint32_t(m.vertices.size());
    for (uint32_t j = 0; j < centres.size(); ++j) {
        float t = float(j) / float(centres.size() - 1);
        auto normal = glm::normalize(glm::cross(right, glm::vec3(0, 1, 0)));
        for (int edge = 0; edge < 2; ++edge)
            m.vertices.push_back({centres[j] + right * widths[j] * (edge ? 1.f : -1.f),
                                  normal,
                                  {right, 1},
                                  {card ? .5f + widths[j] * (edge ? 1 : -1) : float(edge), t}});
    }
    for (uint32_t j = 0; j + 1 < centres.size(); ++j) {
        uint32_t a = first + 2 * j;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
}
float edge(glm::vec2 a, glm::vec2 b, glm::vec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
} // namespace
float vegetationGroundHeight(float x, float z, bool rolling) {
    float ax = std::floor(x), az = std::floor(z), u = x - ax, v = z - az;
    float a = field(ax, az, rolling), b = field(ax + 1, az, rolling), c = field(ax, az + 1, rolling),
          d = field(ax + 1, az + 1, rolling);
    return u > v ? a + u * (b - a) + v * (d - b) : a + u * (d - c) + v * (c - a);
}
VegetationAssets makeVegetationAssets() {
    VegetationAssets out;
    for (uint32_t variant = 0; variant < 2; ++variant)
        for (uint32_t lod = 0; lod < 3; ++lod) {
            auto &m = out.meshes[variant * 6 + lod];
            uint32_t blades = 12 >> lod, segments = 4 - lod;
            for (uint32_t blade = 0; blade < blades; ++blade) {
                uint32_t id = blade * 13 + variant * 177;
                float angle = random(id) * 6.2831853f, length = .65f + .35f * random(id + 1);
                glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                glm::vec3 root = right * (.12f * random(id + 2));
                std::vector<glm::vec3> centres;
                std::vector<float> widths;
                for (uint32_t j = 0; j <= segments; ++j) {
                    float t = float(j) / segments;
                    centres.push_back(root + glm::vec3(0, t * length, 0) + right * (.23f * t * t));
                    widths.push_back(std::max(.0005f, .025f * (1 - t) * (lod ? 1.15f : 1)));
                }
                strip(m, centres, widths, right, false);
            }
            auto &cards = out.meshes[variant * 6 + 3 + lod];
            for (uint32_t k = 0; k < 3; ++k) {
                float a = float(k) * 3.14159265f / 3 + variant * .2f;
                std::vector<glm::vec3> centres;
                std::vector<float> widths;
                uint32_t segments = lod == 0 ? 4 : lod == 1 ? 2 : 1;
                for (uint32_t j = 0; j <= segments; ++j) {
                    float t = float(j) / segments;
                    centres.push_back({0, t, 0});
                    widths.push_back(t < .5f ? .4f : .4f - (t - .5f) * .3f);
                }
                strip(cards, centres, widths, {std::cos(a), 0, std::sin(a)}, true);
            }
        }
    // Orthographic silhouette of the same full-detail tuft; constant safe green
    // extends through transparent pixels. Mips preserve threshold coverage, never
    // black fringes.
    auto &t = out.cards;
    t.width = t.height = 256;
    t.format = TextureFormat::Srgb8;
    t.levels = 9;
    std::vector<uint8_t> level(256 * 256 * 4);
    for (size_t i = 0; i < level.size(); i += 4) {
        level[i] = 91;
        level[i + 1] = 132;
        level[i + 2] = 48;
    }
    const auto &m = out.meshes[0];
    for (size_t i = 0; i < m.indices.size(); i += 3) {
        glm::vec2 p[3];
        for (int k = 0; k < 3; ++k) {
            auto v = m.vertices[m.indices[i + k]].position;
            p[k] = {v.x + .5f, v.y};
        }
        float area = edge(p[0], p[1], p[2]);
        if (std::abs(area) < 1e-9f)
            continue;
        for (uint32_t y = 0; y < 256; ++y)
            for (uint32_t x = 0; x < 256; ++x) {
                glm::vec2 q((x + .5f) / 256, (y + .5f) / 256);
                if (edge(p[0], p[1], q) / area >= 0 && edge(p[1], p[2], q) / area >= 0 &&
                    edge(p[2], p[0], q) / area >= 0)
                    level[(y * 256 + x) * 4 + 3] = 255;
            }
    }
    double coverage = 0;
    for (size_t i = 3; i < level.size(); i += 4)
        coverage += level[i] >= 128;
    coverage /= 256 * 256;
    for (uint32_t n = 256; n; n >>= 1) {
        t.bytes.insert(t.bytes.end(), level.begin(), level.end());
        if (n == 1)
            break;
        uint32_t next = n / 2;
        std::vector<uint8_t> smaller(next * next * 4);
        std::vector<float> alpha(next * next);
        for (uint32_t y = 0; y < next; ++y)
            for (uint32_t x = 0; x < next; ++x) {
                auto i = y * next + x;
                smaller[i * 4] = 91;
                smaller[i * 4 + 1] = 132;
                smaller[i * 4 + 2] = 48;
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                        alpha[i] += level[((2 * y + dy) * n + 2 * x + dx) * 4 + 3] / 4.f;
            }
        // Pick the scale with nearest achievable coverage; tiny mips are quantized.
        float best = 1;
        double error = 1;
        for (int step = 1; step <= 256; ++step) {
            float scale = step / 16.f;
            uint32_t hits = 0;
            for (float a : alpha)
                hits += a * scale >= 127.5f;
            double e = std::abs(double(hits) / alpha.size() - coverage);
            if (e < error) {
                error = e;
                best = scale;
            }
        }
        for (size_t i = 0; i < alpha.size(); ++i)
            smaller[4 * i + 3] = uint8_t(std::clamp(std::lround(alpha[i] * best), 0l, 255l));
        level = std::move(smaller);
    }
    return out;
}
void writeVegetationAssets(const std::filesystem::path &dir, const VegetationAssets &a) {
    std::filesystem::create_directories(dir);
    std::ofstream f(dir / "tufts.bin", std::ios::binary);
    uint32_t magic = 0x56454731;
    f.write(reinterpret_cast<const char *>(&magic), 4);
    for (const auto &m : a.meshes) {
        uint32_t n = uint32_t(m.vertices.size()), k = uint32_t(m.indices.size());
        f.write(reinterpret_cast<const char *>(&n), 4);
        f.write(reinterpret_cast<const char *>(&k), 4);
        f.write(reinterpret_cast<const char *>(m.vertices.data()), n * sizeof(MeshVertex));
        f.write(reinterpret_cast<const char *>(m.indices.data()), k * 4);
    }
    if (!f)
        throw std::runtime_error("Writing vegetation fixture failed");
    writeTexture(dir / "cards.dds", a.cards);
}
VegetationAssets readVegetationAssets(const std::filesystem::path &dir) {
    VegetationAssets a;
    std::ifstream f(dir / "tufts.bin", std::ios::binary);
    uint32_t magic = 0;
    f.read(reinterpret_cast<char *>(&magic), 4);
    if (magic != 0x56454731)
        throw std::runtime_error("Invalid vegetation fixture");
    for (auto &m : a.meshes) {
        uint32_t n = 0, k = 0;
        f.read(reinterpret_cast<char *>(&n), 4);
        f.read(reinterpret_cast<char *>(&k), 4);
        if (!n || n > 4096 || !k || k > 16384 || k % 3)
            throw std::runtime_error("Invalid vegetation mesh size");
        m.vertices.resize(n);
        m.indices.resize(k);
        f.read(reinterpret_cast<char *>(m.vertices.data()), n * sizeof(MeshVertex));
        f.read(reinterpret_cast<char *>(m.indices.data()), k * 4);
        if (!f)
            throw std::runtime_error("Truncated vegetation fixture");
        for (auto index : m.indices)
            if (index >= n)
                throw std::runtime_error("Invalid vegetation mesh index");
        for (auto &v : m.vertices)
            if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) ||
                !std::isfinite(v.position.z) || glm::length(v.position) > 2)
                throw std::runtime_error("Invalid vegetation vertex");
    }
    a.cards = readTexture(dir / "cards.dds");
    return a;
}
VegetationFixture makeVegetationFixture(VegetationAssets assets, uint32_t side, bool rolling, uint32_t seed) {
    if (!side || side > 256 || side % 4)
        throw std::invalid_argument("Meadow side must be a multiple of four in [4,256]");
    VegetationFixture f;
    f.assets = std::move(assets);
    f.side = side;
    f.rolling = rolling;
    f.seed = seed;
    f.instances.reserve(side * side * 8);
    for (uint32_t cz = 0; cz < side / 4; ++cz)
        for (uint32_t cx = 0; cx < side / 4; ++cx)
            for (uint32_t k = 0; k < 128; ++k) {
                uint32_t cell = cz * (side / 4) + cx, id = cell * 128 + k;
                float x = cx * 4.f - side * .5f + 4 * random(id * 7 + seed),
                      z = cz * 4.f - side * .5f + 4 * random(id * 7 + seed + 1);
                f.instances.push_back(
                    {{x, vegetationGroundHeight(x, z, rolling), z, .3f + .3f * random(id * 7 + seed + 2)},
                     random(id * 7 + seed + 3) * 6.2831853f,
                     random(id * 7 + seed + 4),
                     id,
                     cell});
            }
    auto &m = f.ground;
    for (uint32_t z = 0; z <= side; ++z)
        for (uint32_t x = 0; x <= side; ++x) {
            float px = float(x) - side * .5f, pz = float(z) - side * .5f;
            float dx = (field(px + .01f, pz, rolling) - field(px - .01f, pz, rolling)) / .02f;
            float dz = (field(px, pz + .01f, rolling) - field(px, pz - .01f, rolling)) / .02f;
            m.vertices.push_back({{px, field(px, pz, rolling), pz},
                                  glm::normalize(glm::vec3(-dx, 1, -dz)),
                                  {glm::normalize(glm::vec3(1, dx, 0)), 1},
                                  {px, pz}});
        }
    for (uint32_t z = 0; z < side; ++z)
        for (uint32_t x = 0; x < side; ++x) {
            uint32_t a = z * (side + 1) + x, b = a + 1, c = a + side + 1, d = c + 1;
            m.indices.insert(m.indices.end(), {a, d, b, a, c, d});
        }
    return f;
}
void validate(const VegetationSettings &s) {
    for (float v : {s.density, s.cardWidth, s.cardDensity, s.time, s.wind, s.distance, s.shadowDistance})
        if (!std::isfinite(v))
            throw std::invalid_argument("Nonfinite vegetation setting");
    if (!std::isfinite(s.windDirection.x) || !std::isfinite(s.windDirection.y) ||
        (!std::isfinite(glm::length(s.windDirection)) || glm::length(s.windDirection) < .001f) ||
        s.isolateAsset < -2 || s.isolateAsset >= 8 || s.forceLod < -1 || s.forceLod > 2)
        throw std::invalid_argument("Invalid vegetation wind/inspection settings");
    if (s.representation > 2 || s.density < 0 || s.density > 1 || s.cardDensity < 0 || s.cardDensity > 1 ||
        s.time < 0 || s.time > 1000000 || s.cardWidth < .5f || s.cardWidth > 2 || s.wind < 0 || s.wind > 2 ||
        s.distance < 1 || s.distance > 100 || s.shadowDistance < 0 || s.shadowDistance > 50)
        throw std::invalid_argument("Invalid vegetation setting");
}
} // namespace ow

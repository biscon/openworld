#include "tree_library.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
namespace ow {
namespace {
void word(std::ostream &f, uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i)
        b[i] = char(v >> (i * 8));
    f.write(b, 4);
}
uint32_t word(std::istream &f) {
    unsigned char b[4];
    if (!f.read(reinterpret_cast<char *>(b), 4))
        throw std::runtime_error("Truncated tree library");
    return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
void scalar(std::ostream &f, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    word(f, u);
}
float scalar(std::istream &f) {
    auto u = word(f);
    float v;
    std::memcpy(&v, &u, 4);
    return v;
}
bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
} // namespace
const char *treeSpeciesName(TreeSpecies s) {
    static const char *names[] = {"beech", "oak", "birch", "pine"};
    return uint32_t(s) < 4 ? names[uint32_t(s)] : "invalid";
}
glm::vec3 treeOctDecode(glm::vec2 uv) {
    glm::vec2 p = uv * 2.f - 1.f;
    glm::vec3 n(p.x, 1 - std::abs(p.x) - std::abs(p.y), p.y);
    if (n.y < 0) {
        n.x = (1 - std::abs(p.y)) * (p.x >= 0 ? 1 : -1);
        n.z = (1 - std::abs(p.x)) * (p.y >= 0 ? 1 : -1);
    }
    return glm::normalize(n);
}
glm::vec2 treeOctEncode(glm::vec3 n) {
    if (!finite(n) || glm::dot(n, n) < 1e-12f)
        throw std::invalid_argument("Invalid impostor direction");
    n /= std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    glm::vec2 p(n.x, n.z);
    if (n.y < 0)
        p = glm::vec2(1 - std::abs(n.z), 1 - std::abs(n.x)) * glm::vec2(n.x >= 0 ? 1 : -1, n.z >= 0 ? 1 : -1);
    return p * .5f + .5f;
}
TreeViewBlend treeViewBlend(glm::vec3 d, uint32_t n) {
    if (n < 2 || n > 32)
        throw std::invalid_argument("Invalid impostor frame count");
    auto p = treeOctEncode(d) * float(n - 1);
    auto c = glm::min(glm::uvec2(p), glm::uvec2(n - 2));
    auto f = p - glm::vec2(c);
    uint32_t a = c.y * n + c.x;
    return f.x + f.y <= 1 ? TreeViewBlend{{a, a + 1, a + n}, {1 - f.x - f.y, f.x, f.y}}
                          : TreeViewBlend{{a + n + 1, a + n, a + 1}, {f.x + f.y - 1, 1 - f.x, 1 - f.y}};
}
void validate(const TreeLibrary &l) {
    if (l.assets.empty() || l.assets.size() > treeAssetLimit)
        throw std::invalid_argument("Invalid tree asset count");
    for (const auto &a : l.assets) {
        if (uint32_t(a.species) > 3 || a.variant > 5 || !finite(a.minimum) || !finite(a.maximum) ||
            glm::any(glm::lessThanEqual(a.maximum, a.minimum)) || !std::isfinite(a.displacement) ||
            a.displacement < 0 || a.displacement > 10 || !finite(glm::vec3(a.capture)) ||
            !std::isfinite(a.capture.w) || a.capture.w <= 0 || a.frames < 2 || a.frames > 32 ||
            a.tileSize < 8 || a.tileSize > 512 || (a.tileSize & (a.tileSize - 1)))
            throw std::invalid_argument("Invalid tree metadata");
        for (const auto &lod : a.lods)
            for (const auto &p : lod) {
                if (p.vertices.empty() || p.vertices.size() > 1000000 || p.indices.empty() ||
                    p.indices.size() > 3000000 || p.indices.size() % 3)
                    throw std::invalid_argument("Invalid tree mesh size");
                for (auto i : p.indices)
                    if (i >= p.vertices.size())
                        throw std::invalid_argument("Invalid tree mesh index");
                for (const auto &v : p.vertices) {
                    if (!finite(v.position) || !finite(v.normal) || glm::dot(v.normal, v.normal) < .25f ||
                        glm::dot(v.normal, v.normal) > 2 || !finite(glm::vec3(v.tangent)) ||
                        !std::isfinite(v.tangent.w) || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y) ||
                        !finite(glm::vec3(v.deformation)) || !std::isfinite(v.deformation.w) ||
                        glm::any(glm::lessThan(v.position, a.minimum - .001f)) ||
                        glm::any(glm::greaterThan(v.position, a.maximum + .001f)) ||
                        glm::length(v.position - glm::vec3(a.capture)) > a.capture.w + .001f)
                        throw std::invalid_argument("Invalid tree vertex/bounds");
                }
            }
    }
}
void writeTreeLibrary(const std::filesystem::path &path, const TreeLibrary &l) {
    validate(l);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot write tree library");
    word(f, 0x5254574f);
    word(f, 1);
    word(f, uint32_t(l.assets.size()));
    for (const auto &a : l.assets) {
        word(f, uint32_t(a.species));
        word(f, a.variant);
        word(f, a.seed);
        word(f, a.frames);
        word(f, a.tileSize);
        for (auto v : {a.minimum, a.maximum})
            for (int i = 0; i < 3; ++i)
                scalar(f, v[i]);
        scalar(f, a.displacement);
        for (int i = 0; i < 4; ++i)
            scalar(f, a.capture[i]);
        for (const auto &lod : a.lods)
            for (const auto &p : lod) {
                word(f, uint32_t(p.vertices.size()));
                word(f, uint32_t(p.indices.size()));
                for (const auto &v : p.vertices) {
                    for (int i = 0; i < 3; ++i)
                        scalar(f, v.position[i]);
                    for (int i = 0; i < 3; ++i)
                        scalar(f, v.normal[i]);
                    for (int i = 0; i < 4; ++i)
                        scalar(f, v.tangent[i]);
                    for (int i = 0; i < 2; ++i)
                        scalar(f, v.uv[i]);
                    for (int i = 0; i < 4; ++i)
                        scalar(f, v.deformation[i]);
                }
                for (auto i : p.indices)
                    word(f, i);
            }
    }
    if (!f)
        throw std::runtime_error("Tree library write failed");
}
TreeLibrary readTreeLibrary(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f || word(f) != 0x5254574f || word(f) != 1)
        throw std::runtime_error("Unsupported tree library");
    uint32_t n = word(f);
    if (!n || n > treeAssetLimit)
        throw std::runtime_error("Invalid tree count");
    TreeLibrary l;
    l.assets.resize(n);
    uint64_t total = 0;
    for (auto &a : l.assets) {
        a.species = TreeSpecies(word(f));
        a.variant = word(f);
        a.seed = word(f);
        a.frames = word(f);
        a.tileSize = word(f);
        for (int i = 0; i < 3; ++i)
            a.minimum[i] = scalar(f);
        for (int i = 0; i < 3; ++i)
            a.maximum[i] = scalar(f);
        a.displacement = scalar(f);
        for (int i = 0; i < 4; ++i)
            a.capture[i] = scalar(f);
        for (auto &lod : a.lods)
            for (auto &p : lod) {
                auto nv = word(f), ni = word(f);
                total += uint64_t(nv) * 64 + uint64_t(ni) * 4;
                if (nv > 1000000 || ni > 3000000 || total > (256ull << 20))
                    throw std::runtime_error("Tree file exceeds limits");
                // Verify remaining file size before allocating attacker-controlled
                // payloads.
                auto start = f.tellg();
                f.seekg(0, std::ios::end);
                auto end = f.tellg();
                f.seekg(start);
                if (end - start < std::streamoff(uint64_t(nv) * 64 + uint64_t(ni) * 4))
                    throw std::runtime_error("Truncated tree mesh");
                p.vertices.resize(nv);
                p.indices.resize(ni);
                for (auto &v : p.vertices) {
                    for (int i = 0; i < 3; ++i)
                        v.position[i] = scalar(f);
                    for (int i = 0; i < 3; ++i)
                        v.normal[i] = scalar(f);
                    for (int i = 0; i < 4; ++i)
                        v.tangent[i] = scalar(f);
                    for (int i = 0; i < 2; ++i)
                        v.uv[i] = scalar(f);
                    for (int i = 0; i < 4; ++i)
                        v.deformation[i] = scalar(f);
                }
                for (auto &i : p.indices)
                    i = word(f);
            }
    }
    if (f.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Trailing tree data");
    validate(l);
    return l;
}
} // namespace ow

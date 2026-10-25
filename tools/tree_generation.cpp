#include "tree_generation.hpp"
#include <algorithm>
#include <cmath>
namespace ow {
namespace {
constexpr float pi = 3.14159265359f;
uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}
struct Random {
    uint32_t s;
    float next() {
        s = hash(s + 0x9e3779b9);
        return float(s >> 8) / 16777216.f;
    }
    glm::vec3 sphere() {
        float z = next() * 2 - 1, a = next() * 2 * pi, r = std::sqrt(1 - z * z);
        return {r * std::cos(a), z, r * std::sin(a)};
    }
};
struct Node {
    glm::vec3 p;
    uint32_t parent;
    float radius = 0;
    uint32_t children = 0;
    float support = 0;
};
struct Leaf {
    glm::vec3 p, right, up;
    float size, phase;
};
void tube(TreePart &out, glm::vec3 a, glm::vec3 b, float ra, float rb, uint32_t sides, float height,
          float phase) {
    glm::vec3 axis = glm::normalize(b - a),
              u = glm::normalize(
                  glm::cross(axis, std::abs(axis.y) < .95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0))),
              v = glm::cross(axis, u);
    uint32_t base = out.vertices.size();
    for (int row = 0; row < 2; ++row)
        for (uint32_t i = 0; i <= sides; ++i) {
            float angle = 2 * pi * i / sides;
            glm::vec3 n = u * std::cos(angle) + v * std::sin(angle);
            TreeVertex p;
            p.position = (row ? b : a) + n * (row ? rb : ra);
            p.normal = glm::normalize(n + axis * ((ra - rb) / glm::length(b - a)));
            p.tangent = glm::vec4(-u * std::sin(angle) + v * std::cos(angle), 1);
            p.uv = {float(i) / sides * std::max(1.f, ra * 2 * pi), p.position.y * .7f};
            p.deformation = {std::clamp(p.position.y / height, 0.f, 1.f),
                             std::pow(std::max(0.f, p.position.y / height), 2), 0, phase};
            out.vertices.push_back(p);
        }
    for (uint32_t i = 0; i < sides; ++i) {
        uint32_t j = base + i, k = j + sides + 1;
        out.indices.insert(out.indices.end(), {j, k, j + 1, j + 1, k, k + 1});
    }
}
void card(TreePart &out, const Leaf &l, float scale, float height) {
    uint32_t base = out.vertices.size();
    glm::vec3 normal = glm::normalize(glm::cross(l.right, l.up));
    // Four triangles around a raised centre give the twig a shallow folded
    // surface.
    const glm::vec2 uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {.5f, .5f}};
    for (int i = 0; i < 5; ++i) {
        TreeVertex v;
        v.position = l.p + (l.right * (uv[i].x - .5f) + l.up * (.5f - uv[i].y)) * l.size * scale +
                     normal * (i == 4 ? .08f * l.size : 0);
        v.normal = glm::normalize(normal * .65f + glm::vec3(0, .35f, 0));
        v.tangent = glm::vec4(l.right, 1);
        v.uv = uv[i];
        float y = std::max(0.f, v.position.y / height);
        v.deformation = {y, y * y, 1, l.phase};
        out.vertices.push_back(v);
    }
    for (uint32_t i = 0; i < 4; ++i)
        out.indices.insert(out.indices.end(), {base + i, base + (i + 1) % 4, base + 4});
}
} // namespace
TreeAsset generateTree(const TreeRecipe &r) {
    if (uint32_t(r.species) > 3 || r.variant > 5 || r.height < 2 || r.height > 60 || r.crownRadius < .5f ||
        r.crownRadius > 20 || !std::isfinite(r.height) || !std::isfinite(r.crownRadius) ||
        r.attractors < 32 || r.attractors > 4000)
        throw std::invalid_argument("Invalid tree recipe");
    Random rng{r.seed};
    bool pine = r.species == TreeSpecies::Pine, birch = r.species == TreeSpecies::Birch,
         oak = r.species == TreeSpecies::Oak;
    float h = r.height, rad = r.crownRadius,
          base = h * (r.variant < 2   ? .20f
                      : r.variant < 4 ? .32f
                                      : .17f),
          step = h / 45;
    if (pine)
        base = h * (r.variant < 2 ? .18f : r.variant < 4 ? .48f : .43f);
    std::vector<Node> nodes;
    nodes.reserve(4096);
    nodes.push_back({{0, -.35f, 0}, 0});
    float trunkRadius = h * (birch ? .013f : oak ? .027f : .021f);
    uint32_t trunk = 20;
    for (uint32_t i = 1; i <= trunk; ++i) {
        float t = float(i) / trunk;
        nodes.push_back({{std::sin(t * 3 + r.seed % 7) * t * rad *
                              (oak     ? .22f
                               : birch ? .14f
                                       : .08f),
                          t * h, std::sin(t * 4 + r.seed % 5) * t * rad * (oak ? .15f : .07f)},
                         i - 1});
    }
    // Seed structural limbs; attractor growth fills their crowns with fine
    // branches.
    for (uint32_t i = 6; i < trunk; ++i) {
        if (nodes[i].p.y < base)
            continue;
        uint32_t arms = pine ? 4 : 2;
        for (uint32_t a = 0; a < arms; ++a) {
            float angle = i * 2.39996f + a * 2 * pi / arms + (rng.next() - .5f) * .6f;
            float t = (nodes[i].p.y - base) / (h - base);
            float reach =
                rad * (pine && r.variant < 2 ? std::pow(1 - t, .6f)
                                             : std::sqrt(std::max(0.f, 1 - std::pow((t - .35f) / .8f, 2.f))));
            glm::vec3 dir(std::cos(angle), pine ? .12f : oak ? .35f : .65f, std::sin(angle));
            dir = glm::normalize(dir);
            uint32_t parent = i;
            for (uint32_t j = 1; j <= 4; ++j) {
                auto p = nodes[i].p + dir * (reach * j / 4);
                p.y += pine ? -.1f * reach * std::sin(float(j) / 4 * pi) : 0;
                nodes.push_back(
                    {p, parent, 0, 0, trunkRadius * (pine ? .20f : .42f) * (1 - t) * (1 - float(j) / 5)});
                parent = nodes.size() - 1;
            }
        }
    }
    std::vector<glm::vec3> attractors;
    attractors.reserve(r.attractors);
    for (uint32_t i = 0; i < r.attractors; ++i) {
        auto p = rng.sphere() * std::cbrt(rng.next());
        // Correlated crown lobes break the envelope into overlapping branch masses.
        if (!pine) {
            float angle = float(i % 7) * 2.39996f + float(r.seed % 31);
            glm::vec3 lobe(std::cos(angle) * .40f, .25f * std::sin(angle * 2), std::sin(angle) * .40f);
            p = p * .66f + lobe;
        }
        float y = (p.y + 1) * .5f;
        p.x *= rad * (pine && r.variant < 2 ? std::pow(1 - y, .5f) : 1);
        p.z *= rad * (pine && r.variant < 2 ? std::pow(1 - y, .5f) : 1);
        p.y = base + y * (h - base);
        attractors.push_back(p);
    }
    std::vector<glm::vec3> sums(4096);
    std::vector<uint32_t> counts(4096);
    for (uint32_t iteration = 0; iteration < 48 && !attractors.empty() && nodes.size() < 3900; ++iteration) {
        std::fill(sums.begin(), sums.end(), glm::vec3(0));
        std::fill(counts.begin(), counts.end(), 0);
        size_t kept = 0;
        for (auto p : attractors) {
            float best = rad * rad * .6f;
            uint32_t closest = ~0u;
            for (uint32_t j = 1; j < nodes.size(); ++j) {
                auto d = p - nodes[j].p;
                float q = glm::dot(d, d);
                if (q < best) {
                    best = q;
                    closest = j;
                }
            }
            if (best < step * step * 1.3f)
                continue;
            attractors[kept++] = p;
            if (closest != ~0u) {
                sums[closest] += glm::normalize(p - nodes[closest].p);
                ++counts[closest];
            }
        }
        attractors.resize(kept);
        uint32_t n = nodes.size();
        for (uint32_t j = 1; j < n && nodes.size() < 4096; ++j)
            if (counts[j]) {
                auto dir = glm::normalize(sums[j] / float(counts[j]) + glm::vec3(0, birch ? -.08f : .08f, 0));
                nodes.push_back({nodes[j].p + dir * step, j});
            }
        if (nodes.size() == n)
            break;
    }
    for (size_t i = nodes.size(); i-- > 1;) {
        auto &n = nodes[i];
        n.radius = std::sqrt(n.radius + .00009f);
        auto &p = nodes[n.parent];
        p.radius += n.radius * n.radius;
        ++p.children;
    }
    nodes[0].radius = std::sqrt(nodes[0].radius);
    for (auto &n : nodes)
        n.radius = std::max(n.radius, n.support);
    for (uint32_t i = 0; i <= trunk; ++i)
        nodes[i].radius = std::max(nodes[i].radius, trunkRadius * std::pow(1 - float(i) / (trunk + 1), .8f) *
                                                        (i == 0 ? 1.65f : 1));
    std::vector<Leaf> leaves;
    for (uint32_t i = trunk + 1; i < nodes.size(); ++i)
        if (nodes[i].children <= 1 && nodes[i].p.y > base) {
            uint32_t amount = nodes[i].children ? 2 : pine ? 8 : 5;
            for (uint32_t j = 0; j < amount; ++j) {
                auto normal = glm::normalize(rng.sphere() + glm::vec3(0, .5f, 0));
                auto right = glm::normalize(
                    glm::cross(normal, std::abs(normal.y) < .95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                auto up = glm::cross(normal, right);
                leaves.push_back({nodes[i].p + rng.sphere() * step * .8f, right, up,
                                  (pine    ? 1.15f
                                   : birch ? .80f
                                           : .85f) *
                                      (.75f + rng.next() * .5f),
                                  rng.next()});
            }
        }
    TreeAsset a;
    a.species = r.species;
    a.variant = r.variant;
    a.seed = r.seed;
    for (uint32_t lod = 0; lod < 3; ++lod) {
        auto &wood = a.lods[lod][0];
        auto &leaf = a.lods[lod][1];
        for (uint32_t i = 1; i < nodes.size(); ++i) {
            auto &n = nodes[i];
            if (i > trunk && n.radius < (lod == 0 ? .008f : lod == 1 ? .035f : .10f))
                continue;
            auto &p = nodes[n.parent];
            uint32_t sides = i <= trunk ? (lod == 0 ? 12 : lod == 1 ? 7 : 5) : (lod == 0 ? 5 : 3);
            tube(wood, p.p, n.p, std::min(p.radius, n.radius * 1.4f), n.radius, sides, h,
                 float(hash(i + r.seed) >> 8) / 16777216.f);
        }
        uint32_t count = std::min<uint32_t>(6000, leaves.size()) / (lod == 0 ? 1 : lod == 1 ? 5 : 24);
        for (uint32_t i = 0; i < count; ++i)
            card(leaf, leaves[uint64_t(i) * leaves.size() / count], lod == 0 ? 1 : lod == 1 ? 1.9f : 3.8f, h);
    }
    a.minimum = glm::vec3(INFINITY);
    a.maximum = glm::vec3(-INFINITY);
    for (const auto &lod : a.lods)
        for (const auto &p : lod)
            for (const auto &v : p.vertices) {
                a.minimum = glm::min(a.minimum, v.position);
                a.maximum = glm::max(a.maximum, v.position);
            }
    auto center = (a.minimum + a.maximum) * .5f;
    float extent = glm::length(a.maximum - a.minimum) * .5f * 1.08f;
    a.capture = glm::vec4(center, extent);
    return a;
}
} // namespace ow

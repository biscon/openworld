#include "vegetation_library.hpp"
#include "vegetation.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
namespace ow {
namespace {
uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
float random(uint32_t x) { return (hash(x) >> 8) * (1.f / 16777216); }
void leaf(FoliagePart &p, glm::vec3 root, glm::vec3 direction, glm::vec3 right, float length, float width,
          uint32_t segments, float phase, bool masked, float height, bool stem = false) {
    uint32_t first = uint32_t(p.vertices.size());
    for (uint32_t j = 0; j <= segments; ++j) {
        float t = float(j) / segments;
        glm::vec3 centre = root + direction * (length * t) + glm::vec3(0, -.16f * length * t * t, 0);
        glm::vec3 along = direction * length + glm::vec3(0, -.32f * length * t, 0);
        glm::vec3 n = glm::normalize(glm::cross(right, along));
        float w = width * (masked ? 1.f : std::max(.015f, 1 - t));
        for (uint32_t e = 0; e < 2; ++e) {
            auto pos = centre + right * w * (e ? 1.f : -1.f);
            float h = std::clamp(pos.y / height, 0.f, 1.f);
            p.vertices.push_back({pos,
                                  n,
                                  {right, masked ? -1.f : 1.f},
                                  {float(e), masked ? 1 - t : t},
                                  {h, h * h, stem ? 0.f : h * h, phase}});
        }
    }
    for (uint32_t j = 0; j < segments; ++j) {
        auto a = first + j * 2;
        p.indices.insert(p.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
}
// Static correspondence is baked once; no instance-specific data is needed.
void buildGrassMorph(FoliageAsset &a) {
    auto area = [](const FoliagePart &p) {
        float sum = 0;
        for (size_t i = 0; i < p.indices.size(); i += 3) {
            auto x = p.vertices[p.indices[i]].position;
            auto y = p.vertices[p.indices[i + 1]].position;
            auto z = p.vertices[p.indices[i + 2]].position;
            sum += glm::length(glm::cross(y - x, z - x)) * .5f;
        }
        return sum;
    };
    const float fullArea = area(a.lods[0][0]);
    for (uint32_t lod = 1; lod < 3; ++lod) {
        auto &p = a.lods[lod][0];
        const float width = std::min(6.f, fullArea / area(p));
        for (size_t i = 0; i < p.vertices.size(); i += 2) {
            auto centre = (p.vertices[i].position + p.vertices[i + 1].position) * .5f;
            for (size_t e = 0; e < 2; ++e)
                p.vertices[i + e].position = centre + (p.vertices[i + e].position - centre) * width;
        }
    }
    for (uint32_t lod = 0; lod < 3; ++lod) {
        auto &p = a.lods[lod][0];
        const uint32_t stride = lod == 0 ? 8 : 4, next = std::min(2u, lod + 1);
        const auto &target = a.lods[next][0];
        p.grassMorph.resize(p.vertices.size());
        for (size_t i = 0; i < p.vertices.size(); ++i) {
            auto &m = p.grassMorph[i];
            const auto &v = p.vertices[i];
            const uint32_t blade = uint32_t(i) / stride;
            auto centre = (p.vertices[i & ~size_t(1)].position + p.vertices[i | 1].position) * .5f;
            m.centreBlade = {centre, float(blade)};
            m.nextCentreWidth = {centre, 0};
            m.nextNormal = {v.normal, 0};
            m.nextDeformation = v.deformation;
            if (blade >= a.grass.counts[next])
                continue;
            const auto &root = target.vertices[blade * 4 + i % 2];
            const auto &tip = target.vertices[blade * 4 + 2 + i % 2];
            auto targetCentre = glm::mix(
                (target.vertices[blade * 4].position + target.vertices[blade * 4 + 1].position) * .5f,
                (target.vertices[blade * 4 + 2].position + target.vertices[blade * 4 + 3].position) * .5f,
                v.uv.y);
            const auto targetPosition = glm::mix(root.position, tip.position, v.uv.y);
            m.nextCentreWidth = {targetCentre, glm::length(targetPosition - targetCentre) /
                                                   glm::length(v.position - centre)};
            m.nextNormal = {glm::mix(root.normal, tip.normal, v.uv.y), 0};
            m.nextDeformation = glm::mix(root.deformation, tip.deformation, v.uv.y);
        }
    }
    // A morph is a convex interpolation of endpoints, including the widened meshes.
    a.minimum = glm::vec3(INFINITY);
    a.maximum = glm::vec3(-INFINITY);
    for (const auto &lod : a.lods)
        for (const auto &p : lod)
            for (const auto &v : p.vertices) {
                a.minimum = glm::min(a.minimum, v.position);
                a.maximum = glm::max(a.maximum, v.position);
            }
}
struct Wire {
    std::fstream stream;
    bool writing;
    Wire(const std::filesystem::path &p, bool w)
        : stream(p, std::ios::binary | (w ? (std::ios::out | std::ios::trunc) : std::ios::in)), writing(w) {
        if (!stream)
            throw std::runtime_error("Cannot open vegetation library");
    }
    uint32_t u(uint32_t v = 0) {
        unsigned char b[4]{};
        if (writing) {
            for (int i = 0; i < 4; ++i)
                b[i] = uint8_t(v >> (i * 8));
            stream.write(reinterpret_cast<char *>(b), 4);
        } else {
            stream.read(reinterpret_cast<char *>(b), 4);
            v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        }
        if (!stream)
            throw std::runtime_error("Truncated vegetation library");
        return v;
    }
    float f(float v = 0) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        bits = u(bits);
        std::memcpy(&v, &bits, 4);
        return v;
    }
    template <class V> void vec(V &v) {
        for (int i = 0; i < v.length(); ++i)
            v[i] = f(v[i]);
    }
    uint32_t count(size_t n, uint32_t max) {
        auto k = u(uint32_t(n));
        if (k > max)
            throw std::runtime_error("Vegetation library count exceeds limit");
        return k;
    }
};
void payload(Wire &w, VegetationLibrary &l) {
    if (w.u(0x3256474f) != 0x3256474f)
        throw std::runtime_error("Unsupported vegetation library");
    const auto version = w.u(4);
    if (version != 2 && version != 3 && version != 4)
        throw std::runtime_error("Unsupported vegetation library version");
    l.revision = w.u(l.revision);
    l.assets.resize(w.count(l.assets.size(), 8));
    l.materials.resize(w.count(l.materials.size(), 16));
    l.textures.resize(w.count(l.textures.size(), 4));
    for (auto &m : l.materials) {
        w.vec(m.colour);
        m.roughness = w.f(m.roughness);
        m.cutoff = w.f(m.cutoff);
        m.transmission = w.f(m.transmission);
        m.colourTexture = int32_t(w.u(uint32_t(m.colourTexture)));
        m.normalTexture = int32_t(w.u(uint32_t(m.normalTexture)));
        m.masked = w.u(m.masked) != 0;
    }
    for (auto &a : l.assets) {
        a.category = FoliageCategory(w.u(uint32_t(a.category)));
        w.vec(a.minimum);
        w.vec(a.maximum);
        w.vec(a.ranges);
        a.stiffness = w.f(a.stiffness);
        a.displacement = w.f(a.displacement);
        if (version >= 3) {
            for (int k = 0; k < 4; ++k)
                a.grass.counts[k] = w.u(a.grass.counts[k]);
            w.vec(a.grass.bands);
            w.vec(a.grass.shading);
        }
        if (version >= 4) {
            a.species = FoliageSpecies(w.u(uint32_t(a.species)));
            w.vec(a.habitat);
            w.vec(a.placement);
            w.vec(a.interaction);
        } else {
            a.species = a.category == FoliageCategory::Grass     ? FoliageSpecies::Grass
                        : a.category == FoliageCategory::Shrub   ? FoliageSpecies::Shrub
                        : a.category == FoliageCategory::Clutter ? FoliageSpecies::Clutter
                                                                 : FoliageSpecies::Fern;
            a.habitat = a.species == FoliageSpecies::Grass  ? glm::vec4(1, .65f, .35f, 0)
                        : a.species == FoliageSpecies::Fern ? glm::vec4(.15f, .25f, 1, 0)
                                                            : glm::vec4(.15f, .75f, .4f, 0);
            a.placement = a.species == FoliageSpecies::Grass  ? glm::vec4(.25f, .02f, .8f, 1.2f)
                          : a.species == FoliageSpecies::Fern ? glm::vec4(.45f, .02f, 1, 1)
                                                              : glm::vec4(.6f, .05f, 1, 1);
            a.interaction = {};
        }
        for (auto &lod : a.lods) {
            lod.resize(w.count(lod.size(), 2));
            for (auto &p : lod) {
                p.material = w.u(p.material);
                p.vertices.resize(w.count(p.vertices.size(), 65536));
                p.indices.resize(w.count(p.indices.size(), 196608));
                for (auto &v : p.vertices) {
                    w.vec(v.position);
                    w.vec(v.normal);
                    w.vec(v.tangent);
                    w.vec(v.uv);
                    w.vec(v.deformation);
                }
                for (auto &i : p.indices)
                    i = w.u(i);
                if (version >= 3) {
                    p.grassMorph.resize(w.count(p.grassMorph.size(), 65536));
                    for (auto &m : p.grassMorph) {
                        w.vec(m.centreBlade);
                        w.vec(m.nextCentreWidth);
                        w.vec(m.nextNormal);
                        w.vec(m.nextDeformation);
                    }
                }
            }
        }
    }
}
void require(bool b, const char *s) {
    if (!b)
        throw std::invalid_argument(s);
}
} // namespace
VegetationLibrary buildVegetationLibrary(const VegetationRecipe &r) {
    require(r.blades >= 8 && r.blades <= 64 && std::isfinite(r.width) && r.width >= .005f &&
                r.width <= .06f && std::isfinite(r.spread) && r.spread >= 0 && r.spread <= .5f &&
                std::isfinite(r.height) && r.height >= .2f && r.height <= 1.5f,
            "Invalid vegetation recipe");
    VegetationLibrary l;
    l.materials = {{{.18f, .32f, .055f, 1}, .8f, .5f, .28f},
                   {{.12f, .25f, .035f, 1}, .85f, .5f, .28f},
                   {{1, 1, 1, 1}, .8f, .5f, .32f, 0, 1, true},
                   {{.14f, .075f, .025f, 1}, .9f, .5f, 0},
                   {{1, 1, 1, 1}, .75f, .5f, .25f, 2, 3, true}};
    // Owned default textures support tests without production artwork.
    for (uint32_t map = 0; map < 4; ++map) {
        TextureData t;
        t.width = t.height = 16;
        t.levels = 5;
        t.format = map % 2 ? TextureFormat::Rgba8 : TextureFormat::Srgb8;
        for (uint32_t n = 16; n; n >>= 1)
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n; ++x) {
                    bool inside = std::abs((x + .5f) / n - .5f) < .38f;
                    if (map % 2)
                        t.bytes.insert(t.bytes.end(), {128, 128, 255, 255});
                    else
                        t.bytes.insert(t.bytes.end(), {110, 155, 55, uint8_t(inside ? 255 : 0)});
                }
        l.textures.push_back(std::move(t));
    }
    l.materials.push_back({{.16f, .28f, .055f, 1}, .85f, .5f, .1f});
    l.materials.push_back({{1, 1, 1, 1}, .8f, .5f, .18f, 0, 1, true});
    l.materials.push_back({{1, 1, 1, 1}, .86f, .5f, .2f, 2, 3, true});
    for (uint32_t asset = 0; asset < 6; ++asset) {
        FoliageAsset a;
        a.category = asset < 2    ? FoliageCategory::Grass
                     : asset == 3 ? FoliageCategory::Shrub
                                  : FoliageCategory::Plant;
        if (asset == 5)
            a.category = FoliageCategory::Shrub;
        a.species = asset < 2    ? FoliageSpecies::Grass
                    : asset == 2 ? FoliageSpecies::Fern
                    : asset == 3 ? FoliageSpecies::Shrub
                    : asset == 4 ? FoliageSpecies::Daisy
                                 : FoliageSpecies::Heather;
        a.habitat = asset < 2    ? glm::vec4(1, .65f, .35f, 0)
                    : asset == 2 ? glm::vec4(.15f, .2f, 1, .65f)
                    : asset == 3 ? glm::vec4(.1f, .3f, .6f, .65f)
                    : asset == 4 ? glm::vec4(.8f, .1f, .05f, .75f)
                                 : glm::vec4(.05f, 1, .15f, .7f);
        a.placement = asset < 2 ? glm::vec4(.25f, .02f, .8f, 1.2f)
                                : glm::vec4(asset == 2 ? .45f : .6f, asset == 2 ? .02f : .05f, .8f, 1.2f);
        a.interaction = asset < 2    ? glm::vec4(.45f, .18f, 0, 0)
                        : asset == 2 ? glm::vec4(.35f, .18f, 0, 0)
                        : asset == 3 ? glm::vec4(.12f, .05f, 0, 0)
                        : asset == 4 ? glm::vec4(.4f, .16f, 0, 0)
                                     : glm::vec4(.18f, .08f, 0, 0);
        a.stiffness = asset == 3 ? .45f : asset == 2 ? .7f : 1;
        if (asset == 5)
            a.stiffness = .5f;
        a.displacement = .46f;
        a.ranges = asset == 3   ? glm::vec4(25, 50, 90, 50)
                   : asset == 2 ? glm::vec4(15, 35, 70, 25)
                                : glm::vec4(20, 45, 70, 0);
        if (asset == 4)
            a.ranges = {15, 35, 70, 15};
        if (asset == 5)
            a.ranges = {20, 45, 90, 40};
        if (asset < 2)
            a.grass.counts = {r.blades, (r.blades + 2) / 3, (r.blades + 5) / 6, 0};
        a.minimum = glm::vec3(INFINITY);
        a.maximum = glm::vec3(-INFINITY);
        for (uint32_t lod = 0; lod < 3; ++lod) {
            auto &parts = a.lods[lod];
            parts.resize(asset >= 3 ? 2 : 1);
            parts[0].material = asset == 3 ? 3 : asset;
            if (asset == 3)
                parts[1].material = 4;
            if (asset >= 4) {
                parts[0].material = asset == 4 ? 5 : 3;
                parts[1].material = asset == 4 ? 6 : 7;
            }
            uint32_t segments = asset < 2 ? (lod == 0 ? 3 : 1) : 4 - lod;
            if (asset < 2)
                for (uint32_t b = 0; b < a.grass.counts[lod]; ++b) {
                    uint32_t id = b * 13 + asset * 177;
                    float angle = random(id) * 6.2831853f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    glm::vec3 root = right * (r.spread * std::sqrt(random(id + 2)));
                    auto direction = glm::normalize(glm::vec3(-right.z * .4f, 1, right.x * .4f));
                    leaf(parts[0], root, direction, right, r.height * (.7f + .3f * random(id + 1)), r.width,
                         segments, random(id + 3), false, r.height);
                }
            if (asset == 2)
                for (uint32_t b = 0; b < (8u >> lod); ++b) {
                    float angle = b * 2.39996323f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    auto direction = glm::normalize(glm::vec3(-right.z * .85f, 1, right.x * .85f));
                    // Unequal arch lengths and a small crown prevent a radial fan of identical fronds.
                    glm::vec3 crown(-right.z*.035f,0,right.x*.035f);
                    leaf(parts[0], crown, direction, right, .65f+.25f*random(b+811),
                         .14f+.05f*random(b+977), segments, float(b)*.13f, true, .85f);
                }
            if (asset == 3)
                for (uint32_t b = 0; b < 5; ++b) {
                    float angle = b * 2.39996323f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    glm::vec3 direction = glm::normalize(glm::vec3(-right.z * .5f, 1, right.x * .5f));
                    float branchHeight=.72f+.28f*random(b+129);
                    leaf(parts[0], {0, 0, 0}, direction, right, branchHeight, .012f, segments, float(b)*.17f, false, 1, true);
                    for (uint32_t j = 0; j < (6u >> lod); ++j) {
                        float t = .22f + j * .12f;
                        auto root = direction * (branchHeight * t) + glm::vec3(0, -.16f * branchHeight * t * t, 0);
                        auto leafDir = glm::normalize(right * (j % 2 ? 1.f : -1.f) + glm::vec3(0, .45f, 0));
                        auto leafRight = glm::normalize(glm::cross(glm::vec3(0, 1, 0), leafDir));
                        leaf(parts[1], root, leafDir, leafRight, .19f+.07f*random(b*7+j+101), .08f, segments, float(b + j) * .17f,
                             true, 1);
                    }
                }
            if (asset == 4) {
                // Several individually curved stems and shallow, shaped flower heads.
                for (uint32_t b = 0; b < (lod == 0 ? 7u : lod == 1 ? 5u : 3u); ++b) {
                    float angle = b * 2.39996323f, h = .40f + .22f * random(b + 331);
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    glm::vec3 root = right * (.08f + .19f * random(b + 57));
                    auto dir = glm::normalize(glm::vec3(-right.z * .16f, 1, right.x * .16f));
                    leaf(parts[0], root, dir, right, h, .006f, segments, float(b) * .11f, false, .65f, true);
                    leaf(parts[0], root + dir * h * .32f, glm::normalize(right + glm::vec3(0, .4f, 0)),
                         glm::vec3(-right.z, 0, right.x), .13f, .018f, 2, float(b) * .1f, false, .65f);
                    glm::vec3 centre = root + dir * h + glm::vec3(0, -.16f * h, 0);
                    uint32_t first = uint32_t(parts[1].vertices.size()), n = lod == 2 ? 8 : 16;
                    float wh = std::clamp(centre.y / .65f, 0.f, 1.f), radius = .055f + .015f * random(b + 87);
                    glm::vec3 headNormal=glm::normalize(glm::vec3(-right.z*.25f,1,right.x*.25f));
                    glm::vec3 headAcross=glm::normalize(glm::cross(right,headNormal));
                    parts[1].vertices.push_back({centre,
                                                 headNormal,
                                                 {right, -1},
                                                 {.5f, .5f},
                                                 {wh, wh * wh, wh * wh, float(b) * .1f}});
                    for (uint32_t j = 0; j <= n; ++j) {
                        float a = j * 6.2831853f / n;
                        glm::vec2 uv(std::cos(a), std::sin(a));
                        auto pos = centre + (right*uv.x+headAcross*uv.y)*radius-headNormal*.008f;
                        parts[1].vertices.push_back({pos,
                                                     glm::normalize(headNormal+(right*uv.x+headAcross*uv.y)*.15f),
                                                     {right, -1},
                                                     uv * .5f + .5f,
                                                     {wh, wh * wh, wh * wh, float(b) * .1f}});
                        if (j)
                            parts[1].indices.insert(parts[1].indices.end(),
                                                    {first, first + j + 1, first + j});
                    }
                }
            }
            if (asset == 5) {
                for (uint32_t b = 0; b < (lod == 0 ? 12u : lod == 1 ? 8u : 5u); ++b) {
                    float angle = b * 2.39996323f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    float h = .35f + .2f * random(b + 553);
                    glm::vec3 root = right * (.08f + .18f * random(b + 93));
                    auto dir = glm::normalize(glm::vec3(-right.z * .4f, 1, right.x * .4f));
                    leaf(parts[0], root, dir, right, h, .007f, segments, float(b) * .13f, false, .55f, true);
                    leaf(parts[1], root, dir, right, h, .11f, segments, float(b) * .13f, true, .55f);
                }
            }
            for (auto &p : parts)
                for (auto &v : p.vertices) {
                    a.minimum = glm::min(a.minimum, v.position);
                    a.maximum = glm::max(a.maximum, v.position);
                }
        }
        if (asset < 2)
            buildGrassMorph(a);
        l.assets.push_back(std::move(a));
    }
    validate(l);
    return l;
}
void validate(const VegetationLibrary &l) {
    require(!l.assets.empty() && l.assets.size() <= 8 && !l.materials.empty() && l.materials.size() <= 16 &&
                l.textures.size() <= 4,
            "Invalid vegetation library limits");
    for (auto &t : l.textures)
        require(t.layers == 1 && t.width <= 2048 && t.height <= 2048 &&
                    t.bytes.size() == textureBytes(t, true),
                "Invalid foliage texture");
    for (auto &m : l.materials) {
        for (float f :
             {m.colour.x, m.colour.y, m.colour.z, m.colour.w, m.roughness, m.cutoff, m.transmission})
            require(std::isfinite(f) && f >= 0 && f <= 1, "Invalid foliage material");
        require(m.roughness >= .045f && m.cutoff > 0 && m.cutoff < 1, "Invalid foliage cutoff/roughness");
        if (m.colourTexture >= 0 && size_t(m.colourTexture) < l.textures.size()) {
            auto f = l.textures[m.colourTexture].format;
            require(f == TextureFormat::Srgb8 || f == TextureFormat::Bc7Srgb, "Foliage colour must be sRGB");
        }
        if (m.normalTexture >= 0 && size_t(m.normalTexture) < l.textures.size()) {
            auto f = l.textures[m.normalTexture].format;
            require(f == TextureFormat::Rgba8 || f == TextureFormat::Bc5, "Foliage normal must be linear");
        }
        for (int i : {m.colourTexture, m.normalTexture})
            require(i >= -1 && (i < 0 || size_t(i) < l.textures.size()), "Invalid foliage texture reference");
    }
    for (auto &a : l.assets) {
        require(uint32_t(a.species) <= 5, "Invalid foliage species");
        for (int k = 0; k < 4; ++k) {
            require(std::isfinite(a.habitat[k]) && a.habitat[k] >= 0 && a.habitat[k] <= 1,
                    "Invalid species habitat");
            require(std::isfinite(a.placement[k]) && a.placement[k] > 0 && a.placement[k] <= 2,
                    "Invalid species placement");
            require(std::isfinite(a.interaction[k]) && a.interaction[k] >= 0 && a.interaction[k] <= .5f,
                    "Invalid interaction envelope");
        }
        require(a.placement.z <= a.placement.w && a.interaction.z == 0 && a.interaction.w == 0,
                "Invalid species metadata");
        require(uint32_t(a.category) <= 3 && (a.category != FoliageCategory::Grass || a.ranges.w == 0),
                "Invalid foliage category/casting policy");
        require(std::isfinite(a.stiffness) && a.stiffness >= 0 && a.stiffness <= 1 &&
                    std::isfinite(a.displacement) && a.displacement >= .45f && a.displacement <= 2,
                "Invalid deformation envelope");
        for (int i = 0; i < 3; ++i)
            require(std::isfinite(a.minimum[i]) && std::isfinite(a.maximum[i]) &&
                        a.minimum[i] <= a.maximum[i] && std::abs(a.minimum[i]) <= 8 &&
                        std::abs(a.maximum[i]) <= 8,
                    "Invalid asset bounds");
        for (int i = 0; i < 4; ++i)
            require(std::isfinite(a.ranges[i]) && a.ranges[i] >= 0 && a.ranges[i] <= 100,
                    "Invalid foliage ranges");
        require(a.ranges.x > 0 && a.ranges.y > a.ranges.x && a.ranges.z > a.ranges.y,
                "Invalid foliage LOD order");
        if (a.grass.counts.x) {
            require(a.category == FoliageCategory::Grass && a.grass.counts.x <= 64 &&
                        a.grass.counts.x > a.grass.counts.y && a.grass.counts.y > a.grass.counts.z &&
                        a.grass.counts.z > 0 && a.grass.counts.w == 0,
                    "Invalid grass counts");
            for (int k = 0; k < 4; ++k)
                require(std::isfinite(a.grass.bands[k]) && a.grass.bands[k] >= 17 &&
                            a.grass.bands[k] <= 100 && (k == 0 || a.grass.bands[k] > a.grass.bands[k - 1]),
                        "Invalid grass bands");
            require(std::isfinite(a.grass.shading.x) && std::isfinite(a.grass.shading.y) &&
                        a.grass.shading.x >= 15 && a.grass.shading.y > a.grass.shading.x &&
                        a.grass.shading.y <= 100 && std::isfinite(a.grass.shading.z) &&
                        a.grass.shading.z >= 0 && a.grass.shading.z <= .4f &&
                        std::isfinite(a.grass.shading.w) && a.grass.shading.w >= .045f &&
                        a.grass.shading.w <= 1,
                    "Invalid grass shading");
        }
        for (auto &lod : a.lods) {
            require(!lod.empty() && lod.size() <= 2, "Invalid vegetation parts");
            for (auto &p : lod) {
                require(p.material < l.materials.size() && !p.vertices.empty() &&
                            p.vertices.size() <= 65536 && !p.indices.empty() && p.indices.size() <= 196608 &&
                            p.indices.size() % 3 == 0,
                        "Invalid foliage mesh");
                require(a.grass.counts.x ? p.grassMorph.size() == p.vertices.size() : p.grassMorph.empty(),
                        "Invalid grass morph count");
                if (a.grass.counts.x) {
                    require(lod.size() == 1 && !l.materials[p.material].masked,
                            "Grass morph requires opaque blades");
                    const size_t level = size_t(&lod - a.lods.data());
                    const uint32_t stride = level == 0 ? 8 : 4;
                    require(p.vertices.size() == a.grass.counts[level] * stride, "Invalid grass topology");
                    for (size_t i = 0; i < p.grassMorph.size(); ++i) {
                        const auto &m = p.grassMorph[i];
                        for (int k = 0; k < 4; ++k)
                            require(std::isfinite(m.centreBlade[k]) && std::isfinite(m.nextCentreWidth[k]) &&
                                        std::isfinite(m.nextNormal[k]) &&
                                        std::isfinite(m.nextDeformation[k]) && m.nextDeformation[k] >= 0 &&
                                        m.nextDeformation[k] <= (k == 3 ? 8 : 1),
                                    "Invalid grass morph value");
                        require(m.centreBlade.w == float(i / stride) && m.nextCentreWidth.w >= 0 &&
                                    m.nextCentreWidth.w <= 6.01f &&
                                    glm::length(glm::vec3(m.nextNormal)) > .001f,
                                "Invalid grass correspondence");
                        auto target =
                            glm::vec3(m.nextCentreWidth) +
                            (p.vertices[i].position - glm::vec3(m.centreBlade)) * m.nextCentreWidth.w;
                        for (int k = 0; k < 3; ++k)
                            require(m.centreBlade[k] >= a.minimum[k] - .0001f &&
                                        m.centreBlade[k] <= a.maximum[k] + .0001f &&
                                        m.nextCentreWidth[k] >= a.minimum[k] - .0001f &&
                                        m.nextCentreWidth[k] <= a.maximum[k] + .0001f &&
                                        target[k] >= a.minimum[k] - .0001f &&
                                        target[k] <= a.maximum[k] + .0001f,
                                    "Grass morph escapes bounds");
                    }
                }
                for (auto i : p.indices)
                    require(i < p.vertices.size(), "Invalid foliage index");
                for (auto &v : p.vertices) {
                    for (int k = 0; k < 3; ++k)
                        require(std::isfinite(v.position[k]) && v.position[k] >= a.minimum[k] - .0001f &&
                                    v.position[k] <= a.maximum[k] + .0001f && std::isfinite(v.normal[k]),
                                "Invalid foliage vertex/bounds");
                    for (int k = 0; k < 4; ++k)
                        require(std::isfinite(v.tangent[k]) && std::isfinite(v.deformation[k]) &&
                                    v.deformation[k] >= 0 && v.deformation[k] <= 8,
                                "Invalid foliage deformation");
                    require(v.deformation.x <= 1 && v.deformation.y <= 1 && v.deformation.z <= 1 &&
                                std::isfinite(v.uv.x) && std::isfinite(v.uv.y),
                            "Invalid foliage weights/UV");
                    float normalLength = glm::length(v.normal),
                          tangentLength = glm::length(glm::vec3(v.tangent));
                    require(std::isfinite(normalLength) && normalLength > .001f &&
                                std::isfinite(tangentLength) && tangentLength > .001f &&
                                std::abs(std::abs(v.tangent.w) - 1) < .001f &&
                                glm::length(glm::cross(v.normal / normalLength,
                                                       glm::vec3(v.tangent) / tangentLength)) > .001f,
                            "Degenerate foliage shading frame");
                    require(v.position.y > 1e-5f || (v.deformation.y == 0 && v.deformation.z == 0),
                            "Unanchored foliage root");
                }
            }
        }
    }
}
void validate(const VegetationPopulation &p, const VegetationLibrary &l) {
    require(p.cells.size() <= vegetationCellLimit && p.instances.size() <= vegetationCellLimit * 128,
            "Vegetation population exceeds limits");
    size_t first = 0;
    std::set<uint32_t> identities;
    for (uint32_t c = 0; c < p.cells.size(); ++c) {
        auto &cell = p.cells[c];
        validate(cell.origin);
        require(cell.first == first && cell.count <= p.instances.size() - first,
                "Invalid vegetation cell range");
        first += cell.count;
        for (size_t i = cell.first; i < first; ++i) {
            auto &v = p.instances[i];
            require((v.assetCell >> 3) == c && (v.assetCell & 7) < l.assets.size(),
                    "Invalid vegetation instance asset/cell");
            for (int k = 0; k < 4; ++k)
                require(std::isfinite(v.rootScale[k]), "Nonfinite vegetation instance");
            require(v.rootScale.w > 0 && v.rootScale.w <= 4 && glm::length(glm::vec3(v.rootScale)) <= 64 &&
                        std::isfinite(v.yaw) && std::isfinite(v.variation) && v.variation >= 0 &&
                        v.variation <= 1,
                    "Invalid vegetation instance");
            require(identities.insert(v.identity).second, "Duplicate vegetation identity");
        }
    }
    require(first == p.instances.size(), "Unowned vegetation instances");
}
void writeVegetationLibrary(const std::filesystem::path &dir, const VegetationLibrary &l,
                            const std::string &suffix) {
    validate(l);
    std::filesystem::create_directories(dir);
    auto copy = l;
    Wire w(dir / ("library" + suffix + ".bin"), true);
    payload(w, copy);
    for (uint32_t i = 0; i < l.textures.size(); ++i)
        writeTexture(dir / ("texture" + std::to_string(i) + suffix + ".dds"), l.textures[i], true);
}
VegetationLibrary readVegetationLibrary(const std::filesystem::path &dir, bool rgba) {
    std::string suffix = rgba ? "_rgba" : "";
    Wire w(dir / ("library" + suffix + ".bin"), false);
    VegetationLibrary l;
    payload(w, l);
    if (w.stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Trailing vegetation library data");
    for (uint32_t i = 0; i < l.textures.size(); ++i)
        l.textures[i] = readTexture(dir / ("texture" + std::to_string(i) + suffix + ".dds"), true);
    validate(l);
    return l;
}
uint32_t packVegetationAsset(VegetationAssetId asset, uint32_t cell) {
    require(asset.value < vegetationAssetLimit && cell < vegetationCellLimit, "Invalid packed asset/cell");
    return cell * 8 + asset.value;
}
VegetationPopulation makeVegetationPopulation(uint32_t side, bool mixed, bool rolling, uint32_t seed) {
    require(side >= 4 && side <= 256 && side % 4 == 0, "Invalid meadow size");
    VegetationPopulation p;
    for (uint32_t z = 0; z < side / 4; ++z)
        for (uint32_t x = 0; x < side / 4; ++x) {
            uint32_t c = uint32_t(p.cells.size());
            FoliageCell cell;
            cell.origin.local = {x * 4. - side * .5, 0, z * 4. - side * .5};
            cell.first = uint32_t(p.instances.size());
            cell.count = 128;
            p.cells.push_back(cell);
            for (uint32_t k = 0; k < 128; ++k) {
                uint32_t id = c * 128 + k;
                float px = 4 * random(id * 7 + seed), pz = 4 * random(id * 7 + seed + 1);
                uint32_t asset = mixed && k < 2 ? 2 + k : id % 2;
                float h = vegetationGroundHeight(float(cell.origin.local.x) + px,
                                                 float(cell.origin.local.z) + pz, rolling);
                p.instances.push_back({{px, h, pz, asset < 2 ? .8f + .4f * random(id * 7 + seed + 2) : 1},
                                       random(id * 7 + seed + 3) * 6.2831853f,
                                       random(id * 7 + seed + 4),
                                       id,
                                       packVegetationAsset({asset}, c)});
            }
        }
    return p;
}
} // namespace ow

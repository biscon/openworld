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
float random(uint32_t x) {
    return (hash(x) >> 8) * (1.f / 16777216);
}
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
    if (w.u(0x3256474f) != 0x3256474f || w.u(2) != 2)
        throw std::runtime_error("Unsupported vegetation library");
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
    for (uint32_t asset = 0; asset < 4; ++asset) {
        FoliageAsset a;
        a.category = asset < 2    ? FoliageCategory::Grass
                     : asset == 3 ? FoliageCategory::Shrub
                                  : FoliageCategory::Plant;
        a.stiffness = asset == 3 ? .45f : asset == 2 ? .7f : 1;
        a.displacement = .46f;
        a.ranges = asset == 3   ? glm::vec4(25, 50, 90, 50)
                   : asset == 2 ? glm::vec4(15, 35, 70, 25)
                                : glm::vec4(20, 45, 70, 0);
        a.minimum = glm::vec3(INFINITY);
        a.maximum = glm::vec3(-INFINITY);
        for (uint32_t lod = 0; lod < 3; ++lod) {
            auto &parts = a.lods[lod];
            parts.resize(asset == 3 ? 2 : 1);
            parts[0].material = asset == 3 ? 3 : asset;
            if (asset == 3)
                parts[1].material = 4;
            uint32_t segments = asset < 2 ? 3 - lod : 4 - lod;
            if (asset < 2)
                for (uint32_t b = 0; b < (lod == 0   ? r.blades
                                          : lod == 1 ? r.blades / 4
                                                     : std::max(2u, r.blades / 12));
                     ++b) {
                    uint32_t id = b * 13 + asset * 177;
                    float angle = random(id) * 6.2831853f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    glm::vec3 root = right * (r.spread * std::sqrt(random(id + 2)));
                    auto direction = glm::normalize(glm::vec3(-right.z * .4f, 1, right.x * .4f));
                    leaf(parts[0], root, direction, right, r.height * (.7f + .3f * random(id + 1)),
                         r.width * (lod ? 1.15f : 1), segments, random(id + 3), false, r.height);
                }
            if (asset == 2)
                for (uint32_t b = 0; b < (8u >> lod); ++b) {
                    float angle = b * 2.39996323f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    auto direction = glm::normalize(glm::vec3(-right.z * .85f, 1, right.x * .85f));
                    leaf(parts[0], {0, 0, 0}, direction, right, .8f, .20f, segments, float(b) * .13f, true,
                         1);
                }
            if (asset == 3)
                for (uint32_t b = 0; b < 5; ++b) {
                    float angle = b * 2.39996323f;
                    glm::vec3 right(std::cos(angle), 0, std::sin(angle));
                    glm::vec3 direction = glm::normalize(glm::vec3(-right.z * .5f, 1, right.x * .5f));
                    leaf(parts[0], {0, 0, 0}, direction, right, .9f, .012f, segments, float(b) * .17f, false,
                         1, true);
                    for (uint32_t j = 0; j < (6u >> lod); ++j) {
                        float t = .22f + j * .12f;
                        auto root = direction * (.9f * t) + glm::vec3(0, -.144f * t * t, 0);
                        auto leafDir = glm::normalize(right * (j % 2 ? 1.f : -1.f) + glm::vec3(0, .45f, 0));
                        auto leafRight = glm::normalize(glm::cross(glm::vec3(0, 1, 0), leafDir));
                        leaf(parts[1], root, leafDir, leafRight, .25f, .095f, segments, float(b + j) * .17f,
                             true, 1);
                    }
                }
            for (auto &p : parts)
                for (auto &v : p.vertices) {
                    a.minimum = glm::min(a.minimum, v.position);
                    a.maximum = glm::max(a.maximum, v.position);
                }
        }
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
        require(t.layers == 1 && t.width <= 2048 && t.height <= 2048 && t.bytes.size() == textureBytes(t),
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
        for (auto &lod : a.lods) {
            require(!lod.empty() && lod.size() <= 2, "Invalid vegetation parts");
            for (auto &p : lod) {
                require(p.material < l.materials.size() && !p.vertices.empty() &&
                            p.vertices.size() <= 65536 && !p.indices.empty() && p.indices.size() <= 196608 &&
                            p.indices.size() % 3 == 0,
                        "Invalid foliage mesh");
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
                    float normalLength = glm::length(v.normal), tangentLength = glm::length(glm::vec3(v.tangent));
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
    require(p.cells.size() <= 4096 && p.instances.size() <= 524288, "Vegetation population exceeds limits");
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
        writeTexture(dir / ("texture" + std::to_string(i) + suffix + ".dds"), l.textures[i]);
}
VegetationLibrary readVegetationLibrary(const std::filesystem::path &dir, bool rgba) {
    std::string suffix = rgba ? "_rgba" : "";
    Wire w(dir / ("library" + suffix + ".bin"), false);
    VegetationLibrary l;
    payload(w, l);
    if (w.stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Trailing vegetation library data");
    for (uint32_t i = 0; i < l.textures.size(); ++i)
        l.textures[i] = readTexture(dir / ("texture" + std::to_string(i) + suffix + ".dds"));
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

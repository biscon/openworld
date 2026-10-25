#include "texture_import.hpp"
#include "tree_baker.hpp"
#include "tree_generation.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace ow;
namespace {
uint32_t sourceSize = 1024;
Image pad(Image im) {
    std::vector<uint32_t> queue;
    std::vector<uint8_t> seen(im.p.size());
    queue.reserve(im.p.size());
    for (uint32_t i = 0; i < im.p.size(); ++i)
        if (im.p[i].a > .01f) {
            queue.push_back(i);
            seen[i] = 1;
        }
    for (size_t h = 0; h < queue.size(); ++h) {
        auto i = queue[h];
        int x = i % im.size, y = i / im.size;
        for (auto d : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)}) {
            auto p = glm::ivec2(x, y) + d;
            if (p.x < 0 || p.y < 0 || p.x >= int(im.size) || p.y >= int(im.size))
                continue;
            uint32_t j = p.y * im.size + p.x;
            if (seen[j])
                continue;
            seen[j] = 1;
            im.p[j] = glm::vec4(glm::vec3(im.p[i]), im.p[j].a);
            queue.push_back(j);
        }
    }
    return im;
}
Image down(const Image &a) {
    Image b{std::max(1u, a.size / 2), {}};
    b.p.resize(b.size * b.size);
    for (uint32_t y = 0; y < b.size; ++y)
        for (uint32_t x = 0; x < b.size; ++x) {
            glm::vec3 colour(0);
            float alpha = 0;
            for (uint32_t j = 0; j < 2; ++j)
                for (uint32_t i = 0; i < 2; ++i) {
                    auto v = a.p[(y * 2 + j) * a.size + x * 2 + i];
                    colour += glm::vec3(v) * v.a;
                    alpha += v.a;
                }
            b.p[y * b.size + x] = glm::vec4(alpha > 1e-6 ? colour / alpha : glm::vec3(0), alpha * .25f);
        }
    return pad(std::move(b));
}
void coverage(Image &im, float target) {
    float lo = 0, hi = 16, best = 1, error = 1;
    for (int k = 0; k < 20; ++k) {
        float s = (lo + hi) * .5f;
        size_t n = 0;
        for (auto p : im.p)
            n += p.a * s >= .4f;
        float f = float(n) / im.p.size();
        if (std::abs(f - target) < error) {
            error = std::abs(f - target);
            best = s;
        }
        if (f < target)
            lo = s;
        else
            hi = s;
    }
    for (auto &p : im.p)
        p.a = std::clamp(p.a * best, 0.f, 1.f);
}
TextureData sourceMap(const std::filesystem::path &p, bool masked, bool compressed) {
    Image im = pad(resize(load(p, true), sourceSize));
    TextureData t;
    t.width = t.height = im.size;
    t.levels = 0;
    t.format = compressed ? TextureFormat::Bc7Srgb : TextureFormat::Srgb8;
    float target = 0;
    for (auto v : im.p)
        target += v.a >= .4f;
    target /= im.p.size();
    for (;;) {
        Image encoded = im;
        if (masked)
            coverage(encoded, target);
        append(t, encoded, 0, compressed);
        ++t.levels;
        if (im.size == 1)
            break;
        im = down(im);
    }
    return t;
}
float linear(uint8_t b) {
    float v = b / 255.f;
    return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
}
void finishBake(TreeBake &b, uint32_t frames, uint32_t tile, bool compressed) {
    // Each tile owns its mip filters/padding. No filtering ever crosses capture
    // views.
    TextureData colour, normal, depth;
    colour.width = normal.width = depth.width = b.colour.width;
    colour.height = normal.height = depth.height = b.colour.height;
    colour.levels = normal.levels = depth.levels = 0;
    colour.format = compressed ? TextureFormat::Bc7Srgb : TextureFormat::Srgb8;
    normal.format = compressed ? TextureFormat::Bc7 : TextureFormat::Rgba8;
    depth.format = TextureFormat::R16;
    std::vector<Image> cs, ns;
    std::vector<std::vector<float>> ds;
    std::vector<float> targets;
    for (uint32_t z = 0; z < frames; ++z)
        for (uint32_t x = 0; x < frames; ++x) {
            Image c{tile, {}}, n{tile, {}};
            c.p.resize(tile * tile);
            n.p.resize(tile * tile);
            std::vector<float> d(tile * tile);
            float target = 0;
            for (uint32_t y = 0; y < tile; ++y)
                for (uint32_t u = 0; u < tile; ++u) {
                    uint32_t s = (z * tile + y) * b.colour.width + x * tile + u, i = y * tile + u;
                    c.p[i] = {linear(b.colour.bytes[s * 4]), linear(b.colour.bytes[s * 4 + 1]),
                              linear(b.colour.bytes[s * 4 + 2]), b.colour.bytes[s * 4 + 3] / 255.f};
                    for (int k = 0; k < 4; ++k)
                        n.p[i][k] = b.normal.bytes[s * 4 + k] / 255.f;
                    d[i] = (uint32_t(b.depth.bytes[s * 2]) + (uint32_t(b.depth.bytes[s * 2 + 1]) << 8)) /
                           65535.f;
                    target += c.p[i].a >= .4f;
                }
            // Propagate valid depth/normal into transparent border for stable
            // parallax lookup.
            std::vector<uint32_t> q;
            std::vector<bool> seen(tile * tile);
            for (uint32_t i = 0; i < c.p.size(); ++i)
                if (c.p[i].a > .01f) {
                    q.push_back(i);
                    seen[i] = true;
                }
            for (size_t h = 0; h < q.size(); ++h) {
                uint32_t i = q[h];
                for (auto off : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)}) {
                    auto p = glm::ivec2(i % tile, i / tile) + off;
                    if (p.x < 0 || p.y < 0 || p.x >= int(tile) || p.y >= int(tile))
                        continue;
                    uint32_t j = p.y * tile + p.x;
                    if (seen[j])
                        continue;
                    seen[j] = true;
                    n.p[j] = n.p[i];
                    d[j] = d[i];
                    q.push_back(j);
                }
            }
            cs.push_back(pad(std::move(c)));
            ns.push_back(std::move(n));
            ds.push_back(std::move(d));
            targets.push_back(target / (tile * tile));
        }
    for (uint32_t size = tile;; size /= 2) {
        Image ca{size * frames, {}}, na{size * frames, {}};
        ca.p.resize(ca.size * ca.size);
        na.p.resize(ca.p.size());
        size_t offset = depth.bytes.size();
        depth.bytes.resize(offset + ca.p.size() * 2);
        for (uint32_t z = 0; z < frames; ++z)
            for (uint32_t x = 0; x < frames; ++x) {
                uint32_t k = z * frames + x;
                Image c = cs[k];
                coverage(c, targets[k]);
                for (uint32_t y = 0; y < size; ++y)
                    for (uint32_t u = 0; u < size; ++u) {
                        uint32_t src = y * size + u, dst = (z * size + y) * ca.size + x * size + u;
                        ca.p[dst] = c.p[src];
                        na.p[dst] = ns[k].p[src];
                        uint32_t d = std::lround(std::clamp(ds[k][src], 0.f, 1.f) * 65535);
                        depth.bytes[offset + dst * 2] = d & 255;
                        depth.bytes[offset + dst * 2 + 1] = d >> 8;
                    }
                if (size > 1) {
                    Image nn{size / 2, {}};
                    nn.p.resize(nn.size * nn.size);
                    std::vector<float> dd(nn.p.size());
                    for (uint32_t y = 0; y < nn.size; ++y)
                        for (uint32_t u = 0; u < nn.size; ++u) {
                            glm::vec3 n(0);
                            float ao = 0, leaf = 0, d = 0, weight = 0;
                            for (uint32_t j = 0; j < 2; ++j)
                                for (uint32_t i = 0; i < 2; ++i) {
                                    uint32_t id = (y * 2 + j) * size + u * 2 + i;
                                    float w = std::max(.001f, cs[k].p[id].a);
                                    auto v = ns[k].p[id];
                                    n += treeOctDecode(glm::vec2(v)) * w;
                                    ao += v.z * w;
                                    leaf += v.w * w;
                                    d += ds[k][id] * w;
                                    weight += w;
                                }
                            auto oct =
                                treeOctEncode(glm::length(n) > 1e-6 ? glm::normalize(n) : glm::vec3(0, 1, 0));
                            nn.p[y * nn.size + u] = {oct, ao / weight, leaf / weight};
                            dd[y * nn.size + u] = d / weight;
                        }
                    cs[k] = down(cs[k]);
                    ns[k] = std::move(nn);
                    ds[k] = std::move(dd);
                }
            }
        append(colour, ca, 0, compressed);
        append(normal, na, 2, compressed);
        ++colour.levels;
        ++normal.levels;
        ++depth.levels;
        if (size == 1)
            break;
    }
    b = {std::move(colour), std::move(normal), std::move(depth)};
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 4)
            throw std::runtime_error("Usage: tree_import recipe.json output-directory shader-directory "
                                     "[--validation]");
        auto recipe = std::filesystem::path(argv[1]), out = std::filesystem::path(argv[2]);
        std::ifstream in(recipe);
        nlohmann::json j;
        in >> j;
        std::filesystem::create_directories(out);
        bc7enc_compress_block_init();
        TreeLibrary library;
        TreeBaker baker(argv[3], argc > 4);
        bool compressed = j.value("compressed", true);
        sourceSize = j.value("texture_size", 1024u);
        if (sourceSize < 8 || sourceSize > 2048 || (sourceSize & (sourceSize - 1)))
            throw std::invalid_argument("Invalid tree source texture size");
        if (j.at("trees").empty() || j.at("trees").size() > 24)
            throw std::invalid_argument("Invalid recipe tree count");
        std::array<TextureData, 4> bark, leaf;
        for (uint32_t species = 0; species < 4; ++species) {
            auto name = std::string(treeSpeciesName(TreeSpecies(species)));
            bark[species] = sourceMap(recipe.parent_path() / (name + "_bark.png"), false, false);
            leaf[species] = sourceMap(recipe.parent_path() / (name + ".png"), true, false);
            writeTexture(out / (name + "_bark_rgba.dds"), bark[species]);
            writeTexture(out / (name + "_leaf_rgba.dds"), leaf[species]);
            writeTexture(out / (name + "_bark.dds"),
                         compressed ? sourceMap(recipe.parent_path() / (name + "_bark.png"), false, true)
                                    : bark[species]);
            writeTexture(out / (name + "_leaf.dds"),
                         compressed ? sourceMap(recipe.parent_path() / (name + ".png"), true, true)
                                    : leaf[species]);
        }
        for (auto &entry : j.at("trees")) {
            TreeRecipe r;
            r.species = TreeSpecies(entry.at("species").get<uint32_t>());
            r.variant = entry.at("variant");
            r.seed = entry.at("seed");
            r.height = entry.at("height");
            r.crownRadius = entry.at("radius");
            r.attractors = j.value("attractors", 800);
            auto a = generateTree(r);
            a.frames = j.value("frames", 12);
            a.tileSize = j.value("tile_size", 128);
            validate(TreeLibrary{{a}});
            auto id = library.assets.size();
            std::cout << "Baking " << treeSpeciesName(a.species) << " " << a.variant << " ("
                      << a.lods[0][0].indices.size() / 3 + a.lods[0][1].indices.size() / 3 << " triangles)"
                      << std::endl;
            auto b = baker.bake(a, bark[uint32_t(a.species)], leaf[uint32_t(a.species)]);
            auto raw = b;
            finishBake(raw, a.frames, a.tileSize, false);
            // Half-resolution RGBA atlases keep fallback memory bounded.
            for (auto *t : {&raw.colour, &raw.normal})
                if (a.tileSize > 32) {
                    auto bytes = textureMipBytes(t->format, t->width, t->height);
                    t->bytes.erase(t->bytes.begin(), t->bytes.begin() + bytes);
                    t->width /= 2;
                    t->height /= 2;
                    --t->levels;
                }
            finishBake(b, a.frames, a.tileSize, compressed);
            auto prefix = out / ("tree" + std::to_string(id));
            writeTexture(prefix.string() + "_colour_rgba.dds", raw.colour, true);
            writeTexture(prefix.string() + "_normal_rgba.dds", raw.normal, true);
            writeTexture(prefix.string() + "_colour.dds", b.colour, true);
            writeTexture(prefix.string() + "_normal.dds", b.normal, true);
            writeTexture(prefix.string() + "_depth.dds", b.depth, true);
            library.assets.push_back(std::move(a));
        }
        writeTreeLibrary(out / "library.bin", library);
        std::cout << "Built " << library.assets.size() << " trees" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

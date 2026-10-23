#include "bc7enc.h"
#include "material_assets.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <png.h>
#include <squish.h>
using namespace ow;
struct Image {
    uint32_t size;
    std::vector<glm::vec4> p;
};
Image load(const std::filesystem::path &p, bool srgb) {
    png_image im{};
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, p.c_str()))
        throw std::runtime_error("PNG read: " + p.string());
    if (im.width != im.height || im.width < 4 || im.width > 4096) {
        png_image_free(&im);
        throw std::runtime_error("Source must be square, 4..4096");
    }
    im.format = PNG_FORMAT_RGBA;
    std::vector<uint8_t> b(PNG_IMAGE_SIZE(im));
    if (!png_image_finish_read(&im, nullptr, b.data(), 0, nullptr)) {
        png_image_free(&im);
        throw std::runtime_error("PNG decode");
    }
    Image out{im.width, {}};
    png_image_free(&im);
    out.p.resize(out.size * out.size);
    for (size_t i = 0; i < out.p.size(); ++i)
        for (int c = 0; c < 4; ++c) {
            float x = b[i * 4 + c] / 255.f;
            out.p[i][c] =
                srgb && c < 3 ? (x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f)) : x;
        }
    return out;
}
Image resize(const Image &a, uint32_t size) {
    Image b{size, std::vector<glm::vec4>(size * size)};
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            float u = (x + .5f) * a.size / size - .5f, v = (y + .5f) * a.size / size - .5f;
            int ix = int(std::floor(u)), iy = int(std::floor(v));
            float fx = u - ix, fy = v - iy;
            auto at = [&](int px, int py) {
                return a.p[((py + int(a.size)) % int(a.size)) * a.size + (px + int(a.size)) % int(a.size)];
            };
            b.p[y * size + x] = glm::mix(glm::mix(at(ix, iy), at(ix + 1, iy), fx),
                                         glm::mix(at(ix, iy + 1), at(ix + 1, iy + 1), fx), fy);
        }
    return b;
}
uint8_t byte(float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255)); }
void append(TextureData &out, const Image &im, int semantic, bool bc) {
    std::vector<uint8_t> raw(im.p.size() * 4);
    for (size_t i = 0; i < im.p.size(); ++i) {
        auto v = im.p[i];
        if (semantic == 0)
            for (int c = 0; c < 3; ++c)
                v[c] = v[c] <= .0031308f ? v[c] * 12.92f : 1.055f * std::pow(v[c], 1 / 2.4f) - .055f;
        if (semantic == 1)
            v = glm::vec4(glm::normalize(glm::vec3(v)) * .5f + .5f, 1);
        for (int c = 0; c < 4; ++c)
            raw[i * 4 + c] = byte(v[c]);
    }
    if (!bc) {
        out.bytes.insert(out.bytes.end(), raw.begin(), raw.end());
        return;
    }
    size_t offset = out.bytes.size();
    out.bytes.resize(offset + textureMipBytes(out.format, im.size, im.size));
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    params.m_max_partitions = 16;
    if (semantic != 0)
        bc7enc_compress_block_params_init_linear_weights(&params);
    for (uint32_t y = 0; y < im.size; y += 4)
        for (uint32_t x = 0; x < im.size; x += 4) {
            uint8_t block[64];
            for (uint32_t j = 0; j < 4; ++j)
                for (uint32_t i = 0; i < 4; ++i)
                    for (int c = 0; c < 4; ++c)
                        block[(j * 4 + i) * 4 + c] =
                            raw[((std::min(y + j, im.size - 1)) * im.size + std::min(x + i, im.size - 1)) *
                                    4 +
                                c];
            if (semantic == 1)
                squish::Compress(block, out.bytes.data() + offset, squish::kBc5);
            else
                bc7enc_compress_block(out.bytes.data() + offset, block, &params);
            offset += 16;
        }
}
// Macro artwork supplies broad colour only. Roughness is an independently
// seeded, periodic authored field; it is deliberately not inferred from colour
// brightness.
void importMacro(const nlohmann::json &recipe, const std::filesystem::path &root,
                 const std::filesystem::path &dir) {
    const auto &materials = recipe.at("materials");
    size_t present = 0;
    for (const auto &r : materials)
        present += r.contains("macroColour");
    if (!present) {
        std::filesystem::remove(dir / "macro.dds");
        std::filesystem::remove(dir / "macro_rgba.dds");
        return;
    }
    if (present != 4)
        throw std::runtime_error("Macro palette requires all four layers");
    const uint32_t size = recipe.value("macroSize", 512u);
    if (size < 4 || size > 512 || (size & (size - 1)))
        throw std::runtime_error("Invalid macro size");
    std::array<TextureData, 2> outputs;
    for (int variant = 0; variant < 2; ++variant) {
        auto &t = outputs[variant];
        t.width = t.height = size;
        t.layers = 4;
        t.levels = 1;
        for (auto n = size; n > 1; n >>= 1)
            ++t.levels;
        t.format = variant == 0 ? TextureFormat::Bc7Srgb : TextureFormat::Srgb8;
    }
    for (const auto &r : materials) {
        Image source = load(root / r.at("macroColour").get<std::string>(), true);
        // Area filtering removes generated grain before enlarging to the runtime
        // resolution. The final wrap-bilinear reconstruction gives periodic edges.
        constexpr uint32_t coarse = 32;
        Image low{coarse, std::vector<glm::vec4>(coarse * coarse)};
        for (uint32_t y = 0; y < coarse; ++y)
            for (uint32_t x = 0; x < coarse; ++x) {
                glm::vec4 sum(0);
                uint32_t count = 0;
                for (uint32_t sy = y * source.size / coarse;
                     sy < std::max(y * source.size / coarse + 1, (y + 1) * source.size / coarse); ++sy)
                    for (uint32_t sx = x * source.size / coarse;
                         sx < std::max(x * source.size / coarse + 1, (x + 1) * source.size / coarse); ++sx) {
                        sum += source.p[std::min(sy, source.size - 1) * source.size +
                                        std::min(sx, source.size - 1)];
                        ++count;
                    }
                low.p[y * coarse + x] = sum / float(count);
            }
        glm::vec3 mean(0), variance(0);
        for (auto v : low.p)
            mean += glm::vec3(v);
        mean /= float(low.p.size());
        for (auto v : low.p) {
            auto d = glm::vec3(v) - mean;
            variance += d * d;
        }
        const auto sigma = glm::max(glm::sqrt(variance / float(low.p.size())), glm::vec3(.03f));
        const float amplitude = r.value("macroRoughnessAmplitude", .35f);
        const uint32_t seed = r.value("macroRoughnessSeed", 1u);
        if (!std::isfinite(amplitude) || amplitude < 0 || amplitude > .5f)
            throw std::runtime_error("Invalid macro roughness amplitude");
        auto random = [&](uint32_t x, uint32_t y) {
            uint32_t h = (x & 7u) + (y & 7u) * 8u + seed * 747796405u;
            h = (h ^ (h >> 16)) * 0x7feb352du;
            h = (h ^ (h >> 15)) * 0x846ca68bu;
            h ^= h >> 16;
            return float(h >> 8) / 16777216.f * 2 - 1;
        };
        for (uint32_t y = 0; y < coarse; ++y)
            for (uint32_t x = 0; x < coarse; ++x) {
                auto &v = low.p[y * coarse + x];
                glm::vec3 rgb =
                    glm::clamp(.5f + .2f * (glm::vec3(v) - mean) / sigma, glm::vec3(0), glm::vec3(1));
                float fx = (x % 4) / 4.f, fy = (y % 4) / 4.f;
                fx = fx * fx * (3 - 2 * fx);
                fy = fy * fy * (3 - 2 * fy);
                const float rough =
                    glm::mix(glm::mix(random(x / 4, y / 4), random(x / 4 + 1, y / 4), fx),
                             glm::mix(random(x / 4, y / 4 + 1), random(x / 4 + 1, y / 4 + 1), fx), fy);
                v = glm::vec4(rgb, .5f + amplitude * rough);
            }
        Image im = resize(low, size);
        for (;;) {
            append(outputs[0], im, 0, true);
            append(outputs[1], im, 0, false);
            if (im.size == 1)
                break;
            Image next{im.size / 2, std::vector<glm::vec4>(im.size * im.size / 4)};
            for (uint32_t y = 0; y < next.size; ++y)
                for (uint32_t x = 0; x < next.size; ++x)
                    next.p[y * next.size + x] =
                        (im.p[(2 * y) * im.size + 2 * x] + im.p[(2 * y) * im.size + 2 * x + 1] +
                         im.p[(2 * y + 1) * im.size + 2 * x] + im.p[(2 * y + 1) * im.size + 2 * x + 1]) *
                        .25f;
            im = std::move(next);
        }
    }
    writeTexture(dir / "macro.dds", outputs[0]);
    writeTexture(dir / "macro_rgba.dds", outputs[1]);
}
int main(int argc, char **argv) {
    try {
        if (argc != 3 && !(argc == 4 && std::string(argv[3]) == "--macro-only"))
            throw std::runtime_error("Usage: material_import recipe.json output-directory [--macro-only]");
        const std::filesystem::path recipePath = argv[1], dir = argv[2];
        std::ifstream in(recipePath);
        nlohmann::json recipe;
        in >> recipe;
        const auto &materials = recipe.at("materials");
        if (materials.size() != 4)
            throw std::runtime_error("Expected grass, soil, rock, gravel");
        std::filesystem::create_directories(dir);
        bc7enc_compress_block_init();
        std::array<TextureData, 6> outputs;
        if (argc == 3) {
            for (size_t layer = 0; layer < 4; ++layer) {
                const auto &r = materials[layer];
                const auto root = recipePath.parent_path();
                Image colour = load(root / r.at("colour").get<std::string>(), true),
                      height = load(root / r.at("height").get<std::string>(), false);
                if (colour.size != height.size)
                    throw std::runtime_error("Unregistered map dimensions");
                const uint32_t target = recipe.value("size", 2048u);
                if (target < 4 || target > 4096 || (target & (target - 1)))
                    throw std::runtime_error("Invalid import size");
                colour = resize(colour, target);
                height = resize(height, target);
                const uint32_t n = colour.size;
                Image normal{n, std::vector<glm::vec4>(n * n)}, scalar = height;
                float repeat = r.at("repeatMetres"), range = r.at("heightMetres"), rough = r.at("roughness");
                if (!(repeat == 4 && range >= 0 && range <= repeat && rough >= .045 && rough <= 1))
                    throw std::runtime_error("Invalid physical material scale");
                auto h = [&](int x, int y) {
                    return height.p[(y + int(n)) % int(n) * n + (x + int(n)) % int(n)].r;
                };
                for (uint32_t y = 0; y < n; ++y)
                    for (uint32_t x = 0; x < n; ++x) {
                        float dx = (h(x + 1, y) - h(x - 1, y)) * range * n / (2 * repeat),
                              dy = (h(x, y + 1) - h(x, y - 1)) * range * n / (2 * repeat);
                        normal.p[y * n + x] = glm::vec4(glm::normalize(glm::vec3(-dx, -dy, 1)), 1);
                        scalar.p[y * n + x] = {rough, 1, h(x, y), 1};
                    }
                uint32_t levels = 1;
                for (auto s = n; s > 1; s >>= 1)
                    ++levels;
                for (int i = 0; i < 6; ++i) {
                    auto &o = outputs[i];
                    if (layer && o.width != n)
                        throw std::runtime_error("Palette dimensions differ");
                    o.width = o.height = n;
                    o.layers = 4;
                    o.levels = levels;
                    const TextureFormat formats[] = {TextureFormat::Bc7Srgb, TextureFormat::Bc5,
                                                     TextureFormat::Bc7,     TextureFormat::Srgb8,
                                                     TextureFormat::Rgba8,   TextureFormat::Rgba8};
                    o.format = formats[i];
                }
                for (uint32_t mip = 0; mip < levels; ++mip) {
                    for (int variant = 0; variant < 2; ++variant) {
                        append(outputs[variant * 3], colour, 0, variant == 0);
                        append(outputs[variant * 3 + 1], normal, 1, variant == 0);
                        append(outputs[variant * 3 + 2], scalar, 2, variant == 0);
                    }
                    if (colour.size == 1)
                        break;
                    uint32_t s = colour.size / 2;
                    Image c{s, std::vector<glm::vec4>(s * s)}, norm = c, sc = c;
                    for (uint32_t y = 0; y < s; ++y)
                        for (uint32_t x = 0; x < s; ++x) {
                            glm::vec4 a(0), b(0), d(0);

                            for (int j = 0; j < 2; ++j)
                                for (int i = 0; i < 2; ++i) {
                                    auto index = (2 * y + j) * colour.size + 2 * x + i;
                                    a += colour.p[index];
                                    b += normal.p[index];
                                    d += scalar.p[index];
                                }
                            a *= .25f;
                            b *= .25f;
                            d *= .25f;
                            d.r = std::min(
                                1.f, std::sqrt(rough * rough + std::max(0.f, 1 - glm::length(glm::vec3(b)))));
                            // Retain the unnormalized mean to carry normal variance across
                            // all mips.
                            c.p[y * s + x] = a;
                            norm.p[y * s + x] = b;
                            sc.p[y * s + x] = d;
                        }
                    colour = std::move(c);
                    normal = std::move(norm);
                    scalar = std::move(sc);
                }
                std::cout << "Imported " << r.at("name") << '\n';
            }
            const char *names[] = {"colour.dds",      "normal.dds",      "scalar.dds",
                                   "colour_rgba.dds", "normal_rgba.dds", "scalar_rgba.dds"};
            for (size_t i = 0; i < 6; ++i)
                writeTexture(dir / names[i], outputs[i]);
        }
        importMacro(recipe, recipePath.parent_path(), dir);
        std::ofstream manifest(dir / "palette.json");
        manifest << recipe.dump(2) << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

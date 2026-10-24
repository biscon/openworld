#pragma once
#include "bc7enc.h"
#include "material_assets.hpp"
#include <algorithm>
#include <cmath>
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
uint8_t byte(float v) {
    return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255));
}
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

#include "material_assets.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
namespace ow {
void applyMaterialPreset(TerrainMaterialSettings &s, TerrainMaterialPreset preset) {
    s.heightBlend = s.cliffs = s.macro = preset != TerrainMaterialPreset::Baseline;
    s.twoAxis = false;
    s.filtering = preset == TerrainMaterialPreset::High ? 2 : 1;
    s.heightInfluence = .5f;
    s.blendWidth = .25f;
    s.macroRepeatMetres = 90;
    s.macroStrength = 1;
    s.diagnostic = 0;
}
MaterialCoordinates materialCoordinates(int64_t x, int64_t z, double extent, glm::dvec3 local,
                                        double microRepeat, double macroRepeat) {
    if (!std::isfinite(extent) || extent <= 0 || !std::isfinite(local.x) || !std::isfinite(local.y) ||
        !std::isfinite(local.z) || !std::isfinite(microRepeat) || microRepeat <= 0 ||
        !std::isfinite(macroRepeat) || macroRepeat <= 0)
        throw std::invalid_argument("Invalid material coordinates");
    MaterialCoordinates out;
    const int64_t chunks[3] = {x, 0, z};
    for (int axis = 0; axis < 3; ++axis) {
        const long double origin = static_cast<long double>(chunks[axis]) * extent;
        auto phase = [&](double repeat) {
            long double p = std::fmod(std::fmod(origin, repeat) + local[axis], repeat);
            if (p < 0)
                p += repeat;
            return float(p / repeat);
        };
        out.microPhase[axis] = phase(microRepeat);
        out.macroPhase[axis] = phase(macroRepeat);
    }
    out.microPhase.w = float(1 / microRepeat);
    out.macroPhase.w = float(1 / macroRepeat);
    return out;
}
uint64_t textureMipBytes(TextureFormat f, uint32_t w, uint32_t h) {
    return f == TextureFormat::R16 ? uint64_t(w) * h * 2
           : f == TextureFormat::Rgba8 || f == TextureFormat::Srgb8
               ? uint64_t(w) * h * 4
               : uint64_t((w + 3) / 4) * ((h + 3) / 4) * 16;
}
uint64_t textureBytes(const TextureData &t, bool allowPartial) {
    if (!t.width || !t.height || t.width > 8192 || t.height > 8192 || !t.layers || t.layers > 128 ||
        !t.levels || t.levels > 14)
        throw std::runtime_error("Invalid texture dimensions");
    const auto f = t.format;
    if (f != TextureFormat::R16 && f != TextureFormat::Rgba8 && f != TextureFormat::Srgb8 &&
        f != TextureFormat::Bc5 && f != TextureFormat::Bc7 && f != TextureFormat::Bc7Srgb)
        throw std::runtime_error("Unsupported DDS format");
    uint32_t full = 1;
    for (auto n = std::max(t.width, t.height); n > 1; n >>= 1)
        ++full;
    if (t.levels > full || (!allowPartial && t.levels != full))
        throw std::runtime_error("Texture requires a complete mip chain");
    uint64_t bytes = 0;
    for (uint32_t m = 0; m < t.levels; ++m)
        bytes += textureMipBytes(f, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
    bytes *= t.layers;
    if (bytes > 320ull * 1024 * 1024)
        throw std::runtime_error("Texture exceeds material budget");
    return bytes;
}
TextureData readTexture(const std::filesystem::path &path, bool allowPartial) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Missing texture: " + path.string());
    const auto size = in.tellg();
    in.seekg(0);
    std::array<uint32_t, 37> h{};
    if (!in.read(reinterpret_cast<char *>(h.data()), 148) || h[0] != 0x20534444 || h[1] != 124 ||
        h[19] != 32 || h[21] != 0x30315844 || h[33] != 3 || h[34] != 0 || h[28] != 0)
        throw std::runtime_error("Unsupported or malformed DDS DX10: " + path.string());
    TextureData t;
    t.width = h[4];
    t.height = h[3];
    t.levels = h[7];
    t.layers = h[35];
    t.format = TextureFormat(h[32]);
    const auto bytes = textureBytes(t, allowPartial);
    if (uint64_t(size) != 148 + bytes)
        throw std::runtime_error("DDS payload size mismatch");
    t.bytes.resize(bytes);
    if (!in.read(reinterpret_cast<char *>(t.bytes.data()), bytes))
        throw std::runtime_error("Truncated DDS");
    return t;
}
void writeTexture(const std::filesystem::path &path, const TextureData &t, bool allowPartial) {
    if (t.bytes.size() != textureBytes(t, allowPartial))
        throw std::runtime_error("Invalid texture payload");
    std::array<uint32_t, 37> h{};
    h[0] = 0x20534444;
    h[1] = 124;
    const bool raw = t.format == TextureFormat::R16 || t.format == TextureFormat::Rgba8 ||
                     t.format == TextureFormat::Srgb8;
    h[2] = 0x21007 | (raw ? 0x8 : 0x80000);
    h[5] = raw ? t.width * (t.format == TextureFormat::R16 ? 2 : 4)
               : uint32_t(textureMipBytes(t.format, t.width, t.height));
    h[3] = t.height;
    h[4] = t.width;
    h[7] = t.levels;
    h[19] = 32;
    h[20] = 4;
    h[21] = 0x30315844;
    h[27] = 0x1000 | (t.layers > 1 || t.levels > 1 ? 0x8 : 0) | (t.levels > 1 ? 0x400000 : 0);
    h[32] = uint32_t(t.format);
    h[33] = 3;
    h[35] = t.layers;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char *>(h.data()), 148);
    out.write(reinterpret_cast<const char *>(t.bytes.data()), t.bytes.size());
    if (!out)
        throw std::runtime_error("Cannot write DDS: " + path.string());
}
glm::vec2 materialPhase(int64_t x, int64_t z, double extent, glm::dvec2 local, double repeat) {
    if (!std::isfinite(repeat) || repeat <= 0)
        throw std::invalid_argument("Invalid material repeat");
    const auto phase = [&](int64_t c, double p) {
        // Reduce the integer origin before adding the local sub-metre component.
        long double v = std::fmod(static_cast<long double>(c) * extent, static_cast<long double>(repeat));
        v = std::fmod(v + p, static_cast<long double>(repeat));
        if (v < 0)
            v += repeat;
        return float(v / repeat);
    };
    return {phase(x, local.x), phase(z, local.y)};
}
} // namespace ow

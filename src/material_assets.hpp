#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>
namespace ow {
enum class TextureFormat : uint32_t { R16 = 56, Rgba8 = 28, Srgb8 = 29, Bc5 = 83, Bc7 = 98, Bc7Srgb = 99 };
struct TextureData {
    uint32_t width = 1, height = 1, levels = 1, layers = 1;
    TextureFormat format = TextureFormat::Rgba8;
    std::vector<uint8_t> bytes; // DDS order: layer, mip, row/block
};
uint64_t textureMipBytes(TextureFormat, uint32_t width, uint32_t height);
// Atlas callers may opt into truncated mip tails; terrain defaults require complete chains.
uint64_t textureBytes(const TextureData &, bool allowPartial = false);
TextureData readTexture(const std::filesystem::path &, bool allowPartial = false);
void writeTexture(const std::filesystem::path &, const TextureData &, bool allowPartial = false);
struct TerrainMaterialSettings {
    bool enabled = true, weights = false;
    int isolate = -1, filtering = 1; // 0 trilinear, 1 8x, 2 16x
    float repeatMetres = 4, normalStrength = 1, roughness = 1;
    glm::vec3 tint{1};
    bool heightBlend = true, cliffs = true, macro = true, twoAxis = false;
    float heightInfluence = .5f, blendWidth = .25f, macroRepeatMetres = 90, macroStrength = 1;
    int diagnostic = 0; // shaded, adjusted weights, projections, normals, logical reads
};
enum class TerrainMaterialPreset { Baseline, Balanced, High };
void applyMaterialPreset(TerrainMaterialSettings &, TerrainMaterialPreset);
struct MaterialCoordinates {
    glm::vec4 microPhase{}, macroPhase{};
};
static_assert(sizeof(MaterialCoordinates) == 32);
MaterialCoordinates materialCoordinates(int64_t x, int64_t z, double extent, glm::dvec3 local,
                                        double microRepeat, double macroRepeat);
struct MaterialStats {
    bool bindless = false, loaded = false;
    bool macroLoaded = false;
    uint32_t maximumTextureReads = 12;
    float maxAnisotropy = 1, effectiveAnisotropy = 1;
    uint32_t image2DCapacity = 0, arrayCapacity = 0;
    uint64_t imageBytes = 0;
    std::string error;
};
struct TextureHandle {
    uint32_t slot = ~0u, generation = 0;
};
struct MaterialHandle {
    uint32_t slot = ~0u, generation = 0;
};
// Small CPU registry. A stale or exhausted handle always resolves to fallback
// slot 0.
template <class Handle, size_t N> class MaterialSlots {
    std::array<uint32_t, N> generation_{};
    std::array<bool, N> live_{};

  public:
    MaterialSlots() {
        live_[0] = true;
        generation_[0] = 1;
    }
    Handle acquire() {
        for (uint32_t i = 1; i < N; ++i)
            if (!live_[i]) {
                live_[i] = true;
                if (!++generation_[i])
                    ++generation_[i];
                return {i, generation_[i]};
            }
        return {};
    }
    uint32_t resolve(Handle h) const {
        return h.slot < N && live_[h.slot] && generation_[h.slot] == h.generation ? h.slot : 0;
    }
    void release(Handle h) {
        const auto i = resolve(h);
        if (i)
            live_[i] = false;
    }
};
// Reduce the integer origin before adding local sub-metre coordinates.
glm::vec2 materialPhase(int64_t x, int64_t z, double extent, glm::dvec2 local, double repeat);
} // namespace ow

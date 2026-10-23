#pragma once
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace ow {
struct ChunkCoord {
    int64_t x = 0, z = 0;
};
struct TerrainGrid {
    uint32_t cells = 1024;
    float spacing = 0.5f;
    double extent() const { return double(cells) * spacing; }
};
struct TerrainSettings {
    uint32_t seed = 1;
    float wavelength = 2048.0f; // Mountain landforms.
    float heightScale = 1200.0f;
    float erosionStrength = 0.22f;
    float creaseRounding = 0.3f;
    float regionWavelength = 16384.0f;
    float erosionWavelength = 1024.0f;
    float ridgeStrength = 0.50f; // Rounded hills (0) to ridged mountains (1).
    float baseDetail = 0.65f;    // Broadest local octave (0) to all four (1).
};
struct TerrainRequest {
    ChunkCoord chunk;
    TerrainSettings settings;
};
constexpr uint32_t maxTerrainChunks = 9;
struct TerrainBatchRequest {
    TerrainSettings settings;
    std::array<ChunkCoord, maxTerrainChunks> chunks{};
    uint32_t count = 1;
};
TerrainBatchRequest terrainNeighbourhood(const TerrainRequest &centre, bool single = false);
void validateBatch(const TerrainGrid &, const TerrainBatchRequest &, uint32_t capacity);

bool operator==(const TerrainRequest &a, const TerrainRequest &b);
bool operator==(const TerrainGrid &a, const TerrainGrid &b);
void validateGrid(const TerrainGrid &grid);
TerrainGrid terrainGrid(double extent, float spacing);
std::vector<uint32_t> terrainIndices(const TerrainGrid &grid);

// Explicit 16-byte rows shared with HLSL. Each octave is origin.xy / field salt / padding,
// then fractional origin.xy / frequency / padding.
struct alignas(16) NoiseDomain {
    glm::ivec4 lattice{};
    glm::vec4 fractionFrequency{};
};
struct alignas(16) TerrainParameters {
    glm::uvec4 dimensions{};              // cells, seed, rebuild indices, normal subdivisions
    glm::vec4 shape{};                    // spacing, height scale, erosion strength, crease rounding
    std::array<NoiseDomain, 13> domains{}; // 4 local, 2 elevation, 2 distribution, 5 erosion
    glm::uvec4 sampling{};                // normal halo, patch cells, normal levels, canonical
                                         // flag/tile level
    glm::uvec4 worldOrigin{};             // signed 64-bit quarter-metre coordinates, low/high
                                          // X then Z
    glm::vec4 geography{};               // erosion wavelength, ridge strength, base detail, reserved
};
struct TerrainSample {
    glm::vec4 base;
    glm::vec4 eroded;
}; // normal.xyz, height.w
// Blocking probe output; not a resident rendering or cache representation.
struct TerrainGeographySample {
    glm::vec4 base;   // world gradient X/Z, mountain weight, base height in metres
    glm::vec4 detail; // erosion peak/valley target, driving gradient X/Z, eroded height
};
TerrainParameters terrainParameters(const TerrainGrid &, const TerrainRequest &);

// Marks work complete only after successful submission, never on
// acquisition/recording.
class TerrainUpdates {
  public:
    explicit TerrainUpdates(TerrainGrid grid = {}) : grid_(grid) { terrainParameters(grid_, requested_); }
    void request(const TerrainRequest &value);
    void setGrid(TerrainGrid grid);
    void requestGrid(TerrainGrid grid, const TerrainRequest &request) {
        terrainParameters(grid, request);
        grid_ = grid;
        requested_ = request;
    }
    bool pending() const { return !submitted_ || !(grid_ == appliedGrid_) || !(requested_ == applied_); }
    const TerrainRequest &requested() const { return requested_; }
    void submitted() {
        applied_ = requested_;
        appliedGrid_ = grid_;
        submitted_ = true;
        ++count_;
    }
    uint64_t count() const { return count_; }

  private:
    TerrainGrid grid_, appliedGrid_{};
    TerrainRequest requested_{}, applied_{};
    bool submitted_ = false;
    uint64_t count_ = 0;
};
// Fixed storage and transactional batch validation; no frame-time allocations.
class TerrainBatchUpdates {
  public:
    TerrainBatchUpdates(TerrainGrid grid, uint32_t capacity);
    void request(const TerrainBatchRequest &);
    void setGrid(TerrainGrid);
    const TerrainBatchRequest &requested() const { return requested_; }
    bool pending(uint32_t slot) const { return updates_[slot].pending(); }
    void submitted(uint32_t mask);
    uint64_t count() const;

  private:
    TerrainGrid grid_;
    uint32_t capacity_;
    TerrainBatchRequest requested_;
    std::array<TerrainUpdates, maxTerrainChunks> updates_;
};
} // namespace ow

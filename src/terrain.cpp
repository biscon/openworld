#include "terrain.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace ow {
static_assert(sizeof(NoiseDomain) == 32);
static_assert(offsetof(TerrainParameters, domains) == 32);
static_assert(sizeof(TerrainParameters) == 496);
static_assert(offsetof(TerrainParameters, geography) == 480);
static_assert(sizeof(TerrainSample) == 32);
static_assert(offsetof(TerrainSample, eroded) == 16);
bool operator==(const TerrainRequest &a, const TerrainRequest &b) {
    return a.chunk.x == b.chunk.x && a.chunk.z == b.chunk.z && a.settings.seed == b.settings.seed &&
           a.settings.wavelength == b.settings.wavelength && a.settings.heightScale == b.settings.heightScale &&
           a.settings.erosionStrength == b.settings.erosionStrength &&
           a.settings.creaseRounding == b.settings.creaseRounding &&
           a.settings.regionWavelength == b.settings.regionWavelength &&
           a.settings.erosionWavelength == b.settings.erosionWavelength &&
           a.settings.ridgeStrength == b.settings.ridgeStrength && a.settings.baseDetail == b.settings.baseDetail;
}
bool operator==(const TerrainGrid &a, const TerrainGrid &b) { return a.cells == b.cells && a.spacing == b.spacing; }
TerrainGrid terrainGrid(double extent, float spacing) {
    if (!std::isfinite(extent) || !std::isfinite(spacing) || extent <= 0 || spacing <= 0)
        throw std::invalid_argument("Terrain size and spacing must be finite and positive");
    const double cells = extent / double(spacing);
    const double rounded = std::round(cells);
    const double tolerance = std::max(1e-5, 2.0 * std::numeric_limits<float>::epsilon() * std::abs(cells));
    if (rounded < 8 || rounded > 1024 || std::abs(cells - rounded) > tolerance)
        throw std::invalid_argument("Size / spacing must be a whole number of cells in 8..1024");
    TerrainGrid result{uint32_t(std::round(cells)), spacing};
    validateGrid(result);
    return result;
}
void validateGrid(const TerrainGrid &g) {
    if (g.cells < 8 || g.cells > 1024 || !std::isfinite(g.spacing) || g.spacing < 0.01f || g.spacing > 8192.0f)
        throw std::invalid_argument("Terrain grid needs 8..1024 cells and spacing 0.01..8192 metres");
}
std::vector<uint32_t> terrainIndices(const TerrainGrid &g) {
    validateGrid(g);
    std::vector<uint32_t> result;
    result.reserve(size_t(g.cells) * g.cells * 6);
    const auto stride = g.cells + 1;
    for (uint32_t z = 0; z < g.cells; ++z)
        for (uint32_t x = 0; x < g.cells; ++x) {
            const auto a = z * stride + x;
            result.insert(result.end(), {a, a + stride, a + 1, a + 1, a + stride, a + stride + 1});
        }
    return result;
}
TerrainParameters terrainParameters(const TerrainGrid &g, const TerrainRequest &r) {
    validateGrid(g);
    const auto &s = r.settings;
    if (!std::isfinite(s.wavelength) || s.wavelength < 64 || s.wavelength > 8192 || !std::isfinite(s.heightScale) ||
        s.heightScale < 0 || s.heightScale > 2000 || !std::isfinite(s.erosionStrength) || s.erosionStrength < 0 ||
        s.erosionStrength > 1 || !std::isfinite(s.creaseRounding) || s.creaseRounding < 0 || s.creaseRounding > 1 ||
        !std::isfinite(s.regionWavelength) || s.regionWavelength < 4096 || s.regionWavelength > 65536 ||
        !std::isfinite(s.erosionWavelength) || s.erosionWavelength < 64 || s.erosionWavelength > 8192 ||
        !std::isfinite(s.ridgeStrength) || s.ridgeStrength < 0 || s.ridgeStrength > 1 ||
        !std::isfinite(s.baseDetail) || s.baseDetail < 0 || s.baseDetail > 1)
        throw std::invalid_argument("Terrain settings: wavelength 64..8192, height "
                                    "0..2000, erosion/rounding 0..1, region wavelength 4096..65536, "
                                    "erosion wavelength 64..8192, ridge strength/base detail 0..1");
    TerrainParameters p;
    p.dimensions = {g.cells, s.seed, 0, 1};
    p.shape = {g.spacing, s.heightScale, s.erosionStrength, s.creaseRounding};
    p.geography = {s.erosionWavelength, s.ridgeStrength, s.baseDetail, 0};
    for (size_t i = 0; i < p.domains.size(); ++i) {
        // Use the rounded shader frequency on the CPU too, so borders use the same
        // domain.
        const float f = i < 4 ? float(1u << i) / s.wavelength :
                        i < 6 ? float(1u << (i - 4)) / (2 * s.regionWavelength) :
                        i < 8 ? float(1u << (i - 6)) / s.regionWavelength :
                                float(1u << (i - 8)) / (s.erosionWavelength * 0.15f * 0.7f);
        auto &d = p.domains[i];
        d.lattice.z = i < 4 ? 0x13579b : i < 6 ? 0x2468ac : i < 8 ? 0x3579bd : 0;
        for (int axis = 0; axis < 2; ++axis) {
            const int64_t chunk = axis ? r.chunk.z : r.chunk.x;
            const double origin = double(chunk) * g.extent() * double(f);
            const double margin = (g.extent() + 130.0 * g.spacing) * double(f) + 8.0;
            // Leave enough integer headroom for the full grid, halo, and Phacelle
            // neighbours.
            if (!std::isfinite(origin) || std::abs(origin) + margin >= 2147483000.0)
                throw std::invalid_argument("Terrain coordinate exceeds supported noise lattice range");
            const double cell = std::floor(origin);
            d.lattice[axis] = int32_t(cell);
            d.fractionFrequency[axis] = float(origin - cell);
        }
        d.fractionFrequency.z = f;
    }
    const auto split = [](int64_t v) { return glm::uvec2(uint32_t(uint64_t(v)), uint32_t(uint64_t(v) >> 32)); };
    const auto x = split(int64_t(double(r.chunk.x) * g.extent() * 4));
    const auto z = split(int64_t(double(r.chunk.z) * g.extent() * 4));
    p.worldOrigin = {x.x, x.y, z.x, z.y};
    return p;
}
void TerrainUpdates::setGrid(TerrainGrid grid) {
    terrainParameters(grid, requested_);
    grid_ = grid;
}
void TerrainUpdates::request(const TerrainRequest &r) {
    terrainParameters(grid_, r); // Failed validation must leave the pending request intact.
    requested_ = r;
}
TerrainBatchRequest terrainNeighbourhood(const TerrainRequest &centre, bool single) {
    TerrainBatchRequest result;
    result.settings = centre.settings;
    result.count = 0;
    const int radius = single ? 0 : 1;
    auto add = [](int64_t value, int delta) {
        if ((delta > 0 && value == std::numeric_limits<int64_t>::max()) ||
            (delta < 0 && value == std::numeric_limits<int64_t>::min()))
            throw std::invalid_argument("Neighbourhood coordinate overflow");
        return value + delta;
    };
    for (int z = -radius; z <= radius; ++z)
        for (int x = -radius; x <= radius; ++x)
            result.chunks[result.count++] = {add(centre.chunk.x, x), add(centre.chunk.z, z)};
    return result;
}
void validateBatch(const TerrainGrid &grid, const TerrainBatchRequest &request, uint32_t capacity) {
    if (!capacity || capacity > maxTerrainChunks || !request.count || request.count > capacity)
        throw std::invalid_argument("Terrain batch exceeds chunk capacity (1..9)");
    for (uint32_t i = 0; i < request.count; ++i) {
        terrainParameters(grid, {request.chunks[i], request.settings});
        for (uint32_t j = 0; j < i; ++j)
            if (request.chunks[i].x == request.chunks[j].x && request.chunks[i].z == request.chunks[j].z)
                throw std::invalid_argument("Duplicate chunk in terrain batch");
    }
}
TerrainBatchUpdates::TerrainBatchUpdates(TerrainGrid grid, uint32_t capacity) : grid_(grid), capacity_(capacity) {
    validateBatch(grid, requested_, capacity);
    for (auto &update : updates_)
        update.setGrid(grid);
}
void TerrainBatchUpdates::request(const TerrainBatchRequest &request) {
    validateBatch(grid_, request, capacity_);
    for (uint32_t i = 0; i < request.count; ++i)
        updates_[i].requestGrid(grid_, {request.chunks[i], request.settings});
    requested_ = request;
}
void TerrainBatchUpdates::setGrid(TerrainGrid grid) {
    validateBatch(grid, requested_, capacity_);
    // Inactive slots are brought to the current grid when next requested.
    for (uint32_t i = 0; i < requested_.count; ++i)
        updates_[i].setGrid(grid);
    grid_ = grid;
}
void TerrainBatchUpdates::submitted(uint32_t mask) {
    for (uint32_t i = 0; i < requested_.count; ++i)
        if (mask & (1u << i))
            updates_[i].submitted();
}
uint64_t TerrainBatchUpdates::count() const {
    uint64_t count = 0;
    for (const auto &update : updates_)
        count += update.count();
    return count;
}
} // namespace ow

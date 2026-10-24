#pragma once
#include "camera.hpp"
#include "visibility.hpp"
#include <array>

namespace ow {
constexpr uint32_t maxTerrainLodLevels = 16;
// Legacy range helpers retained for fixed/flat diagnostics and v1 bookmarks.
// Hierarchical ground selection uses a fixed normal range and local geometry refinement.
constexpr float terrainHierarchyFirstRange = 64.f;
constexpr float terrainLodMaximumScale = 2.f;
constexpr float terrainLodScaleSlew = 4.f;
float terrainHierarchyRange(float scale);
float terrainDetailRangeScale(float committed, bool quality, bool frozen, float seconds);
struct TerrainPatchKey {
    int64_t x{}, z{}; // World base-grid sample coordinates, independent of resident slots.
    uint32_t level{};
    bool operator==(const TerrainPatchKey &b) const { return x == b.x && z == b.z && level == b.level; }
};
struct TerrainLodReviewState {
    Camera observer;
    float qualityScale = 1, focalPixels = 0;
    bool initialized = false;
    std::vector<TerrainPatchKey> patches;
};
struct TerrainLodConfig {
    bool enabled = false; // Diagnostic callers may retain arbitrary grids.
    uint32_t patchCells = 32;
    float firstRange = 64.0f;
    uint64_t triangleBudget = 2097152;
};
struct TerrainPatchMetadata {
    glm::vec2 heights{}, error{}, parentError{}, displacement{};
};
static_assert(sizeof(TerrainPatchMetadata) == 32);
struct TerrainPatchDraw {
    uint32_t slot{}, x{}, z{}, step{1}, level{}, mask{15};
    bool exterior = false;
    uint32_t cells{}, edges{}, corners{}; // cells=0 retains the legacy masked-grid path.
};
struct TerrainRenderStats {
    uint32_t selected{}, culled{};
    uint64_t triangles{};
    std::array<uint32_t, maxTerrainLodLevels> perLevel{};
    std::array<uint64_t, maxTerrainLodLevels> trianglesPerLevel{};
    float firstRange{};
    bool boundsReady{};
    uint64_t baselineTriangles{};
    float maximumErrorPixels{}, qualityScale{1};
    uint32_t overTargetPatches{};
    bool qualityLimited{};
    uint64_t triangleBudget{};
    float maximumTransitionPixels{};
    uint32_t refined{}, budgetLimited{}, residencyLimited{}, protectedPatches{}, protectedMissing{};
};
struct TerrainIndexRange {
    uint32_t first{}, count{};
};
struct TerrainPatchTopology {
    std::vector<uint32_t> indices;
    std::array<TerrainIndexRange, 16> ranges{};
    std::array<TerrainIndexRange, 16> stitched{}; // Half-size full patches, four coarse-edge bits.
};
TerrainPatchTopology terrainPatchTopology(uint32_t cells);
void validateLodGrid(TerrainGrid grid, uint32_t patchCells);
float terrainMorph(float distance, float range);

// All storage reserved at construction. Coordinates and bounds are separate
// from generation/cache identity; select() never allocates or reads GPU data.
class TerrainLod {
  public:
    TerrainLod(TerrainGrid grid, uint32_t capacity, TerrainLodConfig config,
               uint32_t chunkCapacity = maxTerrainChunks);
    void setGrid(TerrainGrid grid);
    void setBounds(uint32_t slot, const glm::vec2 *leafBounds);
    void select(const std::array<ChunkCoord, maxTerrainChunks> &chunks, uint32_t count, const Camera &camera,
                const Camera &observer, const glm::mat4 &viewProjection, bool enabled, bool culling,
                bool ready);
    void select(const ChunkCoord *chunks, const uint32_t *slots, uint32_t count, const Camera &camera,
                const Camera &observer, const glm::mat4 &viewProjection, bool enabled, bool culling,
                bool ready, float rangeOverride = 0);
    // Append one resident tile to the shared selection. Tile coordinates are
    // expressed in base chunks; data spacing scales independently.
    void beginHierarchy(const Camera &, const Camera &, const glm::mat4 &, int radius,
                        float range = terrainHierarchyFirstRange);
    void selectTile(uint32_t slot, ChunkCoord baseOrigin, uint32_t tileLevel, uint32_t mask, bool enabled,
                    bool culling, uint32_t minimumLevel = 0);
    void setQualityMetadata(uint32_t slot, const void *levels);
    TerrainPatchMetadata metadata(uint32_t slot, uint32_t x, uint32_t z, uint32_t level) const;
    void collectCasters(bool enabled) { collectCasters_ = enabled; }
    void selectExteriorTile(uint32_t slot, ChunkCoord origin, uint32_t level);
    const std::vector<TerrainPatchDraw> &casterDraws() const { return casterDraws_; }
    const std::vector<VisibilityBounds> &casterBounds() const { return casterBounds_; }
    float projectedError(const TerrainPatchDraw &, ChunkCoord baseOrigin, uint32_t tileLevel,
                         const Camera &observer, float focalPixels, bool eroded) const;
    void qualityStats(uint64_t baseline, float error, uint32_t over, bool limited) {
        stats_.baselineTriangles = baseline;
        stats_.maximumErrorPixels = error;
        stats_.overTargetPatches = over;
        stats_.qualityLimited = limited;
        stats_.qualityScale = stats_.firstRange / config_.firstRange;
    }
    float requiredRange(uint32_t slot) const { return requiredRanges_[slot]; }
    uint64_t triangleBudget() const { return config_.triangleBudget; }
    uint32_t patchCells() const { return config_.patchCells; }
    uint32_t levels() const { return nodes_[0].level + 1; }
    uint32_t leafCount() const {
        const auto n = grid_.cells / patchCells();
        return n * n;
    }
    const std::vector<TerrainPatchDraw> &draws() const { return draws_; }
    const std::vector<VisibilityBounds> &drawBounds() const { return drawBounds_; }
    size_t drawCapacity() const { return draws_.capacity(); }
    const TerrainRenderStats &stats() const { return stats_; }
    TerrainGrid grid() const { return grid_; }

  private:
    struct Node {
        uint32_t x, z, step, level;
        std::array<uint32_t, 4> children{};
    };
    TerrainGrid grid_;
    TerrainLodConfig config_;
    uint32_t capacity_, chunkCapacity_;
    std::vector<Node> nodes_;
    std::vector<glm::vec2> bounds_, errors_;
    std::vector<TerrainPatchMetadata> metadata_;
    bool collectCasters_ = false, exterior_ = false;
    std::vector<TerrainPatchDraw> draws_, casterDraws_;
    std::vector<VisibilityBounds> casterBounds_;
    std::vector<VisibilityBounds> drawBounds_;
    TerrainRenderStats stats_{};
    std::array<glm::vec4, 6> planes_{};
    std::vector<float> requiredRanges_;
    uint32_t build(uint32_t x, uint32_t z, uint32_t step, uint32_t level);
    void visit(uint32_t node, uint32_t slot, glm::vec3 offset, glm::vec3 observer, bool lod, bool cull,
               uint32_t mask = 15);
    bool hierarchical_ = false;
    Camera hierarchyCamera_{};
    glm::vec3 hierarchyEye_{};
    glm::vec4 clip_{};
    uint32_t tileLevel_ = 0, minimumLevel_ = 0;
    float hierarchyRange_ = 64;
    void box(uint32_t node, uint32_t slot, glm::vec3 offset, glm::vec3 &lo, glm::vec3 &hi) const;
    bool visible(glm::vec3 lo, glm::vec3 hi) const;
};
} // namespace ow

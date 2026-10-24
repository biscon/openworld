#pragma once
#include "terrain_lod.hpp"
namespace ow {
// Bounded CPU partition. Keys are geographic; slots only identify immutable providers.
class TerrainGroundLod {
  public:
    TerrainGroundLod(TerrainGrid, uint32_t patchCells, uint32_t tileCapacity, uint32_t drawCapacity,
                     uint64_t triangleBudget);
    void begin(const Camera &, const Camera &observer, const glm::mat4 &, float focalPixels, bool quality,
               float target, bool freeze, bool culling, bool eroded, bool enabled);
    void tile(uint32_t slot, ChunkCoord baseOrigin, uint32_t level, uint32_t mask, uint32_t minimumLevel);
    void finish(const TerrainLod &metadata, const TerrainPatchTopology &);
    void invalidate();
    const auto &draws() const { return draws_; }
    const auto &bounds() const { return bounds_; }
    const auto &casters() const { return casters_; }
    const auto &casterBounds() const { return casterBounds_; }
    const auto &stats() const { return stats_; }
    std::vector<TerrainPatchKey> snapshot() const;
    void restore(const std::vector<TerrainPatchKey> &);
    uint64_t allocatedBytes() const;
    uint32_t capacity() const { return leafCapacity_; }

  private:
    struct Node {
        TerrainPatchKey key;
        uint32_t slot{}, x{}, z{}, level{}, minimum{}, child = ~0u;
        uint32_t mark{};
        float priority{}, transition{}, error{}, distance{};
        glm::vec3 lo{}, hi{};
        bool protectedGround{};
    };
    struct Entry {
        TerrainPatchKey key;
        uint32_t index = ~0u, generation{};
    };
    struct Table {
        std::vector<Entry> entries;
        uint32_t generation = 1, maximumLevel = 0;
        void reset();
        void put(TerrainPatchKey, uint32_t);
        uint32_t get(TerrainPatchKey) const;
    };
    TerrainGrid grid_;
    uint32_t cells_, rootLevel_, leafCapacity_, leafCount_{}, mark_ = 0;
    uint64_t budget_;
    Camera camera_, observer_;
    std::array<glm::vec4, 6> planes_{};
    float focal_{}, target_{}, projectionScale_{};
    bool quality_{}, freeze_{}, culling_{}, eroded_{}, enabled_{}, restored_{};
    Table current_, previous_;
    std::vector<Node> nodes_;
    std::vector<uint32_t> heap_, closure_;
    std::vector<TerrainPatchKey> previousKeys_;
    std::vector<TerrainPatchDraw> draws_, casters_;
    std::vector<VisibilityBounds> bounds_, casterBounds_;
    TerrainRenderStats stats_{};
    static int64_t side(uint32_t cells, uint32_t level);
    uint32_t covering(const Table &, int64_t x, int64_t z) const;
    void measure(uint32_t, const TerrainLod &);
    void enqueue(uint32_t);
    bool gather(uint32_t);
    bool split(uint32_t, const TerrainLod &);
    void neighbours(const Node &, std::array<uint32_t, 8> &) const;
    bool visible(const Node &) const;
};
} // namespace ow

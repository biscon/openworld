#pragma once
#include "camera.hpp"
#include "material_assets.hpp"
#include <algorithm>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <vector>
namespace ow {
struct WorldPosition {
    int64_t x = 0, z = 0;
    glm::dvec3 local{0};
};
WorldPosition worldPosition(const Camera &, const TerrainGrid &);
WorldPosition translated(WorldPosition, glm::dvec3);
glm::dvec3 relativePosition(const WorldPosition &, const WorldPosition &);
void validate(const WorldPosition &);
struct LightHandle {
    uint32_t slot = ~0u, generation = 0;
};
struct MeshHandle {
    uint32_t slot = ~0u, generation = 0;
};
struct InstanceHandle {
    uint32_t slot = ~0u, generation = 0;
};
enum class LightType : uint32_t { Point, Spot };
enum class LocalLightMode : uint32_t { Disabled, BruteForce, Clustered };
struct LocalLight {
    WorldPosition position;
    glm::vec3 colour{1}, direction{0, -1, 0};
    float intensity = 1000, range = 20, sourceRadius = .05f;
    float innerAngle = .35f, outerAngle = .6f;
    LightType type = LightType::Point;
    float volumetricScattering = 1;
    bool volumetricHistory = true;
};
void validate(const LocalLight &);
float lightAttenuation(const LocalLight &, glm::vec3 lightToSurface);
struct MeshVertex {
    glm::vec3 position{0}, normal{0, 1, 0};
    glm::vec4 tangent{1, 0, 0, 1};
    glm::vec2 uv{0};
};
static_assert(sizeof(MeshVertex) == 48);
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};
enum class AlphaMode : uint32_t { Opaque, Mask, Blend };
struct MeshMaterial {
    glm::vec4 baseColour{1};
    glm::vec3 emissive{0};
    float roughness = .8f, metallic = 0, ao = 1, normalStrength = 1;
    TextureHandle colourTexture, normalTexture, ormTexture, emissiveTexture;
    AlphaMode alpha = AlphaMode::Opaque;
    float cutoff = .5f;
    bool doubleSided = false, unlit = false;
    bool thinGeometry = false; // Coverage-aware TAA for opaque/masked blades, leaves and needles.
};
void validate(const MeshMaterial &);
struct MeshInstance {
    MeshHandle mesh;
    MaterialHandle material;
    WorldPosition position;
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1};
};
void validate(const MeshInstance &);
struct SceneCapacity {
    uint32_t lights = 4096, instances = 4096, meshes = 256;
    uint32_t clusterTilePixels = 32, clusterSlices = 24, clusterIndices = 64;
    uint64_t geometryBytes = 64ull << 20, clusterBytesPerFrame = 64ull << 20;
};
struct ClusterGrid {
    uint32_t x = 1, y = 1, z = 24, tile = 32, indices = 64;
    uint32_t count() const { return x * y * z; }
    uint64_t bytes() const { return uint64_t(count()) * (40 + uint64_t(indices) * 4); }
};
ClusterGrid clusterGrid(uint32_t width, uint32_t height, const SceneCapacity &);
uint32_t depthSlice(float depth, uint32_t slices);
struct SceneStats {
    uint32_t activeLights = 0, instances = 0, skippedMeshes = 0;
    uint32_t occupiedClusters = 0, maximumOccupancy = 0, overflowClusters = 0;
    double meanOccupancy = 0;
    ClusterGrid grid;
    uint64_t geometryBytes = 0, clusterBytes = 0;
    double assignmentMilliseconds = 0, opaqueMilliseconds = 0, transparentMilliseconds = 0;
    bool timingsAvailable = false;
};
// Fixed allocation at construction; swap removal keeps iteration contiguous.
// Slots are CPU identities, independent of dense GPU record indices.
template <class T, class H> class ScenePool {
    struct Slot {
        uint32_t generation = 0, dense = ~0u;
    };
    std::vector<Slot> slots_;
    std::vector<T> data_;
    std::vector<uint32_t> owners_;

  public:
    explicit ScenePool(uint32_t capacity) : slots_(capacity) {
        data_.reserve(capacity);
        owners_.reserve(capacity);
    }
    H create(const T &value) {
        for (uint32_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].dense == ~0u) {
                auto &s = slots_[i];
                if (!++s.generation)
                    ++s.generation;
                s.dense = uint32_t(data_.size());
                data_.push_back(value);
                owners_.push_back(i);
                return {i, s.generation};
            }
        return {};
    }
    T *get(H h) { return valid(h) ? &data_[slots_[h.slot].dense] : nullptr; }
    const T *get(H h) const { return valid(h) ? &data_[slots_[h.slot].dense] : nullptr; }
    bool valid(H h) const {
        return h.slot < slots_.size() && slots_[h.slot].dense != ~0u &&
               slots_[h.slot].generation == h.generation;
    }
    bool update(H h, const T &value) {
        if (auto *p = get(h)) {
            *p = value;
            return true;
        }
        return false;
    }
    bool remove(H h) {
        if (!valid(h))
            return false;
        const auto d = slots_[h.slot].dense;
        if (d + 1 < data_.size()) {
            data_[d] = std::move(data_.back());
            owners_[d] = owners_.back();
            slots_[owners_[d]].dense = d;
        }
        data_.pop_back();
        owners_.pop_back();
        slots_[h.slot].dense = ~0u;
        return true;
    }
    const std::vector<T> &values() const { return data_; }
    H handle(uint32_t dense) const {
        auto s = owners_.at(dense);
        return {s, slots_[s].generation};
    }
};
} // namespace ow

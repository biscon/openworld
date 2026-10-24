#pragma once
#include "horizon.hpp"
#include "terrain.hpp"
namespace ow {
struct CameraInput {
    glm::vec3 movement{}; // right, world up, forward
    glm::vec2 look{};     // mouse pixels
    bool boost = false;
};
struct Camera {
    static constexpr float nearPlane = .5f, farPlane = 64000.f, verticalFovDegrees = 60.f;
    float renderFar = farPlane;
    glm::vec4 curvature{0};
    ChunkCoord chunk;
    glm::dvec3 local{};
    float yaw = 0, pitch = 0;
    float speed = 100;
    void reset(const TerrainGrid &grid, ChunkCoord terrain = {}, float referenceHeight = 200);
    void rebaseGrid(const TerrainGrid &oldGrid, const TerrainGrid &newGrid);
    void update(const CameraInput &, float seconds, const TerrainGrid &);
    glm::vec3 forward() const;
    glm::vec3 terrainOffset(ChunkCoord terrain, const TerrainGrid &) const;
    glm::mat4 viewProjection(float aspect) const;
};
} // namespace ow

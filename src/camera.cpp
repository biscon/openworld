#include "camera.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <stdexcept>
namespace ow {
glm::vec3 Camera::forward() const {
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}
void Camera::reset(const TerrainGrid &g, ChunkCoord terrain, float referenceHeight) {
    chunk = terrain;
    local = {g.extent() * 0.5, referenceHeight + g.extent() * 0.55, g.extent() * 0.95};
    yaw = 0;
    pitch = -0.65f;
    speed = 100;
}
void Camera::rebaseGrid(const TerrainGrid &oldGrid, const TerrainGrid &newGrid) {
    validateGrid(oldGrid);
    validateGrid(newGrid);
    if (oldGrid.extent() == newGrid.extent())
        return;
    Camera next = *this;
    next.local.x += double(chunk.x) * oldGrid.extent();
    next.local.z += double(chunk.z) * oldGrid.extent();
    next.chunk = {};
    next.update({}, 0, newGrid);
    *this = next;
}
void Camera::update(const CameraInput &input, float dt, const TerrainGrid &g) {
    if (!std::isfinite(dt) || dt < 0)
        return;
    yaw = std::remainder(yaw + input.look.x * 0.0025f, 6.28318530718f);
    pitch = std::clamp(pitch - input.look.y * 0.0025f, -1.55334f, 1.55334f);
    const glm::vec3 right{std::cos(yaw), 0, std::sin(yaw)};
    auto direction =
        right * input.movement.x + glm::vec3(0, 1, 0) * input.movement.y + forward() * input.movement.z;
    const float length = glm::length(direction);
    if (length > 0)
        local += glm::dvec3(direction / length) * double(dt * speed * (input.boost ? 4 : 1));
    for (int axis : {0, 2}) {
        const double carry = std::floor(local[axis] / g.extent());
        auto &coord = axis == 0 ? chunk.x : chunk.z;
        // Camera navigation has a deliberately conservative bound, far beyond this
        // demo.
        const double next = double(coord) + carry;
        if (!std::isfinite(next) || std::abs(next) > 1e12)
            throw std::runtime_error("Camera coordinate out of range");
        coord += int64_t(carry);
        local[axis] -= carry * g.extent();
    }
}
glm::vec3 Camera::terrainOffset(ChunkCoord terrain, const TerrainGrid &g) const {
    return {float((double(terrain.x) - double(chunk.x)) * g.extent() - local.x), float(-local.y),
            float((double(terrain.z) - double(chunk.z)) * g.extent() - local.z)};
}
glm::mat4 Camera::viewProjection(float aspect) const {
    auto p = glm::perspective(glm::radians(verticalFovDegrees), aspect, renderFar, nearPlane);
    p[1][1] *= -1;
    return p * glm::lookAt(glm::vec3(0), forward(), glm::vec3(0, 1, 0));
}
} // namespace ow

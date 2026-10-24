#pragma once
#include "scene.hpp"
#include <array>
namespace ow {
constexpr uint32_t vegetationInteractionSize = 128, vegetationInteractionShapes = 16;
struct VegetationInteractionSettings {
    bool enabled = true;
    float recoveryHalfLife = 1.5f;
};
// Grounded capsules: radius .1..4 m, segment length <=64 m; teleports use reset.
struct VegetationContact {
    WorldPosition a, b;
    float radius = .65f, strength = 1;
};
struct VegetationInteractionInput {
    WorldPosition anchor;
    uint64_t reset = 0;
    bool active = false;
    uint32_t count = 0, rejected = 0;
    std::array<VegetationContact, vegetationInteractionShapes> shapes{};
    bool append(const VegetationContact &shape) {
        if (count >= shapes.size()) {
            ++rejected;
            return false;
        }
        shapes[count++] = shape;
        return true;
    }
};
struct VegetationProxy {
    WorldPosition position;
    bool active = false, route = false, marker = true;
    float radius = .65f, strength = 1;
};
void validate(const VegetationInteractionSettings &, const VegetationInteractionInput &);
WorldPosition vegetationFieldOrigin(WorldPosition);
} // namespace ow

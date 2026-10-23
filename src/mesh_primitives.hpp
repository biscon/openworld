#pragma once
#include "scene.hpp"
namespace ow {
MeshData makeQuad(); // XY [-1,1], normal +Z, CCW, UV origin at bottom left.
MeshData makeSphere(uint32_t rings = 16, uint32_t sectors = 32);
} // namespace ow

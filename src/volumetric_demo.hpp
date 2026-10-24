#pragma once
#include "renderer.hpp"
#include <array>
namespace ow {
// Owned review geometry; no dependency on production artwork or world saves.
class VolumetricDemo {
  public:
    explicit VolumetricDemo(Renderer &r) : renderer_(r) {}
    ~VolumetricDemo();
    void configure(uint32_t preset, WorldPosition anchor); // 0 off, 1 mist, 2 shafts
  private:
    Renderer &renderer_;
    MeshHandle quad_;
    std::array<MaterialHandle, 2> materials_{};
    std::array<InstanceHandle, 12> instances_{};
    std::array<FogVolumeHandle, 2> volumes_{};
    std::array<LightHandle, 2> lights_{};
    void clear();
};
} // namespace ow

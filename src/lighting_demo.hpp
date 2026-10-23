#pragma once
#include "renderer.hpp"
namespace ow {
// Deterministic application/profiling content, entirely through Renderer APIs.
class LightingDemo {
  public:
    explicit LightingDemo(Renderer &r) : renderer_(r) {
        lights_.reserve(4096);
        base_.reserve(4096);
        instances_.reserve(32);
        materials_.reserve(8);
    }
    ~LightingDemo();
    void configure(bool enabled, WorldPosition anchor, uint32_t count, bool overlap, uint32_t layers = 3);
    void animate(float seconds);

  private:
    Renderer &renderer_;
    WorldPosition anchor_;
    std::vector<LightHandle> lights_;
    std::vector<LocalLight> base_;
    std::vector<InstanceHandle> instances_;
    std::vector<MaterialHandle> materials_;
    MeshHandle sphere_, quad_;
    TextureHandle checker_;
    void clear();
};
} // namespace ow

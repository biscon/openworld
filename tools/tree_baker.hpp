#pragma once
#include "tree_library.hpp"
namespace ow {
struct TreeBake {
    TextureData colour, normal, depth;
};
// Owns a headless Vulkan device. Blocking work is restricted to offline tools.
class TreeBaker {
  public:
    explicit TreeBaker(const std::filesystem::path &shaders, bool validation = false);
    ~TreeBaker();
    TreeBake bake(const TreeAsset &, const TextureData &bark, const TextureData &leaf);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

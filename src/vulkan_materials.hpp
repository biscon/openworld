#pragma once
#include "material_assets.hpp"
#include "scene.hpp"
#include <memory>
#include <vulkan/vulkan.h>
namespace ow {
struct MaterialCapabilities {
    bool bindless = false, anisotropy = false;
    uint32_t images = 0, arrays = 4;
    float maxAnisotropy = 1;
};
MaterialCapabilities materialCapabilities(VkPhysicalDevice, bool forceFixed, bool forceTrilinear);
// Called on the render thread. prepare() requires this frame's completed fence.
class VulkanMaterials {
  public:
    VulkanMaterials(VkPhysicalDevice, VkDevice, MaterialCapabilities, uint64_t budget);
    ~VulkanMaterials();
    VkDescriptorSetLayout layout() const;
    VkDescriptorSet set(uint32_t frame) const;
    const MaterialStats &stats() const;
    TextureHandle uploadTexture(const TextureData &);
    bool releaseTexture(TextureHandle);
    MaterialHandle createMaterial(const MeshMaterial &);
    bool releaseMaterial(MaterialHandle);
    uint32_t resolve(MaterialHandle) const;
    const MeshMaterial &meshMaterial(MaterialHandle) const;
    bool load(const std::filesystem::path &, bool forceRgba = false);
    void prepare(uint32_t frame, const Camera &, TerrainGrid, const TerrainSettings &,
                 const TerrainMaterialSettings &, bool terrainCoordinates = true);
    void recordUploads(VkCommandBuffer);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

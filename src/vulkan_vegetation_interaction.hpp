#pragma once
#include "render_resources.hpp"
#include "vegetation.hpp"
namespace ow {
class VulkanVegetationInteraction {
  public:
    VulkanVegetationInteraction(VkPhysicalDevice, VkDevice, const std::filesystem::path &, uint64_t budget,
                                bool diagnostics);
    ~VulkanVegetationInteraction();
    void prepare(uint32_t frame, WorldPosition camera, WorldPosition previousCamera,
                 const VegetationSettings &);
    void record(VkCommandBuffer);
    void submitted();
    void copySnapshot(VkCommandBuffer);
    std::vector<glm::vec2> snapshot() const;
    const render_detail::Buffer &currentBuffer() const { return fields_[current_]; }
    const render_detail::Buffer &previousBuffer() const { return fields_[previous_]; }
    glm::vec4 currentMapping{}, previousMapping{}; // camera-relative origin x/z, enabled, reset
    uint64_t bytes = 0;
    bool reset = false;

  private:
    struct Uniform {
        glm::ivec4 scroll{};
        glm::vec4 response{};
        glm::uvec4 control{};
        std::array<glm::vec4, 16> endpoints{}, properties{};
    };
    static_assert(sizeof(Uniform) == 560);
    VkDevice device_;
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool pool_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    std::array<render_detail::Buffer, 3> fields_;
    std::array<render_detail::Buffer, 2> uniforms_;
    std::array<VkDescriptorSet, 2> sets_{};
    render_detail::Buffer readback_;
    uint32_t frame_ = 0, current_ = 1, previous_ = 0;
    WorldPosition origin_, previousOrigin_;
    float time_ = 0, previousTime_ = 0;
    uint64_t generation_ = 0, previousGeneration_ = 0;
    bool active_ = false, previousActive_ = false;
};
} // namespace ow

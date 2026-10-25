#pragma once
#include "tree_gpu_resources.hpp"
#include "tree_library.hpp"
namespace ow {
inline VkPipeline treeGraphicsPipeline(VkDevice d, VkPipelineLayout layout, VkShaderModule vs,
                                       VkShaderModule fs, const std::vector<VkFormat> &colours,
                                       bool writeDepth, VkCompareOp compare = VK_COMPARE_OP_GREATER,
                                       uint32_t attributeCount = 5) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &s : stages) {
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    VkVertexInputBindingDescription binding{0, sizeof(TreeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TreeVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TreeVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TreeVertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, deformation)},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, tangent)}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = attributeCount;
    input.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = true;
    depth.depthWriteEnable = writeDepth;
    depth.depthCompareOp = compare;
    std::array<VkPipelineColorBlendAttachmentState, 3> attachments{};
    for (auto &a : attachments)
        a.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = colours.size();
    blend.pAttachments = attachments.data();
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkPipelineRenderingCreateInfo render{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    render.colorAttachmentCount = colours.size();
    render.pColorAttachmentFormats = colours.data();
    render.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &render;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &input;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = layout;
    VkPipeline p{};
    render_detail::check(vkCreateGraphicsPipelines(d, {}, 1, &ci, nullptr, &p));
    return p;
}
} // namespace ow

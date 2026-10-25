#include "tree_baker.hpp"
#include "tree_pipeline.hpp"
#include <atomic>
#include <iostream>
namespace ow {
using namespace render_detail;
struct TreeBaker::Impl {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    std::atomic<uint32_t> messages{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                 void *user) {
        static_cast<Impl *>(user)->messages.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[Vulkan validation] tree bake: " << data->pMessage << '\n';
        return VK_FALSE;
    }
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool commands{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkDescriptorSetLayout setLayout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkSampler sampler{};
    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (layout)
                vkDestroyPipelineLayout(device, layout, nullptr);
            if (pool)
                vkDestroyDescriptorPool(device, pool, nullptr);
            if (setLayout)
                vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            if (sampler)
                vkDestroySampler(device, sampler, nullptr);
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (commands)
                vkDestroyCommandPool(device, commands, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy)
                destroy(instance, messenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};
TreeBaker::TreeBaker(const std::filesystem::path &shaders, bool validation)
    : impl_(std::make_unique<Impl>()) {
    auto &x = *impl_;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "OpenWorld offline tree baker";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = Impl::report;
    debug.pUserData = &x;
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &sync;
    const char *extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    if (validation) {
        ic.enabledLayerCount = 1;
        ic.ppEnabledLayerNames = &layer;
        uint32_t count = 0;
        check(vkEnumerateInstanceExtensionProperties(layer, &count, nullptr));
        std::vector<VkExtensionProperties> properties(count);
        check(vkEnumerateInstanceExtensionProperties(layer, &count, properties.data()));
        bool synchronization = std::any_of(properties.begin(), properties.end(), [](const auto &p) {
            return std::strcmp(p.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0;
        });
        ic.enabledExtensionCount = synchronization ? 2 : 1;
        ic.ppEnabledExtensionNames = extensions;
        if (synchronization)
            debug.pNext = &validationFeatures;
        ic.pNext = &debug;
    }
    check(vkCreateInstance(&ic, nullptr, &x.instance));
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(x.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!create)
            throw std::runtime_error("Tree baker validation messenger unavailable");
        debug.pNext = nullptr;
        check(create(x.instance, &debug, nullptr, &x.messenger));
    }
    uint32_t n = 0;
    check(vkEnumeratePhysicalDevices(x.instance, &n, nullptr));
    std::vector<VkPhysicalDevice> devices(n);
    check(vkEnumeratePhysicalDevices(x.instance, &n, devices.data()));
    uint32_t family = ~0u;
    for (auto p : devices) {
        VkPhysicalDeviceVulkan13Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f;
        vkGetPhysicalDeviceFeatures2(p, &f2);
        if (!f.dynamicRendering || !f.synchronization2)
            continue;
        uint32_t c;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &c, nullptr);
        std::vector<VkQueueFamilyProperties> q(c);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &c, q.data());
        for (uint32_t i = 0; i < c; ++i)
            if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                x.physical = p;
                family = i;
                break;
            }
        if (x.physical)
            break;
    }
    if (!x.physical)
        throw std::runtime_error("Tree baking requires Vulkan 1.3 graphics device");
    float priority = 1;
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = family;
    qc.queueCount = 1;
    qc.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features.dynamicRendering = features.synchronization2 = true;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &features;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    check(vkCreateDevice(x.physical, &dc, nullptr, &x.device));
    vkGetDeviceQueue(x.device, family, 0, &x.queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(x.device, &pc, nullptr, &x.commands));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = x.commands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(x.device, &ca, &x.cmd));
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(x.device, &fc, nullptr, &x.fence));
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sc.bindingCount = 3;
    sc.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(x.device, &sc, nullptr, &x.setLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(x.device, &dp, nullptr, &x.pool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = x.pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &x.setLayout;
    check(vkAllocateDescriptorSets(x.device, &da, &x.set));
    VkSamplerCreateInfo sa{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sa.minFilter = sa.magFilter = VK_FILTER_LINEAR;
    sa.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sa.addressModeU = sa.addressModeV = sa.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sa.maxLod = 16;
    check(vkCreateSampler(x.device, &sa, nullptr, &x.sampler));
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 68};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &x.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(x.device, &pl, nullptr, &x.layout));
    ShaderModule vs(x.device, shaders / "tree_bake.vert.spv"), fs(x.device, shaders / "tree_bake.frag.spv");
    x.pipeline =
        treeGraphicsPipeline(x.device, x.layout, vs.handle, fs.handle,
                             {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM}, true,
                             VK_COMPARE_OP_GREATER, 3);
}
TreeBaker::~TreeBaker() = default;
TreeBake TreeBaker::bake(const TreeAsset &a, const TextureData &bark, const TextureData &leaf) {
    auto &x = *impl_;
    auto d = x.device;
    auto cmd = x.cmd;
    uint32_t size = a.frames * a.tileSize;
    TreeImage input[2], output[3], depth;
    input[0].load(x.physical, d, bark);
    input[1].load(x.physical, d, leaf);
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16_UNORM};
    for (int i = 0; i < 3; ++i)
        output[i].create(x.physical, d, size, size, 1, formats[i],
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    depth.create(x.physical, d, size, size, 1, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    Buffer vertices, indices, readback;
    uint32_t nv = 0, ni = 0;
    for (auto &p : a.lods[0]) {
        nv += p.vertices.size();
        ni += p.indices.size();
    }
    vertices.create(x.physical, d, uint64_t(nv) * 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    indices.create(x.physical, d, uint64_t(ni) * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    readback.create(x.physical, d, uint64_t(size) * size * 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    uint32_t v = 0, i = 0;
    for (auto &p : a.lods[0]) {
        std::memcpy(static_cast<TreeVertex *>(vertices.mapped) + v, p.vertices.data(),
                    p.vertices.size() * 64);
        auto *dst = static_cast<uint32_t *>(indices.mapped) + i;
        for (auto idx : p.indices)
            *dst++ = idx + v;
        v += p.vertices.size();
        i += p.indices.size();
    }
    VkDescriptorImageInfo ii[] = {{}, {}, {x.sampler, {}, {}}};
    VkWriteDescriptorSet w[3]{};
    for (int j = 0; j < 3; ++j) {
        if (j < 2)
            ii[j] = {{}, input[j].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[j] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[j].dstSet = x.set;
        w[j].dstBinding = j;
        w[j].descriptorCount = 1;
        w[j].descriptorType = j < 2 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        w[j].pImageInfo = &ii[j];
    }
    vkUpdateDescriptorSets(d, 3, w, 0, nullptr);
    check(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd, &begin));
    for (auto &t : input)
        t.upload(cmd);
    VkRenderingAttachmentInfo colours[3]{};
    for (int j = 0; j < 3; ++j) {
        treeImageBarrier(
            cmd, output[j].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        colours[j] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colours[j].imageView = output[j].view;
        colours[j].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colours[j].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colours[j].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    treeImageBarrier(
        cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0, 0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo di{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    di.imageView = depth.view;
    di.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    di.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    di.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = {size, size};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = colours;
    ri.pDepthAttachment = &di;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, x.layout, 0, 1, &x.set, 0, nullptr);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    struct Push {
        glm::vec4 right, up, direction, centre;
        uint32_t part;
    };
    for (uint32_t z = 0; z < a.frames; ++z)
        for (uint32_t y = 0; y < a.frames; ++y) {
            auto dir = treeOctDecode(glm::vec2(y, z) / float(a.frames - 1));
            auto right = glm::normalize(
                glm::cross(std::abs(dir.y) < .99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1), dir));
            auto up = glm::cross(dir, right);
            Push push{glm::vec4(right, 0), glm::vec4(up, 0), glm::vec4(dir, 0), a.capture, 0};
            VkViewport vp{
                float(y * a.tileSize), float(z * a.tileSize), float(a.tileSize), float(a.tileSize), 0, 1};
            VkRect2D sc{{int(y * a.tileSize), int(z * a.tileSize)}, {a.tileSize, a.tileSize}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            uint32_t first = 0;
            for (uint32_t part = 0; part < 2; ++part) {
                push.part = part;
                vkCmdPushConstants(cmd, x.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, 68, &push);
                auto count = a.lods[0][part].indices.size();
                vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
                first += count;
            }
        }
    vkCmdEndRendering(cmd);
    uint64_t offset = 0;
    for (int j = 0; j < 3; ++j) {
        treeImageBarrier(cmd, output[j].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {size, size, 1};
        vkCmdCopyImageToBuffer(cmd, output[j].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                               &copy);
        offset += uint64_t(size) * size * (j == 2 ? 2 : 4);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    check(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(x.queue, 1, &submit, x.fence));
    check(vkWaitForFences(d, 1, &x.fence, true, UINT64_MAX));
    check(vkResetFences(d, 1, &x.fence));
    TreeBake result;
    TextureData *maps[] = {&result.colour, &result.normal, &result.depth};
    offset = 0;
    for (int j = 0; j < 3; ++j) {
        auto &t = *maps[j];
        t.width = t.height = size;
        t.format = j == 0 ? TextureFormat::Srgb8 : j == 1 ? TextureFormat::Rgba8 : TextureFormat::R16;
        uint64_t bytes = uint64_t(size) * size * (j == 2 ? 2 : 4);
        auto *start = static_cast<uint8_t *>(readback.mapped) + offset;
        t.bytes.assign(start, start + bytes);
        offset += bytes;
    }
    if (x.messages.load())
        throw std::runtime_error("Tree bake Vulkan validation failed");
    return result;
}
} // namespace ow

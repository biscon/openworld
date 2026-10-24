#include "renderer.hpp"
#include "render_resolution.hpp"
#include "render_resources.hpp"
#include "vulkan_atmosphere.hpp"
#include "vulkan_clusters.hpp"
#include "vulkan_gtao.hpp"
#include "vulkan_materials.hpp"
#include "vulkan_meshes.hpp"
#include "vulkan_post.hpp"
#include "vulkan_shadows.hpp"
#include "vulkan_spatial_aa.hpp"
#include "vulkan_terrain.hpp"
#include "vulkan_terrain_stream.hpp"
#include "vulkan_vegetation.hpp"
#include "vulkan_visibility.hpp"
#include "vulkan_volumetrics.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ow {
namespace {
constexpr uint32_t framesInFlight = 2;
constexpr uint64_t forever = std::numeric_limits<uint64_t>::max();

void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) +
                                 ")");
}

bool hasExtension(const std::vector<VkExtensionProperties> &list, const char *name) {
    return std::any_of(list.begin(), list.end(),
                       [name](const auto &e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice physical) {
    uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr),
          "Enumerate device extensions");
    std::vector<VkExtensionProperties> result(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, result.data()),
          "Enumerate device extensions");
    return result;
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};
struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE; // Null for swapchain-owned images.
    VkImageView view = VK_NULL_HANDLE;
    // Presentation waits can outlive a submission fence. Reuse by acquired image,
    // not frame slot.
    VkSemaphore presentReady = VK_NULL_HANDLE;
};
struct Frame {
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkImage hdr{};
    VkDeviceMemory hdrMemory{};
    VkImageView hdrView{};
    Buffer lightingBuffer;
    void *lightingMapped = nullptr;
    VkDescriptorSet lightingSet{}, outputSet{};
    float preExposure = 1;
    bool diagnostic = false, timed = false;
};
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout before,
                  VkImageLayout after, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t levels = 1,
                  uint32_t layers = 1) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, levels, 0, layers};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
} // namespace

struct Renderer::Impl {
    RendererOptions options;
    std::unique_ptr<VulkanAtmosphere> atmosphere;
    std::unique_ptr<VulkanVolumetrics> volumetrics;
    VkPipelineLayout skyLayout{};
    VkPipeline skyPipeline{}, skyTemporalPipeline{};
    std::unique_ptr<VulkanShadows> shadows;
    ShadowCapabilities shadowCaps;
    VkPipelineLayout shadowTerrainLayout{};
    VkPipeline shadowTerrainPipeline{};
    struct TerrainShadowDraw {
        VkDescriptorSet set;
        uint32_t caster;
    };
    std::vector<TerrainShadowDraw> shadowTerrainDraws;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    std::string gpuName;
    bool validation = false;
    std::atomic<uint32_t> errors{0};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline temporalTerrainPipeline{}, temporalTerrainEqualPipeline{};
    VkPipeline terrainDepthPipeline = VK_NULL_HANDLE, terrainEqualPipeline = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkExtent2D extent{1280, 800}; // Output framebuffer.
    VkExtent2D sceneExtent{};
    uint32_t maxSceneWidth = 16384, maxSceneHeight = 16384;
    bool sceneDirty = false;
    uint32_t minImageCount = 2;
    std::vector<Target> targets;
    std::array<Frame, framesInFlight> frames{};
    uint32_t frameIndex = 0;
    uint32_t lastTarget = 0;
    bool hasRendered = false, lastHiZ = false;
    bool dirty = false;
    bool imguiReady = false;
    std::unique_ptr<VulkanTerrain> terrain;
    std::unique_ptr<VulkanMaterials> materials;
    MaterialCapabilities materialCaps;
    TerrainSettings materialTerrainSettings;
    std::unique_ptr<VulkanTerrainStream> streaming;
    static constexpr VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkDescriptorSetLayout lightingSetLayout{}, outputSetLayout{}, emptySetLayout{};
    VkDescriptorPool lightingPool{};
    VkPipelineLayout outputLayout{};
    VkPipeline outputPipeline{};
    std::unique_ptr<VulkanClusters> clusters;
    std::unique_ptr<VulkanMeshes> meshes;
    std::unique_ptr<VulkanVegetation> vegetation;
    std::unique_ptr<VulkanVisibility> visibility;
    VisibilityCapabilities visibilityCaps;
    std::unique_ptr<VulkanPost> post;
    std::unique_ptr<VulkanSpatialAA> spatialAa;
    PostCapabilities postCaps;
    std::unique_ptr<TerrainHistory> terrainHistory;
    std::unique_ptr<VulkanGtao> gtao;
    GtaoCapabilities gtaoCaps;
    GtaoMode gtaoMode = GtaoSettings{}.mode;
    struct TerrainDraw {
        glm::vec4 offset;
        glm::uvec4 options, patch;
        glm::vec4 morph;
        glm::uvec4 normals, indices;
    };
    struct PreparedTerrain {
        TerrainDraw data;
        TerrainPatchDraw patch;
        VisibilityBounds bounds;
        uint32_t count = 0, first = 0, order = 0, batch = ~0u;
    };
    std::vector<PreparedTerrain> preparedTerrain;
    mutable SceneStats sceneStatistics;
    VkImage environmentImage{}, brdfImage{};
    VkDeviceMemory environmentMemory{}, brdfMemory{};
    VkImageView environmentView{}, brdfView{};
    VkSampler lightingSampler{};
    VkQueryPool renderQueries{};
    double timestampPeriod = 0;
    uint32_t timestampBits = 0;
    RenderingStats renderingStats;
    std::vector<uint32_t> uiShaderCode;
    uint32_t lastFrame = 0;
    Camera terrainCamera;
    Camera lodObserver;
    bool observerInitialized = false;
    TerrainRenderStats renderStats{};

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT,
                                                        const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                        void *user) {
        auto *self = static_cast<Impl *>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            ++self->errors;
        std::cerr << "[Vulkan validation] " << data->pMessage << '\n';
        return VK_FALSE;
    }

    void init(const RendererOptions &config) {
        options = config;
        if (options.vegetation.streaming.enabled &&
            (!options.streaming.enabled || !options.streaming.hierarchical ||
             options.terrainGrid.spacing != .5f || options.terrainGrid.extent() < 4 ||
             std::fmod(options.terrainGrid.extent(), 4.0) != 0))
            throw std::invalid_argument("Streamed vegetation requires stationary hierarchical terrain with a "
                                        "4 m aligned extent");
        if (!options.width || !options.height || options.width > 16384 || options.height > 16384)
            throw std::invalid_argument("Offscreen extent must be in 1..16384");
        extent = {options.width, options.height};
        sceneRenderExtent(extent.width, extent.height, options.renderHeight);
        if (options.streaming.enabled) {
            if (options.terrainGrid.cells != options.terrainCapacity)
                throw std::invalid_argument("Streaming capacity must match its startup grid");
            options.lod.enabled = true;
        }
        const auto &capacity = options.sceneCapacity;
        if (!capacity.lights || capacity.lights > 65535 || !capacity.instances ||
            capacity.instances > 65535 || !capacity.meshes || capacity.meshes > 4096 ||
            !capacity.geometryBytes || capacity.clusterBytesPerFrame > (128ull << 20))
            throw std::invalid_argument("Invalid scene resource capacity");
        clusterGrid(options.width, options.height, capacity);
        createInstance();
        if (options.window && !SDL_Vulkan_CreateSurface(options.window, instance, nullptr, &surface))
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
        createDevice();
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        const auto &limits = properties.limits;
        maxSceneWidth = std::min({16384u, limits.maxImageDimension2D, limits.maxFramebufferWidth,
                                  limits.maxViewportDimensions[0]});
        maxSceneHeight = std::min({16384u, limits.maxImageDimension2D, limits.maxFramebufferHeight,
                                   limits.maxViewportDimensions[1]});
        createCommands();
        const auto lightingStart = std::chrono::steady_clock::now();
        materials =
            std::make_unique<VulkanMaterials>(physical, device, materialCaps, options.materialBudgetBytes);
        if (!options.materialDirectory.empty())
            materials->load(options.materialDirectory, options.forceRgbaMaterials);
        createLighting();
        atmosphere = std::make_unique<VulkanAtmosphere>(
            physical, device, queueFamily, options.atmosphereBudgetBytes, options.forceNoAtmosphere,
            options.shaderDirectory, options.curvedHorizon);
        volumetrics = std::make_unique<VulkanVolumetrics>(
            physical, device, queueFamily, options.volumetricCapacity, options.sceneCapacity.lights,
            options.forceNoVolumetrics, lightingSetLayout, options.shaderDirectory);
        renderingStats.lightingStartupMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lightingStart)
                .count();
        clusters =
            std::make_unique<VulkanClusters>(physical, device, options.sceneCapacity, queueFamily,
                                             lightingSetLayout, emptySetLayout, options.shaderDirectory);
        visibility =
            std::make_unique<VulkanVisibility>(physical, device, queueFamily, options.visibilityCapacity,
                                               visibilityCaps, options.shaderDirectory);
        gtao = std::make_unique<VulkanGtao>(physical, device, queueFamily, gtaoCaps, options.gtaoBudgetBytes,
                                            options.shaderDirectory);
        {
            auto cmd = beginImmediate();
            gtao->initialize(cmd);
            endImmediate(cmd);
        }
        shadows = std::make_unique<VulkanShadows>(
            physical, device, queueFamily, shadowCaps, options.shadowCapacity,
            options.vegetation.streaming.enabled ? options.vegetation.streaming.cellCapacity
            : (options.vegetation.fixture || options.vegetation.library) ? vegetationCellLimit
                                                                         : 0);
        {
            auto cmd = beginImmediate();
            shadows->initialize(cmd);
            endImmediate(cmd);
        }
        meshes = std::make_unique<VulkanMeshes>(physical, device, options.sceneCapacity, lightingSetLayout,
                                                *materials, options.shaderDirectory, materialCaps.bindless,
                                                shadows->layout(), postCaps.taa);
        if (options.vegetation.population && !options.vegetation.library)
            throw std::invalid_argument("Vegetation population requires a library");
        if (options.vegetation.fixture || options.vegetation.library)
            vegetation = std::make_unique<VulkanVegetation>(
                physical, device, queueFamily, lightingSetLayout, *materials, options.shaderDirectory,
                materialCaps.bindless, postCaps.taa, options.vegetation);
        terrain = std::make_unique<VulkanTerrain>(
            TerrainDevice{physical, device, queue, commandPool}, options.terrainGrid, options.terrainCapacity,
            options.normalSubdivisions, options.streaming.enabled ? 1 : options.terrainChunkCapacity,
            options.shaderDirectory, options.lod, lightingSetLayout, materials->layout(),
            visibility->layout());
        shadowTerrainDraws.reserve(options.shadowCapacity.casters);
        preparedTerrain.reserve(terrain->lod() ? terrain->lod()->drawCapacity() : maxTerrainChunks);
        const auto temporalCapacity =
            options.streaming.enabled
                ? options.visibilityCapacity
                : uint32_t(std::min<size_t>(options.visibilityCapacity, terrain->lod()
                                                                            ? terrain->lod()->drawCapacity()
                                                                            : options.terrainChunkCapacity));
        post =
            std::make_unique<VulkanPost>(physical, device, queueFamily, postCaps, options.taaBudgetBytes,
                                         options.bloomBudgetBytes, temporalCapacity, options.shaderDirectory);
        spatialAa = std::make_unique<VulkanSpatialAA>(physical, device, queueFamily,
                                                      options.spatialAaBudgetBytes, options.shaderDirectory);
        terrainHistory = std::make_unique<TerrainHistory>(options.streaming.enabled ? 0 : temporalCapacity);
        if (options.streaming.enabled)
            streaming = std::make_unique<VulkanTerrainStream>(
                *terrain, options.streaming, options.shaderDirectory, options.visibilityCapacity);
        createPresentationTargets();
        updateSceneTargets(false);
        createPipelines();
        if (options.ui)
            initImGui();
        std::cout << "Vulkan device: " << gpuName << "\nValidation: " << (validation ? "enabled" : "disabled")
                  << '\n';
    }

    void createInstance() {
        uint32_t count = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
              "Enumerate instance extensions");
        std::vector<VkExtensionProperties> available(count);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()),
              "Enumerate instance extensions");
        std::vector<const char *> extensions;
        if (options.window) {
            const char *const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&count);
            if (!sdlExtensions)
                throw std::runtime_error(SDL_GetError());
            extensions.assign(sdlExtensions, sdlExtensions + count);
        }
        VkInstanceCreateFlags flags = 0;
        if (hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "Enumerate layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "Enumerate layers");
        const char *validationLayer = "VK_LAYER_KHRONOS_validation";
        validation = options.validation &&
                     std::any_of(layers.begin(), layers.end(),
                                 [&](const auto &layer) {
                                     return std::strcmp(layer.layerName, validationLayer) == 0;
                                 }) &&
                     hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (options.requireValidation && !validation)
            throw std::runtime_error("Validation is required. Install vulkan-validation-layers and a "
                                     "working Vulkan driver.");
        if (options.validation && !validation)
            std::cerr << "Validation unavailable; continuing without it.\n";
        if (validation)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = debugCallback;
        debug.pUserData = this;

        // Enable synchronization validation as well as ordinary API validation when
        // supported.
        VkValidationFeatureEnableEXT syncValidation =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = &syncValidation;
        if (validation) {
            check(vkEnumerateInstanceExtensionProperties(validationLayer, &count, nullptr),
                  "Enumerate validation extensions");
            std::vector<VkExtensionProperties> layerExtensions(count);
            check(vkEnumerateInstanceExtensionProperties(validationLayer, &count, layerExtensions.data()),
                  "Enumerate validation extensions");
            if (hasExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
                extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                debug.pNext = &validationFeatures;
            }
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "OpenWorld";
        app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        app.pEngineName = "OpenWorld";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.flags = flags;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount = validation ? 1 : 0;
        info.ppEnabledLayerNames = validation ? &validationLayer : nullptr;
        info.pNext = validation ? &debug : nullptr;
        check(vkCreateInstance(&info, nullptr, &instance),
              "Create Vulkan 1.3 instance (check your Vulkan driver installation)");
        if (validation) {
            debug.pNext = nullptr;
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create)
                throw std::runtime_error("Debug messenger entry point unavailable");
            check(create(instance, &debug, nullptr, &messenger), "Create debug messenger");
        }
    }

    void createDevice() {
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate physical devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "Enumerate physical devices");
        int best = -1;
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_3)
                continue;
            VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &v13;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!v13.dynamicRendering || !v13.synchronization2)
                continue;
            if (surface && !hasExtension(deviceExtensions(candidate), VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;
            VkFormatProperties format{};
            vkGetPhysicalDeviceFormatProperties(candidate, hdrFormat, &format);
            const auto hdrRequired = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                     VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if ((format.optimalTilingFeatures & hdrRequired) != hdrRequired)
                continue;
            vkGetPhysicalDeviceFormatProperties(candidate, depthFormat, &format);
            if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
                continue;
            if (!surface) {
                vkGetPhysicalDeviceFormatProperties(candidate, colorFormat, &format);
                const auto required =
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
                if ((format.optimalTilingFeatures & required) != required)
                    continue;
            }
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
            for (uint32_t i = 0; i < count; ++i) {
                const auto needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if ((families[i].queueFlags & needed) != needed)
                    continue;
                VkBool32 present = VK_TRUE;
                if (surface)
                    check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present),
                          "Query present support");
                if (!present)
                    continue;
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 100
                                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 50
                                                                                               : 10;
                if (score > best) {
                    best = score;
                    physical = candidate;
                    queueFamily = i;
                    gpuName = props.deviceName;
                }
            }
        }
        if (!physical)
            throw std::runtime_error("No suitable Vulkan 1.3 device: need dynamic "
                                     "rendering, synchronization2, and a "
                                     "graphics/compute/present queue.");
        const auto available = deviceExtensions(physical);
        std::vector<const char *> extensions;
        if (surface)
            extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        // Required by portability implementations (including MoltenVK) when
        // advertised.
        if (hasExtension(available, "VK_KHR_portability_subset"))
            extensions.push_back("VK_KHR_portability_subset");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        v13.dynamicRendering = VK_TRUE;
        v13.synchronization2 = VK_TRUE;
        materialCaps = materialCapabilities(physical, options.forceFixedMaterials, options.forceTrilinear);
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physical, &supported);
        VkPhysicalDeviceFeatures features{};
        visibilityCaps = visibilityCapabilities(physical);
        gtaoCaps = gtaoCapabilities(physical, options.forceNoGtao, options.forceWideGtao);
        postCaps = postCapabilities(physical);
        postCaps.taa &= !options.forceNoTaa;
        postCaps.bloom &= !options.forceNoBloom;
        features.independentBlend = postCaps.taa;
        features.shaderStorageImageExtendedFormats = gtaoCaps.supported && gtaoCaps.compact;
        visibilityCaps.count &= !options.forceFixedIndirect;
        visibilityCaps.multiDraw &= !options.forceSingleIndirect;
        visibilityCaps.firstInstance &= !options.forceDirectVisibility;
        visibilityCaps.hiZ &= !options.forceNoHiZ;
        shadowCaps = shadowCapabilities(physical);
        shadowCaps.depth &= !options.forceNoShadowDepth;
        shadowCaps.filtered &= !options.forceManualShadowFiltering;
        shadowCaps.indirect &= !options.forceDirectShadows;
        features.drawIndirectFirstInstance = visibilityCaps.firstInstance || shadowCaps.indirect;
        features.multiDrawIndirect = visibilityCaps.multiDraw || shadowCaps.multi;
        features.samplerAnisotropy = materialCaps.anisotropy;
        features.textureCompressionBC = supported.textureCompressionBC;
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.drawIndirectCount = visibilityCaps.count;
        v12.runtimeDescriptorArray = materialCaps.bindless;
        v12.shaderSampledImageArrayNonUniformIndexing = materialCaps.bindless;
        v13.pNext = &v12;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pEnabledFeatures = &features;
        info.pNext = &v13;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        check(vkCreateDevice(physical, &info, nullptr, &device), "Create device");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("No compatible Vulkan memory type");
    }

    void createBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags flags) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &buffer.handle), "Create buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, flags);
        check(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory), "Allocate buffer memory");
        check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "Bind buffer memory");
    }

    void destroyBuffer(Buffer &buffer) {
        if (buffer.handle)
            vkDestroyBuffer(device, buffer.handle, nullptr);
        if (buffer.memory)
            vkFreeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }

    VkDeviceSize createImage(VkImage &image, VkDeviceMemory &memory, VkFormat format, VkImageUsageFlags usage,
                             uint32_t width = 0, uint32_t height = 0, uint32_t levels = 1,
                             uint32_t layers = 1) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width ? width : extent.width, height ? height : extent.height, 1};
        info.mipLevels = levels;
        info.arrayLayers = layers;
        if (layers == 6)
            info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device, &info, nullptr, &image), "Create image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex =
            memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &memory), "Allocate image memory");
        check(vkBindImageMemory(device, image, memory, 0), "Bind image memory");
        return requirements.size;
    }

    VkImageView createView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t levels = 1,
                           uint32_t layers = 1) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = {aspect, 0, levels, 0, layers};
        VkImageView view = VK_NULL_HANDLE;
        check(vkCreateImageView(device, &info, nullptr, &view), "Create image view");
        return view;
    }

    VkCommandBuffer beginImmediate() {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device, &allocation, &cmd), "Allocate immediate commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &begin), "Begin immediate commands");
        return cmd;
    }

    void endImmediate(VkCommandBuffer cmd) {
        check(vkEndCommandBuffer(cmd), "End immediate commands");
        VkCommandBufferSubmitInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commands.commandBuffer = cmd;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        check(vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE), "Submit immediate commands");
        // Only used at startup and explicit screenshots, never in the ordinary
        // frame loop.
        check(vkQueueWaitIdle(queue), "Wait for immediate commands");
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
    }

    void createCommands() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &pool, nullptr, &commandPool), "Create command pool");
        for (auto &frame : frames) {
            VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocation.commandPool = commandPool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device, &allocation, &frame.commands), "Allocate frame commands");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device, &fence, nullptr, &frame.fence), "Create frame fence");
            if (surface) {
                VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                check(vkCreateSemaphore(device, &semaphore, nullptr, &frame.acquired),
                      "Create acquire semaphore");
            }
        }
    }

    VkShaderModule shader(const char *name) {
        const auto path = options.shaderDirectory / name;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("Cannot open shader: " + path.string() +
                                     ". Build the shaders target first.");
        const auto length = file.tellg();
        if (length <= 0 || length % 4)
            throw std::runtime_error("Invalid SPIR-V size: " + path.string());
        std::vector<uint32_t> code(static_cast<size_t>(length) / 4);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(code.data()), length))
            throw std::runtime_error("Cannot read shader: " + path.string());
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device, &info, nullptr, &module), "Create shader module");
        return module;
    }

    void uploadLightingImage(VkImage &image, VkDeviceMemory &memory, VkImageView &view,
                             const std::vector<glm::vec4> &pixels, uint32_t size, uint32_t levels,
                             uint32_t layers) {
        renderingStats.lightingBytes += createImage(
            image, memory, hdrFormat, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, size,
            size, levels, layers);
        view = createView(image, hdrFormat, VK_IMAGE_ASPECT_COLOR_BIT, levels, layers);
        Buffer upload;
        try {
            createBuffer(upload, pixels.size() * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            void *mapped = nullptr;
            check(vkMapMemory(device, upload.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map IBL upload");
            auto *packed = static_cast<uint16_t *>(mapped);
            for (size_t i = 0; i < pixels.size(); i++)
                for (int c = 0; c < 4; c++)
                    packed[i * 4 + c] = glm::packHalf1x16(pixels[i][c]);
            vkUnmapMemory(device, upload.memory);
            auto cmd = beginImmediate();
            imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, levels, layers);
            VkDeviceSize offset = 0;
            for (uint32_t mip = 0; mip < levels; mip++) {
                uint32_t width = size >> mip;
                VkBufferImageCopy copy{};
                copy.bufferOffset = offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers};
                copy.imageExtent = {width, width, 1};
                vkCmdCopyBufferToImage(cmd, upload.handle, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &copy);
                offset += VkDeviceSize(width) * width * layers * 8;
            }
            imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, levels, layers);
            endImmediate(cmd);
        } catch (...) {
            destroyBuffer(upload);
            throw;
        }
        destroyBuffer(upload);
    }
    void createLighting() {
        VkDescriptorSetLayoutBinding b[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {13, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {14, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {19, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {20, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {21, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {23, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {24, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        std::vector<VkDescriptorSetLayoutBinding> shared(std::begin(b), std::end(b));
        for (auto &binding : shared)
            if (binding.binding == 0 || (binding.binding >= 10 && binding.binding <= 13) ||
                binding.binding >= 16)
                binding.stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
        for (uint32_t k = 25; k <= 31; ++k)
            shared.push_back({k,
                              k == 25 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                              1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        li.bindingCount = uint32_t(shared.size());
        li.pBindings = shared.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &lightingSetLayout),
              "Create lighting layout");
        li.bindingCount = 0;
        li.pBindings = nullptr;
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &emptySetLayout),
              "Create empty compute layout");
        VkDescriptorSetLayoutBinding ob[] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        li.bindingCount = 4;
        li.pBindings = ob;
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &outputSetLayout), "Create output layout");
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 38},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 8}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 4;
        pool.poolSizeCount = 4;
        pool.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pool, nullptr, &lightingPool), "Create lighting pool");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &outputSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &outputLayout), "Create output pipeline layout");
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.maxLod = 6;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device, &sampler, nullptr, &lightingSampler), "Create lighting sampler");
        const auto &bake = environmentBake();
        uploadLightingImage(environmentImage, environmentMemory, environmentView, bake.cube, 64, 7, 6);
        uploadLightingImage(brdfImage, brdfMemory, brdfView, bake.brdf, 128, 1, 1);
        for (auto &frame : frames) {
            createBuffer(frame.lightingBuffer, sizeof(LightingUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkMapMemory(device, frame.lightingBuffer.memory, 0, sizeof(LightingUniform), 0,
                              &frame.lightingMapped),
                  "Map lighting constants");
            VkMemoryRequirements r{};
            vkGetBufferMemoryRequirements(device, frame.lightingBuffer.handle, &r);
            renderingStats.lightingBytes += r.size;
            VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool = lightingPool;
            a.descriptorSetCount = 1;
            a.pSetLayouts = &lightingSetLayout;
            check(vkAllocateDescriptorSets(device, &a, &frame.lightingSet), "Allocate lighting set");
            a.pSetLayouts = &outputSetLayout;
            check(vkAllocateDescriptorSets(device, &a, &frame.outputSet), "Allocate output set");
            VkDescriptorBufferInfo buffer{frame.lightingBuffer.handle, 0, sizeof(LightingUniform)};
            VkDescriptorImageInfo images[] = {
                {VK_NULL_HANDLE, environmentView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE, brdfView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {lightingSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
            VkWriteDescriptorSet writes[4]{};
            for (uint32_t i = 0; i < 4; i++) {
                auto &w = writes[i];
                w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = frame.lightingSet;
                w.dstBinding = i;
                w.descriptorCount = 1;
                w.descriptorType = b[i].descriptorType;
                if (!i)
                    w.pBufferInfo = &buffer;
                else
                    w.pImageInfo = &images[i - 1];
            }
            vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
            VkWriteDescriptorSet outputSampler{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            outputSampler.dstSet = frame.outputSet;
            outputSampler.dstBinding = 2;
            outputSampler.descriptorCount = 1;
            outputSampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            outputSampler.pImageInfo = &images[2];
            vkUpdateDescriptorSets(device, 1, &outputSampler, 0, nullptr);
        }
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        timestampBits = families[queueFamily].timestampValidBits;
        if (timestampBits && props.limits.timestampComputeAndGraphics) {
            timestampPeriod = props.limits.timestampPeriod;
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 8;
            check(vkCreateQueryPool(device, &query, nullptr, &renderQueries), "Create rendering timing pool");
        }
    }
    void updateLighting(const Camera &camera, const SceneSettings &settings, Frame &frame) {
        if (uint32_t(settings.localLights) > 2 || settings.clusterDiagnostic > 2)
            throw std::invalid_argument("Invalid local-light mode or diagnostic");
        LightingUniform u;
        const auto finite = [](glm::vec3 v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const auto &l = settings.lighting;
        const auto &m = settings.material;
        if (!finite(l.sunDirection) || !finite(l.sunColour) || !std::isfinite(l.sunLux) ||
            !std::isfinite(l.environmentNits) || !std::isfinite(l.environmentRotation) ||
            !finite(m.baseColour) || !finite(m.emissive) || !std::isfinite(m.roughness) ||
            !std::isfinite(m.metallic) || !std::isfinite(m.ao))
            throw std::invalid_argument("Lighting/material parameters must be finite");
        const bool legacyDiagnostic =
            settings.shading == ShadingMode::Legacy ||
            (settings.terrainVisible && (settings.normalView || settings.lodColours || settings.wireframe ||
                                         settings.chunkBorders || settings.terrainLodErrorView));
        const bool materialDiagnostic =
            settings.terrainMaterial.enabled && materials->stats().loaded &&
            (settings.terrainMaterial.weights || settings.terrainMaterial.diagnostic > 0);
        frame.diagnostic = legacyDiagnostic || materialDiagnostic || settings.clusterDiagnostic != 0;
        frame.preExposure = frame.diagnostic ? 1 : exposure(settings.output);
        u.viewProjection = camera.viewProjection(float(sceneExtent.width) / sceneExtent.height);
        u.camera = glm::vec4(
            glm::vec3(camera.local + glm::dvec3(double(camera.chunk.x) * options.terrainGrid.extent(), 0,
                                                double(camera.chunk.z) * options.terrainGrid.extent())),
            0);
        glm::vec3 direction = glm::dot(l.sunDirection, l.sunDirection) > 1e-10f
                                  ? glm::normalize(l.sunDirection)
                                  : glm::vec3(0, 1, 0);
        u.sunDirection = glm::vec4(direction, 0);
        u.sunColour = glm::vec4(glm::clamp(l.sunColour, glm::vec3(0), glm::vec3(1)) *
                                    (l.sunEnabled ? std::clamp(l.sunLux, 0.f, 200000.f) : 0),
                                0);
        u.environment = {l.environmentEnabled ? std::clamp(l.environmentNits, 0.f, 20000.f) : 0, 0,
                         std::cos(l.environmentRotation), std::sin(l.environmentRotation)};
        u.baseRoughness = glm::vec4(glm::clamp(m.baseColour, glm::vec3(0), glm::vec3(1)),
                                    std::clamp(m.roughness, .045f, 1.f));
        u.material = {std::clamp(m.metallic, 0.f, 1.f), std::clamp(m.ao, 0.f, 1.f),
                      gtao->active() ? 1.f : 0.f, 0};
        u.emissive = glm::vec4(glm::clamp(m.emissive, glm::vec3(0), glm::vec3(1e6f)), 0);
        u.output = {frame.preExposure, legacyDiagnostic ? 1.f : 0.f, float(settings.output.toneMapper),
                    materialDiagnostic ? 1.f : 0.f};
        u.irradiance = environmentBake().irradiance;
        std::memcpy(frame.lightingMapped, &u, sizeof(u));
    }
    void collectRenderTimings(Frame &frame) {
        if (!renderQueries || !frame.timed)
            return;
        uint64_t t[8]{};
        if (vkGetQueryPoolResults(device, renderQueries, frameIndex * 4, 4, sizeof(t), t, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                VK_SUCCESS &&
            t[1] && t[3] && t[5] && t[7]) {
            uint64_t mask = timestampBits == 64 ? ~uint64_t(0) : (uint64_t(1) << timestampBits) - 1;
            renderingStats.sceneMilliseconds = double((t[4] - t[2]) & mask) * timestampPeriod / 1e6;
            renderingStats.outputMilliseconds = double((t[6] - t[4]) & mask) * timestampPeriod / 1e6;
            renderingStats.frameMilliseconds = double((t[6] - t[0]) & mask) * timestampPeriod / 1e6;
            renderingStats.timingsAvailable = true;
        }
        frame.timed = false;
    }
    void destroyLighting() {
        if (renderQueries)
            vkDestroyQueryPool(device, renderQueries, nullptr);
        if (lightingPool)
            vkDestroyDescriptorPool(device, lightingPool, nullptr);
        for (auto &frame : frames) {
            if (frame.lightingMapped)
                vkUnmapMemory(device, frame.lightingBuffer.memory);
            destroyBuffer(frame.lightingBuffer);
        }
        for (auto view : {environmentView, brdfView})
            if (view)
                vkDestroyImageView(device, view, nullptr);
        for (auto image : {environmentImage, brdfImage})
            if (image)
                vkDestroyImage(device, image, nullptr);
        for (auto memory : {environmentMemory, brdfMemory})
            if (memory)
                vkFreeMemory(device, memory, nullptr);
        if (lightingSampler)
            vkDestroySampler(device, lightingSampler, nullptr);
        for (auto layout : {outputLayout})
            if (layout)
                vkDestroyPipelineLayout(device, layout, nullptr);
        for (auto layout : {lightingSetLayout, outputSetLayout, emptySetLayout})
            if (layout)
                vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
    void resizeOffscreen(uint32_t width, uint32_t height) {
        if (surface)
            throw std::invalid_argument("resizeOffscreen requires an offscreen renderer");
        if (!width || !height || width > 16384 || height > 16384)
            throw std::invalid_argument("Invalid offscreen extent");
        if (width == extent.width && height == extent.height)
            return;
        check(vkDeviceWaitIdle(device), "Wait before offscreen resize");
        destroyPresentationTargets();
        extent = {width, height};
        createPresentationTargets();
        updateSceneTargets(false);
        invalidateFrameResults();
    }
    VisibilityPyramidSnapshot readVisibilityPyramid() {
        if (!hasRendered || !lastHiZ)
            throw std::runtime_error("Pyramid readback requires a HiZ frame");
        check(vkDeviceWaitIdle(device), "Wait before pyramid diagnostic");
        VisibilityPyramidSnapshot result;
        result.width = sceneExtent.width;
        result.height = sceneExtent.height;
        VkDeviceSize bytes = 0;
        for (uint32_t w = sceneExtent.width, h = sceneExtent.height;;
             w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
            result.levels.emplace_back(size_t(w) * h);
            bytes += uint64_t(w) * h * 4;
            if (w == 1 && h == 1)
                break;
        }
        Buffer readback;
        try {
            createBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            auto cmd = beginImmediate();
            visibility->copyPyramid(cmd, lastFrame, readback.handle);
            endImmediate(cmd);
            void *mapped = nullptr;
            check(vkMapMemory(device, readback.memory, 0, bytes, 0, &mapped), "Map pyramid");
            size_t offset = 0;
            for (auto &level : result.levels) {
                std::memcpy(level.data(), static_cast<char *>(mapped) + offset, level.size() * 4);
                offset += level.size() * 4;
            }
            vkUnmapMemory(device, readback.memory);
        } catch (...) {
            destroyBuffer(readback);
            throw;
        }
        destroyBuffer(readback);
        return result;
    }
    ShadowSnapshot readShadowSnapshot() {
        if (!hasRendered || !shadows->active())
            throw std::runtime_error("Shadow snapshot requires a rendered shadow frame");
        check(vkDeviceWaitIdle(device), "Wait before shadow diagnostic");
        Buffer readback;
        try {
            const auto bytes = shadows->snapshotBytes();
            createBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            auto cmd = beginImmediate();
            shadows->copySnapshot(cmd, readback.handle);
            endImmediate(cmd);
            void *mapped = nullptr;
            check(vkMapMemory(device, readback.memory, 0, bytes, 0, &mapped), "Map shadow diagnostic");
            auto result = shadows->decodeSnapshot(mapped);
            vkUnmapMemory(device, readback.memory);
            destroyBuffer(readback);
            return result;
        } catch (...) {
            destroyBuffer(readback);
            throw;
        }
    }
    GtaoSnapshot readGtaoSnapshot() {
        if (!hasRendered)
            throw std::runtime_error("GTAO snapshot requires a rendered frame");
        const auto bytes = gtao->snapshotBytes(lastFrame);
        check(vkDeviceWaitIdle(device), "Wait before GTAO diagnostic");
        Buffer readback;
        try {
            createBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            auto cmd = beginImmediate();
            gtao->copySnapshot(cmd, lastFrame, readback.handle);
            endImmediate(cmd);
            void *mapped = nullptr;
            check(vkMapMemory(device, readback.memory, 0, bytes, 0, &mapped), "Map GTAO snapshot");
            auto result = gtao->decodeSnapshot(lastFrame, mapped);
            vkUnmapMemory(device, readback.memory);
            destroyBuffer(readback);
            return result;
        } catch (...) {
            destroyBuffer(readback);
            throw;
        }
    }
    VolumetricSnapshot readVolumetricSnapshot() {
        if (!hasRendered || !volumetrics->snapshotBytes())
            throw std::runtime_error("Volumetric snapshot requires an active submitted frame");
        check(vkDeviceWaitIdle(device), "Wait before volumetric snapshot");
        render_detail::Buffer readback;
        readback.create(physical, device, volumetrics->snapshotBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        true);
        auto cmd = beginImmediate();
        volumetrics->copySnapshot(cmd, readback.buffer);
        endImmediate(cmd);
        return volumetrics->decodeSnapshot(readback.mapped);
    }
    AtmosphereSnapshot readAtmosphereSnapshot() {
        if (!hasRendered || !atmosphere->snapshotBytes())
            throw std::runtime_error("Atmosphere snapshot requires a rendered frame");
        check(vkDeviceWaitIdle(device), "Wait before atmosphere snapshot");
        render_detail::Buffer readback;
        readback.create(physical, device, atmosphere->snapshotBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        true);
        auto cmd = beginImmediate();
        atmosphere->copySnapshot(cmd, readback.buffer);
        endImmediate(cmd);
        return atmosphere->decodeSnapshot(readback.mapped);
    }
    PostSnapshot readPostSnapshot() {
        if (!hasRendered || !post->snapshotBytes())
            throw std::runtime_error("Post snapshot requires an active rendered effect");
        check(vkDeviceWaitIdle(device), "Wait before post snapshot");
        render_detail::Buffer readback;
        readback.create(physical, device, post->snapshotBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        auto cmd = beginImmediate();
        post->copySnapshot(cmd, readback.buffer);
        endImmediate(cmd);
        return post->decodeSnapshot(readback.mapped);
    }
    HdrSnapshot readHdrSnapshot() {
        if (!hasRendered)
            throw std::runtime_error("HDR readback requires a rendered frame");
        check(vkDeviceWaitIdle(device), "Wait before HDR readback");
        HdrSnapshot result;
        result.width = sceneExtent.width;
        result.height = sceneExtent.height;
        result.preExposure = frames[lastFrame].preExposure;
        result.pixels.resize(size_t(sceneExtent.width) * sceneExtent.height);
        Buffer readback;
        const VkDeviceSize bytes = result.pixels.size() * 8;
        try {
            createBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            auto cmd = beginImmediate();
            auto image = frames[lastFrame].hdr;
            imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {sceneExtent.width, sceneExtent.height, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1,
                                   &copy);
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
            imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            endImmediate(cmd);
            void *mapped = nullptr;
            check(vkMapMemory(device, readback.memory, 0, bytes, 0, &mapped), "Map HDR readback");
            auto *values = static_cast<const uint16_t *>(mapped);
            for (size_t i = 0; i < result.pixels.size(); i++)
                for (int c = 0; c < 4; c++)
                    result.pixels[i][c] = glm::unpackHalf1x16(values[i * 4 + c]);
            vkUnmapMemory(device, readback.memory);
        } catch (...) {
            destroyBuffer(readback);
            throw;
        }
        destroyBuffer(readback);
        return result;
    }

    void createPresentationTargets() {
        if (surface) {
            VkSurfaceCapabilitiesKHR caps{};
            check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
                  "Query surface capabilities");
            if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
                throw std::runtime_error("Surface does not support color attachments");
            uint32_t count = 0;
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr),
                  "Query surface formats");
            if (!count)
                throw std::runtime_error("Surface has no formats");
            std::vector<VkSurfaceFormatKHR> formats(count);
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()),
                  "Query surface formats");
            auto format = formats.front();
            if (formats.size() == 1 && format.format == VK_FORMAT_UNDEFINED)
                format = {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
            for (const auto candidate : formats)
                if ((candidate.format == VK_FORMAT_B8G8R8A8_SRGB ||
                     candidate.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                    candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    format = candidate;
            if ((format.format != VK_FORMAT_B8G8R8A8_SRGB && format.format != VK_FORMAT_R8G8B8A8_SRGB) ||
                format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                throw std::runtime_error("5A requires an sRGB presentation surface");
            colorFormat = format.format;
            if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
                extent = caps.currentExtent;
            else {
                int w = 0, h = 0;
                if (!SDL_GetWindowSizeInPixels(options.window, &w, &h))
                    throw std::runtime_error(SDL_GetError());
                extent.width = std::clamp(uint32_t(std::max(w, 1)), caps.minImageExtent.width,
                                          caps.maxImageExtent.width);
                extent.height = std::clamp(uint32_t(std::max(h, 1)), caps.minImageExtent.height,
                                           caps.maxImageExtent.height);
            }
            if (!extent.width || !extent.height)
                throw std::runtime_error("Cannot create a zero-sized swapchain");
            minImageCount = std::max(2u, caps.minImageCount);
            count = std::max(minImageCount, caps.minImageCount + 1);
            if (caps.maxImageCount)
                count = std::min(count, caps.maxImageCount);
            if (count < minImageCount)
                throw std::runtime_error("Surface requires fewer than two swapchain "
                                         "images; unsupported by this demo");
            VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            for (const auto candidate :
                 {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
                if (caps.supportedCompositeAlpha & candidate) {
                    alpha = candidate;
                    break;
                }
            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = surface;
            info.minImageCount = count;
            info.imageFormat = colorFormat;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = alpha;
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            info.clipped = VK_TRUE;
            check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "Create swapchain");
            check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "Get swapchain images");
            std::vector<VkImage> images(count);
            check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()), "Get swapchain images");
            targets.resize(count);
            for (uint32_t i = 0; i < count; ++i)
                targets[i].image = images[i];
        } else {
            targets.resize(framesInFlight);
            for (auto &target : targets)
                createImage(target.image, target.memory, colorFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
        for (auto &target : targets) {
            target.view = createView(target.image, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
            if (surface) {
                VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                check(vkCreateSemaphore(device, &semaphore, nullptr, &target.presentReady),
                      "Create present semaphore");
            }
        }
    }

    void updateSceneTargets(bool wait) {
        const auto next = sceneRenderExtent(extent.width, extent.height, options.renderHeight, maxSceneWidth,
                                            maxSceneHeight);
        sceneDirty = false;
        if (next.width == sceneExtent.width && next.height == sceneExtent.height)
            return;
        if (wait)
            check(vkDeviceWaitIdle(device), "Wait before scene resolution change");
        destroySceneTargets();
        atmosphere->invalidateSceneViews();
        volumetrics->invalidateSceneViews();
        sceneExtent = {next.width, next.height};
        visibility->resize(sceneExtent.width, sceneExtent.height);
        gtao->resize(sceneExtent.width, sceneExtent.height, gtaoMode);
        clusters->resize(sceneExtent.width, sceneExtent.height);
        for (auto &frame : frames) {
            createImage(frame.depth, frame.depthMemory, depthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                            ((visibilityCaps.hiZ || gtaoCaps.supported || postCaps.taa ||
                              atmosphere->supported() || volumetrics->supported())
                                 ? VK_IMAGE_USAGE_SAMPLED_BIT
                                 : 0),
                        sceneExtent.width, sceneExtent.height);
            frame.depthView = createView(frame.depth, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
            renderingStats.hdrBytes += createImage(
                frame.hdr, frame.hdrMemory, hdrFormat,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    ((atmosphere->supported() || volumetrics->supported()) ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                sceneExtent.width, sceneExtent.height);
            frame.hdrView = createView(frame.hdr, hdrFormat, VK_IMAGE_ASPECT_COLOR_BIT);
            VkDescriptorImageInfo image{VK_NULL_HANDLE, frame.hdrView,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = frame.outputSet;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &image;
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        }
    }

    enum class DepthMode { Write, Prepass, Equal, Sky };

    void createPipelines() {
        VkDescriptorSetLayout shadowLayouts[] = {streaming ? streaming->descriptorLayout()
                                                           : terrain->descriptorLayout(),
                                                 lightingSetLayout, materials->layout(), shadows->layout()};
        VkPipelineLayoutCreateInfo sl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        sl.setLayoutCount = 4;
        sl.pSetLayouts = shadowLayouts;
        VkPushConstantRange shadowPush{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
        sl.pushConstantRangeCount = 1;
        sl.pPushConstantRanges = &shadowPush;
        check(vkCreatePipelineLayout(device, &sl, nullptr, &shadowTerrainLayout), "Shadow terrain layout");
        createGraphicsPipeline(shadowTerrainPipeline,
                               streaming ? "shadow_terrain_stream.vert.spv" : "shadow_terrain.vert.spv",
                               streaming ? "shadow_terrain_stream.frag.spv" : nullptr, shadowTerrainLayout,
                               false, DepthMode::Prepass);

        createGraphicsPipeline(
            graphicsPipeline, streaming ? "terrain_stream.vert.spv" : "terrain.vert.spv",
            streaming
                ? (materialCaps.bindless ? "terrain_stream_bindless.frag.spv" : "terrain_stream.frag.spv")
                : (materialCaps.bindless ? "terrain_bindless.frag.spv" : "terrain.frag.spv"),
            streaming ? streaming->graphicsLayout() : terrain->graphicsLayout(), false);
        createGraphicsPipeline(
            terrainDepthPipeline, streaming ? "terrain_stream.vert.spv" : "terrain.vert.spv",
            streaming ? "terrain_depth.frag.spv" : nullptr,
            streaming ? streaming->graphicsLayout() : terrain->graphicsLayout(), false, DepthMode::Prepass);
        createGraphicsPipeline(
            terrainEqualPipeline, streaming ? "terrain_stream.vert.spv" : "terrain.vert.spv",
            streaming
                ? (materialCaps.bindless ? "terrain_stream_bindless.frag.spv" : "terrain_stream.frag.spv")
                : (materialCaps.bindless ? "terrain_bindless.frag.spv" : "terrain.frag.spv"),
            streaming ? streaming->graphicsLayout() : terrain->graphicsLayout(), false, DepthMode::Equal);
        if (postCaps.taa) {
            const char *fragment = streaming
                                       ? (materialCaps.bindless ? "terrain_stream_bindless_temporal.frag.spv"
                                                                : "terrain_stream_temporal.frag.spv")
                                       : (materialCaps.bindless ? "terrain_bindless_temporal.frag.spv"
                                                                : "terrain_temporal.frag.spv");
            createGraphicsPipeline(temporalTerrainPipeline,
                                   streaming ? "terrain_stream.vert.spv" : "terrain.vert.spv", fragment,
                                   streaming ? streaming->graphicsLayout() : terrain->graphicsLayout(), false,
                                   DepthMode::Write, true);
            createGraphicsPipeline(temporalTerrainEqualPipeline,
                                   streaming ? "terrain_stream.vert.spv" : "terrain.vert.spv", fragment,
                                   streaming ? streaming->graphicsLayout() : terrain->graphicsLayout(), false,
                                   DepthMode::Equal, true);
        }
        VkDescriptorSetLayout skySets[] = {emptySetLayout, lightingSetLayout};
        VkPipelineLayoutCreateInfo skyInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        skyInfo.setLayoutCount = 2;
        skyInfo.pSetLayouts = skySets;
        check(vkCreatePipelineLayout(device, &skyInfo, nullptr, &skyLayout), "Create sky layout");
        createGraphicsPipeline(skyPipeline, "sky.vert.spv", "sky.frag.spv", skyLayout, false, DepthMode::Sky);
        if (postCaps.taa)
            createGraphicsPipeline(skyTemporalPipeline, "sky.vert.spv", "sky_temporal.frag.spv", skyLayout,
                                   false, DepthMode::Sky, true);
        createGraphicsPipeline(outputPipeline, "fullscreen.vert.spv", "output.frag.spv", outputLayout, true);
    }
    void createGraphicsPipeline(VkPipeline &destination, const char *vs, const char *fs,
                                VkPipelineLayout pipelineLayout, bool output,
                                DepthMode depthMode = DepthMode::Write, bool temporal = false) {
        VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
        try {
            vert = shader(vs);
            if (fs)
                frag = shader(fs);
            std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
            for (auto &stage : stages) {
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stage.pName = "main";
            }
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag;
            VkPipelineVertexInputStateCreateInfo input{
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode =
                (output || depthMode == DepthMode::Sky) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = !output;
            depth.depthWriteEnable = !output && depthMode != DepthMode::Equal && depthMode != DepthMode::Sky;
            depth.depthCompareOp = (depthMode == DepthMode::Equal || depthMode == DepthMode::Sky)
                                       ? VK_COMPARE_OP_EQUAL
                                       : VK_COMPARE_OP_GREATER;
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{
                VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            std::array<VkPipelineColorBlendAttachmentState, 3> attachments{attachment, attachment,
                                                                           attachment};
            blend.attachmentCount = depthMode == DepthMode::Prepass ? 0 : temporal ? 3 : 1;
            blend.pAttachments = attachments.data();
            const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = states;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            VkFormat colours[] = {hdrFormat, hdrFormat, postCaps.taa ? VK_FORMAT_R8_UNORM : hdrFormat};
            rendering.colorAttachmentCount = blend.attachmentCount;
            rendering.pColorAttachmentFormats = output ? &colorFormat : colours;
            rendering.depthAttachmentFormat = output ? VK_FORMAT_UNDEFINED : depthFormat;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.pNext = &rendering;
            info.stageCount = fs ? 2 : 1;
            info.pStages = stages.data();
            info.pVertexInputState = &input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pDepthStencilState = &depth;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = pipelineLayout;
            check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &destination),
                  "Create graphics pipeline");
        } catch (...) {
            if (vert)
                vkDestroyShaderModule(device, vert, nullptr);
            if (frag)
                vkDestroyShaderModule(device, frag, nullptr);
            throw;
        }
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
    }

    void initImGui() {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = instance;
        info.PhysicalDevice = physical;
        info.Device = device;
        info.QueueFamily = queueFamily;
        info.Queue = queue;
        info.DescriptorPoolSize = 32;
        info.MinImageCount = minImageCount;
        info.ImageCount = uint32_t(targets.size());
        info.UseDynamicRendering = true;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        auto &rendering = info.PipelineInfoMain.PipelineRenderingCreateInfo;
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        if (uiShaderCode.empty()) {
            std::ifstream file(options.shaderDirectory / "ui.frag.spv", std::ios::binary | std::ios::ate);
            if (!file || file.tellg() <= 0 || file.tellg() % 4)
                throw std::runtime_error("Missing UI shader");
            uiShaderCode.resize(size_t(file.tellg()) / 4);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char *>(uiShaderCode.data()),
                           std::streamsize(uiShaderCode.size() * 4)))
                throw std::runtime_error("Cannot read UI shader");
        }
        info.CustomShaderFragCreateInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.CustomShaderFragCreateInfo.codeSize = uiShaderCode.size() * 4;
        info.CustomShaderFragCreateInfo.pCode = uiShaderCode.data();
        info.CheckVkResultFn = [](VkResult result) { check(result, "ImGui Vulkan operation"); };
        if (!ImGui_ImplVulkan_Init(&info))
            throw std::runtime_error("ImGui Vulkan initialization failed");
        imguiReady = true;
    }

    void invalidateFrameResults() {
        hasRendered = false;
        lastHiZ = false;
        renderingStats.timingsAvailable = false;
        for (auto &frame : frames)
            frame.timed = false;
    }

    void destroySceneTargets() {
        invalidateFrameResults();
        for (auto &frame : frames) {
            if (frame.depthView)
                vkDestroyImageView(device, frame.depthView, nullptr);
            if (frame.depth)
                vkDestroyImage(device, frame.depth, nullptr);
            if (frame.depthMemory)
                vkFreeMemory(device, frame.depthMemory, nullptr);
            frame.depthView = {};
            frame.depth = {};
            frame.depthMemory = {};
            if (frame.hdrView)
                vkDestroyImageView(device, frame.hdrView, nullptr);
            if (frame.hdr)
                vkDestroyImage(device, frame.hdr, nullptr);
            if (frame.hdrMemory)
                vkFreeMemory(device, frame.hdrMemory, nullptr);
            frame.hdrView = {};
            frame.hdr = {};
            frame.hdrMemory = {};
            frame.timed = false;
        }
        renderingStats.hdrBytes = 0;
        renderingStats.timingsAvailable = false;
    }

    void destroyPresentationTargets() {
        for (auto &target : targets) {
            if (target.presentReady)
                vkDestroySemaphore(device, target.presentReady, nullptr);
            if (target.view)
                vkDestroyImageView(device, target.view, nullptr);
            if (!surface && target.image)
                vkDestroyImage(device, target.image, nullptr);
            if (target.memory)
                vkFreeMemory(device, target.memory, nullptr);
        }
        targets.clear();
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    bool recreate() {
        int w = 0, h = 0;
        if (!SDL_GetWindowSizeInPixels(options.window, &w, &h))
            throw std::runtime_error(SDL_GetError());
        if (w <= 0 || h <= 0)
            return false;
        check(vkDeviceWaitIdle(device), "Wait before swapchain recreation");
        if (imguiReady) {
            ImGui_ImplVulkan_Shutdown();
            imguiReady = false;
        }
        const auto previousFormat = colorFormat;
        destroyPresentationTargets();
        createPresentationTargets();
        updateSceneTargets(false);
        if (previousFormat != colorFormat) {
            vkDestroyPipeline(device, outputPipeline, nullptr);
            outputPipeline = {};
            createGraphicsPipeline(outputPipeline, "fullscreen.vert.spv", "output.frag.spv", outputLayout,
                                   true);
        }
        invalidateFrameResults();
        if (options.ui)
            initImGui();
        dirty = false;
        hasRendered = false;
        return true;
    }

    TerrainDraw terrainDraw(const Camera &camera, const SceneSettings &settings,
                            const TerrainPatchDraw &patch) {
        auto *lod = terrain->lod();
        const bool morph = lod && settings.lod && renderStats.boundsReady && patch.level + 1 < lod->levels();
        const float range = renderStats.firstRange * float(patch.step);
        const auto indices =
            lod ? terrain->patchRange(patch.mask) : TerrainIndexRange{0, terrain->indexCount()};
        return TerrainDraw{
            glm::vec4(camera.terrainOffset(terrain->coordinate(patch.slot), options.terrainGrid), 0),
            glm::uvec4(settings.eroded, settings.filteredNormals, options.terrainGrid.cells,
                       (settings.normalView ? 1u : 0u) | (settings.wireframe ? 2u : 0u) |
                           (settings.chunkBorders ? 4u : 0u) | (settings.lodColours ? 8u : 0u)),
            glm::uvec4(patch.x, patch.z, patch.step, lod ? lod->patchCells() : 0),
            glm::vec4(range * 0.75f, range, float(patch.level), morph ? 1.0f : 0.0f),
            glm::uvec4(terrain->normalHalo(patch.level),
                       terrain->normalHalo(std::min(patch.level + 1, terrain->normalLevels() - 1)),
                       options.normalSubdivisions, 0),
            glm::uvec4(indices.first, 0, 0, 0)};
    }

    void prepareTerrainShadows(const Camera &camera, const SceneSettings &settings) {
        shadowTerrainDraws.clear();
        if (!shadows->active() || !settings.terrainVisible)
            return;
        if (streaming) {
            streaming->prepareShadows(frameIndex, camera, settings, *shadows);
            return;
        }
        auto *lod = terrain->lod();
        const auto append = [&](const TerrainPatchDraw &patch, VisibilityBounds bounds) {
            const auto record = terrainDraw(camera, settings, patch);
            const auto indices =
                lod ? terrain->patchRange(patch.mask) : TerrainIndexRange{0, terrain->indexCount()};
            const auto caster =
                shadows->append(bounds, indices.count, indices.first, 0, &record, sizeof(record));
            if (caster != ~0u)
                shadowTerrainDraws.push_back({terrain->descriptor(frameIndex, patch.slot), caster});
        };
        if (lod)
            for (size_t i = 0; i < lod->casterDraws().size(); ++i)
                append(lod->casterDraws()[i], lod->casterBounds()[i]);
        else
            for (uint32_t slot = 0; slot < terrain->chunkCount(); ++slot)
                append({slot, 0, 0, 1, 0, 15}, {});
    }

    bool render(const Camera &inputCamera, const SceneSettings &settings) {
        Camera camera = inputCamera;
        if (options.curvedHorizon) {
            camera.curvature = {settings.atmosphere.planetRadius,
                                float(camera.local.y - settings.atmosphere.datum), 1,
                                float(settings.viewRadius * options.terrainGrid.extent())};
            camera.renderFar = float(std::max(64000.0, options.streaming.horizonMaximumDistance * 1.5));
        }
        // Reinitializing ImGui on resize invalidates the current draw data. Skip it
        // and build a fresh UI next iteration.
        if (dirty && surface) {
            recreate();
            return false;
        }
        validateAtmosphere(settings.atmosphere);
        validateVolumetrics(settings.volumetrics);
        if (atmosphere->needsResize(settings.atmosphere)) {
            check(vkDeviceWaitIdle(device), "Wait for atmosphere resource change");
            atmosphere->resize(settings.atmosphere);
            hasRendered = false;
        }
        validateGtao(settings.gtao);
        if (uint32_t(settings.antialiasing) > uint32_t(AntialiasingMode::Taa))
            throw std::invalid_argument("Invalid AA mode");
        validatePost(settings.taa, settings.bloom);
        if (sceneDirty)
            updateSceneTargets(true);
        if (volumetrics->needsResize(sceneExtent.width, sceneExtent.height, settings.volumetrics)) {
            check(vkDeviceWaitIdle(device), "Wait for explicit volumetric reconfiguration");
            volumetrics->resize(sceneExtent.width, sceneExtent.height, settings.volumetrics);
            post->reset(TemporalReset::Settings);
            hasRendered = false;
        }
        if (gtao->needsResize(sceneExtent.width, sceneExtent.height, settings.gtao.mode)) {
            check(vkDeviceWaitIdle(device), "Wait for explicit GTAO mode change");
            gtaoMode = settings.gtao.mode;
            gtao->resize(sceneExtent.width, sceneExtent.height, gtaoMode);
            hasRendered = false;
        }
        const bool spatialResize =
            spatialAa->needsResize(sceneExtent.width, sceneExtent.height, settings.antialiasing);
        const bool temporalResize =
            post->needsResize(sceneExtent.width, sceneExtent.height,
                              settings.antialiasing == AntialiasingMode::Taa, settings.bloom.intensity > 0);
        if (spatialResize || temporalResize) {
            check(vkDeviceWaitIdle(device), "Wait for AA/post-process resource change");
            // Release inactive temporal images before allocating the spatial mode.
            if (temporalResize)
                post->resize(sceneExtent.width, sceneExtent.height,
                             settings.antialiasing == AntialiasingMode::Taa, settings.bloom.intensity > 0);
            if (spatialResize)
                spatialAa->resize(sceneExtent.width, sceneExtent.height, settings.antialiasing);
            hasRendered = false;
        }
        auto &frame = frames[frameIndex];
        check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, forever), "Wait for frame fence");
        if (settings.terrainVisible)
            terrainCamera = camera;
        if (streaming)
            streaming->beginFrame(frameIndex, terrainCamera, settings);
        else
            terrain->consumeBounds(frameIndex);
        const auto aoView = gtao->begin(frameIndex, settings.gtao, settings.wireframe);
        VkDescriptorImageInfo aoImage{VK_NULL_HANDLE, aoView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet aoWrites[2]{};
        for (auto &w : aoWrites) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &aoImage;
        }
        aoWrites[0].dstSet = frame.lightingSet;
        aoWrites[0].dstBinding = 9;
        aoWrites[1].dstSet = frame.outputSet;
        aoWrites[1].dstBinding = 1;
        vkUpdateDescriptorSets(device, 2, aoWrites, 0, nullptr);
        shadows->begin(
            frameIndex, camera, options.terrainGrid, float(sceneExtent.width) / sceneExtent.height,
            settings.lighting.sunDirection, settings.shadows,
            settings.wireframe || !settings.lighting.sunEnabled,
            streaming ? float(std::max(1, streaming->stats().effectiveRadius) * options.terrainGrid.extent())
                      : 8192.f);
        updateLighting(camera, settings, frame);
        const bool postDiagnostic = frame.diagnostic || settings.gtao.diagnostic != GtaoDiagnostic::Shaded ||
                                    settings.shadows.diagnostic != 0;
        const bool atmosphereChanged = atmosphere->historyChanged(settings.atmosphere, postDiagnostic);
        if (atmosphereChanged || volumetrics->historyChanged(settings.volumetrics))
            post->reset(TemporalReset::Settings);
        post->begin(frameIndex, camera, options.terrainGrid, settings.antialiasing == AntialiasingMode::Taa,
                    settings.taa, settings.bloom, frame.preExposure,
                    postDiagnostic || settings.volumetrics.diagnostic != 0 ||
                        settings.atmosphere.diagnostic != 0);
        spatialAa->begin(frameIndex,
                         postDiagnostic || settings.volumetrics.diagnostic != 0 ||
                             settings.atmosphere.diagnostic != 0,
                         post->stats());
        volumetrics->begin(frameIndex, camera, options.terrainGrid, settings.volumetrics, settings.lighting,
                           clusters->lights, settings.localLights, post->view(), frame.preExposure,
                           postDiagnostic || settings.atmosphere.diagnostic != 0, atmosphereChanged,
                           frame.lightingSet, frame.hdrView, frame.depthView,
                           glm::vec3(settings.background[0], settings.background[1], settings.background[2]));
        atmosphere->begin(frameIndex, camera, settings.atmosphere, settings.lighting, post->view(),
                          frame.preExposure, postDiagnostic,
                          float((streaming ? streaming->stats().outerEffectiveRadius : settings.viewRadius) *
                                options.terrainGrid.extent()),
                          frame.lightingSet, frame.hdrView, frame.depthView,
                          volumetrics->active() ? settings.volumetrics.distance : 0);
        post->bindLighting(frame.lightingSet);
        static_cast<LightingUniform *>(frame.lightingMapped)->viewProjection = post->view().raster;
        materials->prepare(frameIndex, camera, options.terrainGrid, materialTerrainSettings,
                           settings.terrainMaterial, settings.terrainVisible,
                           options.vegetation.streaming.enabled ? &settings.vegetation : nullptr);
        clusters->prepare(frameIndex, camera, options.terrainGrid, frame.lightingSet, settings.localLights,
                          settings.clusterDiagnostic);
        meshes->prepare(frameIndex, camera, options.terrainGrid, post->previousCamera(),
                        post->stats().historyValid, post->stats().taa);
        if (vegetation)
            vegetation->prepare(frameIndex, camera, options.terrainGrid, post->view(), settings.vegetation);
        if (streaming)
            streaming->vegetationDemandRadius(vegetation ? vegetation->streamingRadius() : 0);
        uint32_t imageIndex = frameIndex;
        if (surface) {
            const auto acquired = vkAcquireNextImageKHR(device, swapchain, forever, frame.acquired,
                                                        VK_NULL_HANDLE, &imageIndex);
            if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
                dirty = true;
                return false;
            }
            if (acquired == VK_SUBOPTIMAL_KHR)
                dirty = true;
            else
                check(acquired, "Acquire swapchain image");
        }
        auto &target = targets[imageIndex];
        collectRenderTimings(frame);
        check(vkResetCommandBuffer(frame.commands, 0), "Reset command buffer");
        const auto cmd = frame.commands;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &begin), "Begin frame commands");

        if (renderQueries) {
            vkCmdResetQueryPool(cmd, renderQueries, frameIndex * 4, 4);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, renderQueries, frameIndex * 4);
        }
        materials->recordUploads(cmd);
        meshes->uploads(cmd, frameIndex);
        const uint32_t generated = streaming ? 0 : terrain->record(cmd, frameIndex);

        if (renderQueries) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, renderQueries,
                                 frameIndex * 4 + 1);
        }
        // Discard previous contents, ordering writes after previous reads of this
        // slot.
        imageBarrier(cmd, frame.hdr, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        imageBarrier(
            cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = frame.hdrView;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::copy_n(settings.background, 3, color.clearValue.color.float32);
        color.clearValue.color.float32[3] = 1.0f;
        post->beginAttachments(cmd);
        const auto vp = post->view().raster;
        const auto cullVp =
            post->stats().taa
                ? conservativeProjection(post->view().current, {sceneExtent.width, sceneExtent.height})
                : vp;
        visibility->begin(frameIndex, settings.visibility, settings.wireframe, settings.frustumCulling, vp);
        renderingStats.terrainSelectionMilliseconds = renderingStats.terrainPreparationMilliseconds = 0;
        if (settings.terrainVisible) {
            if (!settings.freezeLod || !observerInitialized) {
                lodObserver = camera;
                observerInitialized = true;
            }
            const auto observer =
                camera.terrainOffset(lodObserver.chunk, options.terrainGrid) + glm::vec3(lodObserver.local);
            if (streaming) {
                const auto start = std::chrono::steady_clock::now();
                streaming->prepareDraw(frameIndex, camera, lodObserver, cullVp, settings, sceneExtent.height);
                renderStats = streaming->renderStats();
                const auto selected = std::chrono::steady_clock::now();
                renderingStats.terrainSelectionMilliseconds =
                    std::chrono::duration<double, std::milli>(selected - start).count();
                streaming->prepareVisibility(*visibility, frameIndex, camera, lodObserver, settings,
                                             post.get());
                renderingStats.terrainPreparationMilliseconds =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - selected)
                        .count();
            } else {
                terrain->setView(frameIndex, vp, observer);
                auto *lod = terrain->lod();
                if (lod) {
                    std::array<ChunkCoord, maxTerrainChunks> chunks{};
                    for (uint32_t i = 0; i < terrain->chunkCount(); ++i)
                        chunks[i] = terrain->coordinate(i);
                    lod->collectCasters(shadows->active());
                    lod->select(chunks, terrain->chunkCount(), camera, lodObserver, cullVp, settings.lod,
                                settings.frustumCulling, terrain->boundsReady());
                    renderStats = lod->stats();
                } else {
                    renderStats = {};
                    renderStats.selected = terrain->chunkCount();
                    renderStats.triangles = uint64_t(terrain->indexCount() / 3) * terrain->chunkCount();
                }
            }
        } else
            renderStats = {};

        terrainHistory->begin(post->stats().historyValid);
        preparedTerrain.clear();
        if (settings.terrainVisible && !streaming) {
            auto *lod = terrain->lod();
            uint32_t order = 0;
            const auto preparePatch = [&](const TerrainPatchDraw &patch) {
                const auto indices =
                    lod ? terrain->patchRange(patch.mask) : TerrainIndexRange{0, terrain->indexCount()};
                const auto draw = terrainDraw(camera, settings, patch);
                PreparedTerrain record{
                    draw,          patch,         lod ? lod->drawBounds()[order] : VisibilityBounds{},
                    indices.count, indices.first, order++};
                preparedTerrain.push_back(record);
            };
            if (lod)
                for (const auto &patch : lod->draws())
                    preparePatch(patch);
            else
                for (uint32_t slot = 0; slot < terrain->chunkCount(); ++slot)
                    preparePatch({slot, 0, 0, 1, 0, 15});
            std::sort(preparedTerrain.begin(), preparedTerrain.end(),
                      [](const PreparedTerrain &a, const PreparedTerrain &b) {
                          if (a.patch.slot != b.patch.slot)
                              return a.patch.slot < b.patch.slot;
                          if (a.patch.level != b.patch.level)
                              return a.patch.level < b.patch.level;
                          return a.order < b.order;
                      });
            uint32_t previousSlot = ~0u, previousLevel = ~0u;
            uint32_t temporalIndex = 0;
            for (auto &r : preparedTerrain) {
                if (post->stats().taa) {
                    TemporalTerrainRecord now{};
                    std::memcpy(now.draw.data(), &r.data, sizeof(r.data));
                    now.observer = glm::vec4(camera.terrainOffset(lodObserver.chunk, options.terrainGrid) +
                                                 glm::vec3(lodObserver.local),
                                             0);
                    now.valid.x = 1;
                    auto coord = terrain->coordinate(r.patch.slot);
                    TemporalTerrainKey key;
                    key.words = {uint64_t(coord.x),
                                 uint64_t(coord.z),
                                 terrain->contentRevision(r.patch.slot) +
                                     ((generated & (1u << r.patch.slot)) ? 1 : 0),
                                 r.patch.slot,
                                 (uint64_t(r.patch.x) << 32) | r.patch.z,
                                 (uint64_t(r.patch.step) << 32) | r.patch.mask,
                                 uint64_t(settings.eroded),
                                 uint64_t(r.patch.level)};
                    post->terrainRecord(temporalIndex, terrainHistory->append(key, now));
                }
                ++temporalIndex;
                r.batch = visibility->append(r.bounds, r.count, r.first, 0, &r.data, sizeof(r.data),
                                             r.patch.slot != previousSlot || r.patch.level != previousLevel);
                previousSlot = r.patch.slot;
                previousLevel = r.patch.level;
            }
        }
        meshes->prepareVisibility(frameIndex, *visibility);
        const auto shadowCollectionStart = std::chrono::steady_clock::now();
        prepareTerrainShadows(camera, settings);
        meshes->prepareShadows(frameIndex, *shadows);
        const auto vegetationPreparationStart = std::chrono::steady_clock::now();
        if (streaming) {
            streaming->vegetationReservation(0);
            if (options.vegetation.streaming.enabled && settings.vegetation.enabled &&
                settings.terrainVisible)
                streaming->prepareVegetation(camera, settings.eroded);
        }
        if (vegetation)
            vegetation->prepareStream(settings.terrainVisible ? streaming.get() : nullptr,
                                      settings.vegetation,
                                      std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - vegetationPreparationStart)
                                          .count());
        if (streaming)
            streaming->record(cmd, frameIndex);
        if (vegetation) {
            vegetation->budgetStream(streaming ? streaming->vegetationRemaining() : 0);
            vegetation->bounds(*shadows);
        }
        shadows->prepare(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                   shadowCollectionStart)
                             .count());
        shadows->writeLighting(frame.lightingSet);
        // Finalize all borrowed terrain descriptors before any command binds
        // this lighting set, including clustered-light compute.
        clusters->record(cmd, frameIndex, frame.lightingSet);
        if (vegetation) {
            if (settings.vegetation.occlusion && visibilityCaps.hiZ && !settings.wireframe)
                vegetation->preparePyramid(visibility->pyramidView());
            vegetation->updateAndSelectCasters(cmd, shadows->uniform(),
                                               shadows->active() ? shadows->cascadeCount() : 0);
        }
        clusters->timestamp(cmd, frameIndex, 2);
        if (shadows->active()) {
            for (uint32_t cascade = 0; cascade < shadows->cascadeCount(); ++cascade) {
                shadows->beginRaster(cmd, cascade);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTerrainPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTerrainLayout, 1, 1,
                                        &frame.lightingSet, 0, nullptr);
                if (streaming)
                    streaming->drawShadows(cmd, shadowTerrainLayout, *shadows);
                else {
                    vkCmdBindIndexBuffer(cmd, terrain->shadowIndices(), 0, VK_INDEX_TYPE_UINT32);
                    shadows->bind(cmd, shadowTerrainLayout);
                    for (size_t i = 0; i < shadowTerrainDraws.size();) {
                        const auto &draw = shadowTerrainDraws[i];
                        size_t end = i + 1;
                        while (end < shadowTerrainDraws.size() && shadowTerrainDraws[end].set == draw.set &&
                               shadowTerrainDraws[end].caster == draw.caster + end - i)
                            ++end;
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowTerrainLayout, 0,
                                                1, &draw.set, 0, nullptr);
                        shadows->drawRange(cmd, draw.caster, uint32_t(end - i));
                        i = end;
                    }
                }
                meshes->drawShadows(cmd, frameIndex, frame.lightingSet, *shadows);
                if (vegetation)
                    vegetation->draw(cmd, frame.lightingSet, 4, cascade + 1);
                shadows->endRaster(cmd);
            }
            shadows->finish(cmd);
        }
        atmosphere->record(cmd);
        volumetrics->record(cmd);
        visibility->beginGpu(cmd);
        visibility->cull(cmd, false);
        const bool vegetationActive = vegetation && settings.vegetation.enabled &&
                                      (!options.vegetation.streaming.enabled || settings.terrainVisible);
        const bool vegetationOcclusion =
            vegetationActive && settings.vegetation.occlusion && visibilityCaps.hiZ && !settings.wireframe;
        const bool buildPyramid = visibility->hiZ() || vegetationOcclusion;
        const bool separateVegetationDepth = vegetationActive && buildPyramid;
        if (vegetation && !vegetationOcclusion)
            vegetation->selectCamera(cmd);
        const bool depthPrepass = settings.opaqueDepthPrepass ||
                                  (vegetationActive && settings.vegetation.depthPrepass) || buildPyramid ||
                                  gtao->active();
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = frame.depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = (depthPrepass || post->stats().taa || atmosphere->active() || volumetrics->active())
                            ? VK_ATTACHMENT_STORE_OP_STORE
                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.f, 0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, sceneExtent};
        rendering.layerCount = 1;
        auto temporalAttachments = post->attachments();
        std::array<VkRenderingAttachmentInfo, 3> sceneColours{color, temporalAttachments[0],
                                                              temporalAttachments[1]};
        rendering.colorAttachmentCount = depthPrepass ? 0 : post->stats().taa ? 3 : 1;
        rendering.pColorAttachments = depthPrepass ? nullptr : sceneColours.data();
        rendering.pDepthAttachment = &depth;
        if (depthPrepass)
            visibility->stamp(cmd, 2);
        else
            visibility->stamp(cmd, 8);
        vkCmdBeginRendering(cmd, &rendering);
        VkViewport viewport{0, 0, float(sceneExtent.width), float(sceneExtent.height), 0, 1};
        VkRect2D scissor{{0, 0}, sceneExtent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          depthPrepass        ? terrainDepthPipeline
                          : post->stats().taa ? temporalTerrainPipeline
                                              : graphicsPipeline);
        const auto drawTerrain = [&](bool finalPass) {
            if (!settings.terrainVisible)
                return;
            auto layout = streaming ? streaming->graphicsLayout() : terrain->graphicsLayout();
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &frame.lightingSet, 0,
                                    nullptr);
            auto materialSet = materials->set(frameIndex);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &materialSet, 0,
                                    nullptr);
            visibility->bindRecords(cmd, layout);
            if (streaming) {
                streaming->draw(cmd, frameIndex, camera, lodObserver, settings, finalPass, visibility.get());
                return;
            }
            uint32_t previous = ~0u;
            uint32_t temporalIndex = 0;
            for (const auto &r : preparedTerrain) {
                auto recordIndex = temporalIndex++;
                if (visibility->indirect() && previous == r.batch)
                    continue;
                previous = r.batch;
                terrain->bind(cmd, frameIndex, r.patch.slot, r.patch.level);
                auto draw = r.data;
                if (visibility->indirect())
                    draw.options.w |= 0x80000000u;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(draw), &draw);
                if (visibility->indirect())
                    visibility->draw(cmd, r.batch);
                else if (settings.wireframe)
                    vkCmdDraw(cmd, r.count, 1, 0, 0);
                else
                    vkCmdDrawIndexed(cmd, r.count, 1, r.first, 0, recordIndex);
            }
        };
        if (depthPrepass) {
            drawTerrain(false);
            meshes->draw(cmd, frameIndex, frame.lightingSet, 2, visibility.get());
            if (vegetation && !separateVegetationDepth)
                vegetation->draw(cmd, frame.lightingSet, 2);
            vkCmdEndRendering(cmd);
            visibility->stamp(cmd, 3);
            const auto depthStages =
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            if (buildPyramid) {
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, depthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                visibility->pyramid(cmd, frame.depth, frame.depthView);
                if (visibility->hiZ())
                    visibility->cull(cmd, true);
                if (vegetationOcclusion)
                    vegetation->selectCamera(cmd, visibility->pyramidView());
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, depthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
                if (separateVegetationDepth) {
                    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    vkCmdBeginRendering(cmd, &rendering);
                    vegetation->draw(cmd, frame.lightingSet, 2);
                    vkCmdEndRendering(cmd);
                }
            }
            if (gtao->active()) {
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, depthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                gtao->record(cmd, frame.depthView, glm::vec2(post->view().pixels.z, post->view().pixels.w),
                             post->stats().taa ? post->stats().sequence : 0, camera.renderFar);
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, depthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            } else {
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depthStages,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            }
            rendering.colorAttachmentCount = post->stats().taa ? 3 : 1;
            rendering.pColorAttachments = sceneColours.data();
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp =
                (visibility->hiZ() || post->stats().taa || atmosphere->active() || volumetrics->active())
                    ? VK_ATTACHMENT_STORE_OP_STORE
                    : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            visibility->stamp(cmd, 8);
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              post->stats().taa ? temporalTerrainEqualPipeline : terrainEqualPipeline);
        }
        drawTerrain(true);
        meshes->draw(cmd, frameIndex, frame.lightingSet, depthPrepass ? 1 : 0, visibility.get());
        if (vegetation)
            vegetation->draw(cmd, frame.lightingSet, depthPrepass ? 1 : 0);
        visibility->stamp(cmd, 9);
        clusters->timestamp(cmd, frameIndex, 3);
        if (atmosphere->active() || volumetrics->active()) {
            vkCmdEndRendering(cmd);
            if (atmosphere->physical() || volumetrics->active()) {
                imageBarrier(cmd, frame.hdr, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (volumetrics->active()) {
                    atmosphere->composite(cmd, sceneExtent.width, sceneExtent.height, false);
                    volumetrics->composite(cmd, sceneExtent.width, sceneExtent.height);
                } else
                    atmosphere->composite(cmd, sceneExtent.width, sceneExtent.height);
                imageBarrier(cmd, frame.hdr, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
            } else {
                render_detail::barrier(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                       VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            }
            for (auto &a : sceneColours)
                a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            rendering.colorAttachmentCount = post->stats().taa ? 3 : 1;
            rendering.pColorAttachments = sceneColours.data();
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              post->stats().taa ? skyTemporalPipeline : skyPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout, 1, 1, &frame.lightingSet,
                                    0, nullptr);
            vkCmdDraw(cmd, 64 * 32 * 6, 1, 0, 0);
        }
        clusters->timestamp(cmd, frameIndex, 4);
        meshes->draw(cmd, frameIndex, frame.lightingSet, 3);
        clusters->timestamp(cmd, frameIndex, 5);
        vkCmdEndRendering(cmd);
        imageBarrier(cmd, frame.hdr, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (post->stats().taa)
            imageBarrier(cmd, frame.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        post->record(cmd, frame.hdrView, frame.depthView);
        spatialAa->record(cmd, frame.hdrView, post->bloomView(frame.hdrView), post->stats().bloom,
                          settings.bloom.intensity, uint32_t(settings.output.toneMapper));
        const auto spatialResult = spatialAa->result();
        VkDescriptorImageInfo postImages[] = {
            {{},
             spatialResult ? spatialResult : post->resolved(frame.hdrView),
             post->stats().taa ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {{},
             post->bloomView(frame.hdrView),
             post->stats().bloom ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (uint32_t i = 0; i < 2; ++i) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = frame.outputSet;
            w.dstBinding = i ? 3 : 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &postImages[i];
            vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        }
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        if (renderQueries)
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, renderQueries,
                                 frameIndex * 4 + 2);
        color.imageView = target.view;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        rendering.pDepthAttachment = nullptr;
        rendering.renderArea = {{0, 0}, extent};
        viewport = {0, 0, float(extent.width), float(extent.height), 0, 1};
        scissor = {{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, outputLayout, 0, 1, &frame.outputSet, 0,
                                nullptr);
        uint32_t outputConstants[] = {frame.diagnostic ? 1u : 0u,
                                      spatialResult ? 2u : uint32_t(settings.output.toneMapper),
                                      gtao->active() ? uint32_t(settings.gtao.diagnostic) : 0u,
                                      extent.width,
                                      extent.height,
                                      0};
        const float bloomIntensity = post->stats().bloom && !spatialResult ? settings.bloom.intensity : 0;
        std::memcpy(&outputConstants[5], &bloomIntensity, 4);
        vkCmdPushConstants(cmd, outputLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(outputConstants),
                           outputConstants);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        if (imguiReady)
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRendering(cmd);
        if (vegetation)
            vegetation->finish(cmd);
        if (renderQueries) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, renderQueries,
                                 frameIndex * 4 + 3);
            frame.timed = true;
        }
        if (surface)
            imageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        check(vkEndCommandBuffer(cmd), "End frame commands");

        VkCommandBufferSubmitInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commands.commandBuffer = cmd;
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = frame.acquired;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = target.presentReady;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commands;
        if (surface) {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
            submit.signalSemaphoreInfoCount = 1;
            submit.pSignalSemaphoreInfos = &signal;
        }
        // Reset only once we know a submission will happen (out-of-date acquisition
        // must leave it signaled).
        check(vkResetFences(device, 1, &frame.fence), "Reset frame fence");
        check(vkQueueSubmit2(queue, 1, &submit, frame.fence), "Submit frame");
        atmosphere->submitted();
        volumetrics->submitted();
        post->submitted();
        meshes->submitted();
        if (vegetation)
            vegetation->submitted();
        terrainHistory->submitted();
        renderingStats.terrainFinalizeMilliseconds = 0;
        if (streaming) {
            const auto start = std::chrono::steady_clock::now();
            streaming->submitted(frameIndex);
            renderingStats.terrainFinalizeMilliseconds =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        } else
            terrain->submitted(generated);
        if (surface) {
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &target.presentReady;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &imageIndex;
            const auto result = vkQueuePresentKHR(queue, &present);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                dirty = true;
            else
                check(result, "Present frame");
        }
        lastTarget = imageIndex;
        lastFrame = frameIndex;
        hasRendered = true;
        lastHiZ = visibility->hiZ();
        frameIndex = (frameIndex + 1) % framesInFlight;
        return true;
    }

    void capture(const std::filesystem::path &path) {
        if (surface || !hasRendered)
            throw std::runtime_error("Capture requires a rendered offscreen frame");
        check(vkDeviceWaitIdle(device), "Wait before capture");
        Buffer readback;
        const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
        try {
            createBuffer(readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            const auto cmd = beginImmediate();
            const auto image = targets[lastTarget].image;
            imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1,
                                   &copy);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
            imageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            endImmediate(cmd);
            void *mapped = nullptr;
            check(vkMapMemory(device, readback.memory, 0, bytes, 0, &mapped), "Map screenshot");
            std::vector<char> rgb(size_t(extent.width) * extent.height * 3);
            const auto *rgba = static_cast<const char *>(mapped);
            for (size_t pixel = 0; pixel < rgb.size() / 3; ++pixel)
                std::copy_n(rgba + pixel * 4, 3, rgb.data() + pixel * 3);
            vkUnmapMemory(device, readback.memory);
            std::ofstream output(path, std::ios::binary);
            output << "P6\n" << extent.width << ' ' << extent.height << "\n255\n";
            output.write(rgb.data(), std::streamsize(rgb.size()));
            if (!output)
                throw std::runtime_error("Cannot write capture: " + path.string());
            std::cout << "Captured " << path << '\n';
        } catch (...) {
            destroyBuffer(readback);
            throw;
        }
        destroyBuffer(readback);
    }

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (imguiReady)
                ImGui_ImplVulkan_Shutdown();
            if (temporalTerrainPipeline)
                vkDestroyPipeline(device, temporalTerrainPipeline, nullptr);
            if (temporalTerrainEqualPipeline)
                vkDestroyPipeline(device, temporalTerrainEqualPipeline, nullptr);
            if (graphicsPipeline)
                vkDestroyPipeline(device, graphicsPipeline, nullptr);
            if (shadowTerrainPipeline)
                vkDestroyPipeline(device, shadowTerrainPipeline, nullptr);
            if (shadowTerrainLayout)
                vkDestroyPipelineLayout(device, shadowTerrainLayout, nullptr);
            if (terrainDepthPipeline)
                vkDestroyPipeline(device, terrainDepthPipeline, nullptr);
            if (terrainEqualPipeline)
                vkDestroyPipeline(device, terrainEqualPipeline, nullptr);
            if (outputPipeline)
                vkDestroyPipeline(device, outputPipeline, nullptr);
            destroyPresentationTargets();
            destroySceneTargets();
            streaming.reset();
            terrain.reset();
            vegetation.reset();
            meshes.reset();
            visibility.reset();
            volumetrics.reset();
            atmosphere.reset();
            if (skyPipeline)
                vkDestroyPipeline(device, skyPipeline, nullptr);
            if (skyTemporalPipeline)
                vkDestroyPipeline(device, skyTemporalPipeline, nullptr);
            if (skyLayout)
                vkDestroyPipelineLayout(device, skyLayout, nullptr);
            gtao.reset();
            spatialAa.reset();
            post.reset();
            shadows.reset();
            clusters.reset();
            destroyLighting();
            materials.reset();
            for (auto &frame : frames) {
                if (frame.fence)
                    vkDestroyFence(device, frame.fence, nullptr);
                if (frame.acquired)
                    vkDestroySemaphore(device, frame.acquired, nullptr);
            }
            if (commandPool)
                vkDestroyCommandPool(device, commandPool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (messenger) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy)
                destroy(instance, messenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

Renderer::Renderer(const RendererOptions &options) : impl_(std::make_unique<Impl>()) { impl_->init(options); }
Renderer::~Renderer() = default;
LightHandle Renderer::createLight(const LocalLight &l) {
    validate(l);
    return impl_->clusters->lights.create(l);
}
bool Renderer::updateLight(LightHandle h, const LocalLight &l) {
    validate(l);
    return impl_->clusters->lights.update(h, l);
}
bool Renderer::removeLight(LightHandle h) { return impl_->clusters->lights.remove(h); }
const VegetationStats &Renderer::vegetationStats() const {
    static const VegetationStats empty;
    return impl_->vegetation ? impl_->vegetation->stats() : empty;
}
VegetationSnapshot Renderer::readVegetationSnapshot() {
    if (!impl_->vegetation)
        return {};
    waitIdle();
    auto cmd = impl_->beginImmediate();
    impl_->vegetation->copySnapshot(cmd);
    impl_->endImmediate(cmd);
    return impl_->vegetation->snapshot();
}
MeshHandle Renderer::uploadMesh(const MeshData &m) { return impl_->meshes->upload(m); }
bool Renderer::releaseMesh(MeshHandle h) { return impl_->meshes->release(h); }
InstanceHandle Renderer::createInstance(const MeshInstance &i) {
    validate(i);
    return impl_->meshes->instances.create(i);
}
bool Renderer::updateInstance(InstanceHandle h, const MeshInstance &i) {
    validate(i);
    return impl_->meshes->instances.update(h, i);
}
bool Renderer::removeInstance(InstanceHandle h) { return impl_->meshes->instances.remove(h); }
TextureHandle Renderer::uploadTexture(const TextureData &t) { return impl_->materials->uploadTexture(t); }
bool Renderer::releaseTexture(TextureHandle h) { return impl_->materials->releaseTexture(h); }
MaterialHandle Renderer::createMaterial(const MeshMaterial &m) { return impl_->materials->createMaterial(m); }
bool Renderer::releaseMaterial(MaterialHandle h) { return impl_->materials->releaseMaterial(h); }
const SceneStats &Renderer::sceneStats() const {
    auto &s = impl_->sceneStatistics;
    s = impl_->clusters->stats();
    s.geometryBytes = impl_->meshes->bytes();
    s.instances = uint32_t(impl_->meshes->instances.values().size());
    s.skippedMeshes = impl_->meshes->skipped();
    return s;
}
const MaterialStats &Renderer::materialStats() const { return impl_->materials->stats(); }
bool Renderer::reloadMaterials(const std::filesystem::path &p) {
    return impl_->materials->load(p, impl_->options.forceRgbaMaterials);
}
void Renderer::resize() { impl_->dirty = true; }
void Renderer::setRenderHeight(uint32_t height) {
    if (height > 16384)
        throw std::invalid_argument("Render height must be in 0..16384");
    if (height != impl_->options.renderHeight) {
        impl_->options.renderHeight = height;
        impl_->sceneDirty = true;
    }
}
VisibilityPyramidSnapshot Renderer::readVisibilityPyramid() { return impl_->readVisibilityPyramid(); }
const GtaoStats &Renderer::gtaoStats() const { return impl_->gtao->stats(); }
GtaoSnapshot Renderer::readGtaoSnapshot() { return impl_->readGtaoSnapshot(); }
const VisibilityStats &Renderer::visibilityStats() const { return impl_->visibility->stats(); }
void Renderer::resizeOffscreen(uint32_t w, uint32_t h) { impl_->resizeOffscreen(w, h); }
HdrSnapshot Renderer::readHdrSnapshot() { return impl_->readHdrSnapshot(); }
const RenderingStats &Renderer::renderingStats() const { return impl_->renderingStats; }
ShadowSnapshot Renderer::readShadowSnapshot() { return impl_->readShadowSnapshot(); }
const ShadowStats &Renderer::shadowStats() const { return impl_->shadows->stats(); }
bool Renderer::render(const Camera &camera, const SceneSettings &settings) {
    return impl_->render(camera, settings);
}
void Renderer::setTerrainLodObserver(const Camera &camera) {
    impl_->lodObserver = camera;
    impl_->observerInitialized = true;
}
TerrainLodReviewState Renderer::terrainLodReviewState() const {
    TerrainLodReviewState state{impl_->lodObserver, 1, 0, impl_->observerInitialized};
    if (impl_->streaming) {
        impl_->streaming->reviewQuality(state.qualityScale, state.focalPixels);
        state.patches = impl_->streaming->reviewPatches();
    }
    return state;
}
void Renderer::setTerrainLodReviewState(const TerrainLodReviewState &state) {
    if (!std::isfinite(state.qualityScale) || state.qualityScale < 1 ||
        state.qualityScale > terrainLodMaximumScale || !std::isfinite(state.focalPixels) ||
        state.focalPixels < 0)
        throw std::invalid_argument("Invalid review LOD range");
    if (impl_->streaming) {
        // Partition validation can fail; leave the observer/range unchanged until
        // it succeeds.
        impl_->streaming->restoreReviewPatches(state.patches);
        impl_->streaming->restoreReviewQuality(state.qualityScale, state.focalPixels);
    }
    impl_->lodObserver = state.observer;
    impl_->observerInitialized = state.initialized;
    resetTemporalHistory();
}
void Renderer::setTerrainGrid(TerrainGrid grid) {
    impl_->post->reset(TemporalReset::Settings);
    impl_->volumetrics->reset();
    if (impl_->streaming)
        throw std::invalid_argument("Streaming footprint is selected at startup");
    impl_->terrain->setGrid(grid);
    impl_->options.terrainGrid = grid;
    impl_->observerInitialized = false;
}
std::vector<uint32_t> Renderer::readTerrainIndices() { return impl_->terrain->readIndices(); }
void Renderer::requestTerrain(const TerrainRequest &request) {
    impl_->volumetrics->reset();
    if (impl_->streaming)
        impl_->streaming->settings(request.settings);
    else
        impl_->terrain->request(request);
    impl_->materialTerrainSettings = request.settings;
}
uint64_t Renderer::terrainGenerationCount() const {
    return impl_->streaming ? impl_->streaming->stats().generations : impl_->terrain->generationCount();
}
std::vector<TerrainSample> Renderer::readTerrainSnapshot(uint32_t slot) {
    return impl_->terrain->readback(slot);
}
std::vector<glm::vec4> Renderer::readTerrainNormals(uint32_t slot, uint32_t level) {
    return impl_->terrain->readNormals(slot, level);
}
void Renderer::requestTerrainBatch(const TerrainBatchRequest &request) {
    impl_->volumetrics->reset();
    if (impl_->streaming)
        impl_->streaming->settings(request.settings);
    else
        impl_->terrain->request(request);
    impl_->materialTerrainSettings = request.settings;
}
uint32_t Renderer::terrainChunkCount() const {
    return impl_->streaming ? impl_->streaming->stats().coverage : impl_->terrain->chunkCount();
}
const TerrainRenderStats &Renderer::terrainRenderStats() const { return impl_->renderStats; }
std::vector<TerrainPatchDraw> Renderer::readTerrainPatches() const {
    if (impl_->streaming)
        return impl_->streaming->patches();
    const auto *lod = impl_->terrain->lod();
    return lod ? lod->draws() : std::vector<TerrainPatchDraw>{};
}
uint64_t Renderer::terrainAllocatedBytes() const {
    return impl_->streaming ? impl_->streaming->stats().gpuBytes : impl_->terrain->allocatedBytes();
}
std::vector<TerrainGeographySample>
Renderer::sampleTerrainGeography(TerrainGrid grid, const TerrainRequest &r, bool canonical) {
    if (impl_->streaming)
        throw std::invalid_argument("Geography diagnostics require a non-streamed renderer");
    return impl_->terrain->sampleTerrainGeography(grid, r, canonical);
}
std::vector<glm::vec2> Renderer::sampleTerrainHeightLevel(TerrainGrid grid, const TerrainRequest &request,
                                                          uint32_t level, bool fixture) {
    if (impl_->streaming)
        throw std::invalid_argument("Height probes require non-streamed renderer");
    return impl_->terrain->sampleTerrainHeightLevel(grid, request, level, fixture);
}
std::vector<TerrainSample> Renderer::sampleTerrainNormalFixture(TerrainGrid grid, uint32_t level,
                                                                uint32_t tileLevel) {
    if (impl_->streaming)
        throw std::invalid_argument("Normal fixture is non-streamed only");
    return impl_->terrain->sampleTerrainNormalFixture(grid, level, tileLevel);
}
std::vector<TerrainSample> Renderer::normalsForFixture(const std::vector<glm::vec2> &h) {
    return impl_->terrain->normalsForFixture(h);
}
std::vector<TerrainSample> Renderer::sampleCurvatureFixture(const std::vector<TerrainSample> &p, glm::vec4 c,
                                                            bool reduced) {
    if (impl_->streaming)
        throw std::invalid_argument("Curvature fixture is non-streamed only");
    return impl_->terrain->sampleCurvatureFixture(p, c, reduced);
}
void Renderer::setTerrainSettings(const TerrainSettings &settings) {
    impl_->post->reset(TemporalReset::Settings);
    impl_->volumetrics->reset();
    if (impl_->streaming)
        impl_->streaming->settings(settings);
    else
        impl_->terrain->request(TerrainRequest{{}, settings});
    impl_->materialTerrainSettings = settings;
}
const TerrainStreamStats &Renderer::terrainStreamingStats() const {
    static const TerrainStreamStats empty{};
    return impl_->streaming ? impl_->streaming->stats() : empty;
}
std::vector<glm::vec2> Renderer::readStreamedTileHeights(TerrainTileKey tile, uint32_t geometryLevel) {
    if (!impl_->streaming)
        throw std::invalid_argument("Streaming is disabled");
    return impl_->streaming->readHeights({tile.x, tile.z}, tile.level, geometryLevel);
}
std::vector<glm::vec4> Renderer::readStreamedTileNormals(TerrainTileKey tile, uint32_t level) {
    if (!impl_->streaming)
        throw std::invalid_argument("Streaming is disabled");
    return impl_->streaming->readNormals({tile.x, tile.z}, level, tile.level);
}
TerrainDetailJobSnapshot Renderer::readTerrainDetailJob() const {
    return impl_->streaming ? impl_->streaming->detailJob() : TerrainDetailJobSnapshot{};
}
std::vector<TerrainTileSnapshot> Renderer::readTerrainResidency() const {
    return impl_->streaming ? impl_->streaming->residency() : std::vector<TerrainTileSnapshot>{};
}
std::vector<glm::vec4> Renderer::readTerrainVertexPositions() {
    if (!impl_->streaming)
        throw std::invalid_argument("Streaming is disabled");
    return impl_->streaming->readVertexPositions();
}
std::vector<glm::vec2> Renderer::readStreamedHeights(ChunkCoord coord) {
    if (!impl_->streaming)
        throw std::invalid_argument("Streaming is disabled");
    return impl_->streaming->readHeights(coord);
}
std::vector<glm::vec4> Renderer::readStreamedNormals(ChunkCoord coord, uint32_t level) {
    if (!impl_->streaming)
        throw std::invalid_argument("Streaming is disabled");
    return impl_->streaming->readNormals(coord, level);
}
void Renderer::capture(const std::filesystem::path &path) { impl_->capture(path); }
void Renderer::waitIdle() { check(vkDeviceWaitIdle(impl_->device), "Wait for device idle"); }
const std::string &Renderer::deviceName() const { return impl_->gpuName; }
bool Renderer::validationEnabled() const { return impl_->validation; }
uint32_t Renderer::validationErrors() const { return impl_->errors.load(); }
uint32_t Renderer::width() const { return impl_->extent.width; }
uint32_t Renderer::height() const { return impl_->extent.height; }
uint32_t Renderer::renderWidth() const { return impl_->sceneExtent.width; }
uint32_t Renderer::renderHeight() const { return impl_->sceneExtent.height; }
} // namespace ow

namespace ow {
void Renderer::resetTemporalHistory() {
    impl_->post->reset();
    impl_->volumetrics->reset();
}
bool Renderer::invalidateInstanceHistory(InstanceHandle h) { return impl_->meshes->invalidateHistory(h); }
const PostStats &Renderer::postStats() const { return impl_->post->stats(); }
const SpatialAaStats &Renderer::spatialAaStats() const { return impl_->spatialAa->stats(); }
PostSnapshot Renderer::readPostSnapshot() { return impl_->readPostSnapshot(); }
const AtmosphereStats &Renderer::atmosphereStats() const { return impl_->atmosphere->stats(); }
AtmosphereSnapshot Renderer::readAtmosphereSnapshot() { return impl_->readAtmosphereSnapshot(); }
} // namespace ow

namespace ow {
FogVolumeHandle Renderer::createFogVolume(const FogVolume &v) {
    validate(v);
    auto h = impl_->volumetrics->volumes.create(v);
    if (h.slot != ~0u)
        impl_->volumetrics->edited();
    return h;
}
bool Renderer::updateFogVolume(FogVolumeHandle h, const FogVolume &v) {
    validate(v);
    bool ok = impl_->volumetrics->volumes.update(h, v);
    if (ok)
        impl_->volumetrics->edited();
    return ok;
}
bool Renderer::removeFogVolume(FogVolumeHandle h) {
    bool ok = impl_->volumetrics->volumes.remove(h);
    if (ok)
        impl_->volumetrics->edited();
    return ok;
}
const VolumetricStats &Renderer::volumetricStats() const { return impl_->volumetrics->stats(); }
VolumetricSnapshot Renderer::readVolumetricSnapshot() { return impl_->readVolumetricSnapshot(); }
} // namespace ow

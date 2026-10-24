#include "vulkan_materials.hpp"
#include "vegetation.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace ow {
namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan material resource failure: " + std::to_string(r));
}
uint32_t memoryType(VkPhysicalDevice p, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(p, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("Material memory type unavailable");
}
VkFormat format(TextureFormat f) {
    switch (f) {
    case TextureFormat::Bc5:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureFormat::Bc7:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::Bc7Srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    case TextureFormat::Srgb8:
        return VK_FORMAT_R8G8B8A8_SRGB;
    default:
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
}
struct Buffer {
    VkDevice device{};
    VkBuffer handle{};
    VkDeviceMemory memory{};
    void *mapped = nullptr;
    ~Buffer() {
        if (mapped)
            vkUnmapMemory(device, memory);
        if (handle)
            vkDestroyBuffer(device, handle, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
    void create(VkPhysicalDevice p, VkDevice d, uint64_t size, VkBufferUsageFlags usage) {
        device = d;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage;
        check(vkCreateBuffer(d, &ci, nullptr, &handle));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(d, handle, &req);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex =
            memoryType(p, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(d, &a, nullptr, &memory));
        check(vkBindBufferMemory(d, handle, memory, 0));
        check(vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    }
};
struct Image {
    VkDevice device{};
    VkImage image{};
    VkImageView view{}, view2D{};
    VkDeviceMemory memory{};
    uint64_t bytes = 0;
    uint32_t levels = 1, layers = 1, width = 1, height = 1;
    TextureFormat fmt{};
    bool uploaded = false;
    std::unique_ptr<Buffer> staging;
    ~Image() {
        if (view2D)
            vkDestroyImageView(device, view2D, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
};
struct Palette {
    std::array<std::unique_ptr<Image>, 4> images;
    bool macro = false;
    bool uploaded = false;
    uint64_t bytes = 0;
};
struct MaterialRecord {
    glm::uvec4 maps{0, 1, 2, 0};
    glm::uvec4 macro{3, 0, 0, 0};
    glm::vec4 base{1}, emission{0}, factors{.8f, 0, 1, 1};
    glm::uvec4 meshMaps{0, 1, 2, 0};
    glm::vec4 alpha{0, .5f, 0, 0};
    glm::uvec4 temporal{0};
};
struct MaterialUniform {
    TerrainParameters geography;
    glm::vec4 controls, tint;
    glm::uvec4 options;
    MaterialCoordinates coordinates;
    glm::vec4 blend;
    glm::uvec4 features;
    glm::vec4 habitat, clearing;
};
static_assert(sizeof(MaterialRecord) == 128);
static_assert(sizeof(MaterialUniform) == 640);
static_assert(offsetof(MaterialUniform, coordinates) == 544);
static_assert(offsetof(MaterialUniform, blend) == 576);
static_assert(offsetof(MaterialUniform, features) == 592);
} // namespace
MaterialCapabilities materialCapabilities(VkPhysicalDevice p, bool fixed, bool tri) {
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &v12;
    vkGetPhysicalDeviceFeatures2(p, &f);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    auto &l = props.limits;
    MaterialCapabilities c;
    c.images = 8;
    c.anisotropy = !tri && f.features.samplerAnisotropy;
    c.maxAnisotropy = c.anisotropy ? l.maxSamplerAnisotropy : 1;
    const uint32_t limit = std::min(
        {l.maxPerStageDescriptorSampledImages, l.maxDescriptorSetSampledImages, l.maxPerStageResources});
    c.bindless =
        !fixed && v12.runtimeDescriptorArray && v12.shaderSampledImageArrayNonUniformIndexing && limit >= 96;
    if (c.bindless) {
        c.arrays = 16;
        // Reserve the terrain, clustered lighting, shadow, temporal, atmosphere and
        // volumetric bindings across all sets, including the 16 array-image
        // descriptors.
        c.images = std::min(128u, limit - 80);
    }
    return c;
}
struct VulkanMaterials::Impl {
    VkPhysicalDevice physical;
    VkDevice device;
    MaterialCapabilities caps;
    uint64_t budget;
    MaterialStats stats;
    VkDescriptorSetLayout layout{};
    VkDescriptorPool pool{};
    std::array<VkSampler, 3> samplers{};
    struct Frame {
        VkDescriptorSet set{};
        Buffer uniform, records;
        std::shared_ptr<Palette> palette;
        int sampler = -1;
        uint64_t revision = 0;
        std::array<std::shared_ptr<Image>, 128> images;
    };
    std::array<Frame, 2> frames;
    std::shared_ptr<Palette> fallback, current;
    // At most current + two in-flight versions + fallback. No growing retirement
    // queue.
    std::array<std::weak_ptr<Palette>, 4> versions;
    MaterialSlots<TextureHandle, 16> textureSlots;
    MaterialSlots<MaterialHandle, 64> materialSlots;
    struct MeshImageSlot {
        uint32_t generation = 0;
        bool live = false;
        std::shared_ptr<Image> image;
    };
    std::array<MeshImageSlot, 128> meshImages;
    std::array<MeshMaterial, 64> meshMaterials;
    uint64_t revision = 1;
    std::array<TextureHandle, 4> textureHandles{};
    std::array<MaterialHandle, 4> materialHandles{};
    Impl(VkPhysicalDevice p, VkDevice d, MaterialCapabilities c, uint64_t b)
        : physical(p), device(d), caps(c), budget(b) {}
    ~Impl() {
        for (auto &f : frames)
            f.palette.reset();
        current.reset();
        fallback.reset();
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (layout)
            vkDestroyDescriptorSetLayout(device, layout, nullptr);
        for (auto s : samplers)
            if (s)
                vkDestroySampler(device, s, nullptr);
    }
    uint64_t resident() const {
        uint64_t n = fallback ? fallback->bytes : 0;
        for (const auto &w : versions)
            if (auto p = w.lock())
                n += p->bytes;
        for (const auto &im : meshImages)
            if (im.image)
                n += im.image->bytes;
        return n;
    }
    uint32_t textureIndex(TextureHandle h, uint32_t fallbackSlot) const {
        return h.slot < caps.images && meshImages[h.slot].live &&
                       meshImages[h.slot].generation == h.generation
                   ? h.slot
                   : fallbackSlot;
    }
    void retireImages() {
        for (auto &im : meshImages)
            if (!im.live && im.image && im.image.use_count() == 1)
                im.image.reset();
    }
    std::unique_ptr<Image> image(const TextureData &t, uint64_t available) {
        auto out = std::make_unique<Image>();
        out->device = device;
        out->width = t.width;
        out->height = t.height;
        out->levels = t.levels;
        out->layers = t.layers;
        out->fmt = t.format;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format(t.format);
        ci.extent = {t.width, t.height, 1};
        ci.mipLevels = t.levels;
        ci.arrayLayers = t.layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, ci.format, &properties);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
            throw std::invalid_argument("Material format cannot be linearly sampled/uploaded");
        VkImageFormatProperties limits{};
        check(vkGetPhysicalDeviceImageFormatProperties(physical, ci.format, ci.imageType, ci.tiling, ci.usage,
                                                       ci.flags, &limits));
        if (t.width > limits.maxExtent.width || t.height > limits.maxExtent.height ||
            t.levels > limits.maxMipLevels || t.layers > limits.maxArrayLayers)
            throw std::runtime_error("Material image exceeds device format limits");
        check(vkCreateImage(device, &ci, nullptr, &out->image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, out->image, &req);
        if (req.size > available)
            throw std::runtime_error("Material image budget exhausted");
        out->bytes = req.size;
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = req.size;
        a.memoryTypeIndex = memoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &a, nullptr, &out->memory));
        check(vkBindImageMemory(device, out->image, out->memory, 0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = out->image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        v.format = ci.format;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, t.layers};
        check(vkCreateImageView(device, &v, nullptr, &out->view));
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device, &v, nullptr, &out->view2D));
        out->staging = std::make_unique<Buffer>();
        out->staging->create(physical, device, t.bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(out->staging->mapped, t.bytes.data(), t.bytes.size());
        return out;
    }
    void init() {
        stats.bindless = caps.bindless;
        stats.maxAnisotropy = caps.maxAnisotropy;
        stats.image2DCapacity = caps.images;
        stats.arrayCapacity = caps.arrays;
        // Reserve semantic fallback slots 0=colour, 1=normal, 2=scalars, 3=macro.
        textureSlots.acquire();
        textureSlots.acquire();
        textureSlots.acquire();
        for (auto &h : textureHandles)
            h = textureSlots.acquire();
        for (auto &h : materialHandles)
            h = materialSlots.acquire();
        fallback = std::make_shared<Palette>();
        const std::array<std::array<uint8_t, 4>, 4> colours{
            {{{128, 128, 128, 255}}, {{128, 128, 255, 255}}, {{230, 255, 128, 255}}, {{188, 188, 188, 128}}}};
        for (int i = 0; i < 4; ++i) {
            TextureData t;
            t.layers = 4;
            t.format = (i == 0 || i == 3) ? TextureFormat::Srgb8 : TextureFormat::Rgba8;
            for (int j = 0; j < 4; ++j)
                t.bytes.insert(t.bytes.end(), colours[i].begin(), colours[i].end());
            fallback->images[i] = image(t, budget - fallback->bytes);
            fallback->bytes += fallback->images[i]->bytes;
        }
        current = fallback;
        for (uint32_t i = 0; i < 3; ++i) {
            TextureData t;
            t.format = i == 0 ? TextureFormat::Srgb8 : TextureFormat::Rgba8;
            t.bytes =
                i == 1 ? std::vector<uint8_t>{128, 128, 255, 255} : std::vector<uint8_t>{255, 255, 255, 255};
            meshImages[i].image = image(t, budget - resident());
            meshImages[i].live = true;
            meshImages[i].generation = 1;
        }
        std::array<VkDescriptorSetLayoutBinding, 9> b{};
        uint32_t count = 0;
        auto binding = [&](uint32_t n, VkDescriptorType type, uint32_t size) {
            b[count++] = {n, type, size, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        };
        binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);
        binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1);
        binding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, caps.images);
        if (caps.bindless) {
            binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, caps.arrays);
        } else {
            binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1);
            binding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1);
            binding(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1);
            binding(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1);
        }
        binding(6, VK_DESCRIPTOR_TYPE_SAMPLER, 1);
        binding(8, VK_DESCRIPTOR_TYPE_SAMPLER, 1);
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = count;
        li.pBindings = b.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &layout));
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * (caps.images + caps.arrays)},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 4}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2;
        pi.poolSizeCount = 4;
        pi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pi, nullptr, &pool));
        for (int i = 0; i < 3; ++i) {
            VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            s.magFilter = s.minFilter = VK_FILTER_LINEAR;
            s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            s.maxLod = VK_LOD_CLAMP_NONE;
            s.anisotropyEnable = caps.anisotropy && i > 0;
            s.maxAnisotropy = i == 0 ? 1 : std::min(caps.maxAnisotropy, i == 1 ? 8.f : 16.f);
            check(vkCreateSampler(device, &s, nullptr, &samplers[i]));
        }
        for (auto &f : frames) {
            f.uniform.create(physical, device, sizeof(MaterialUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            f.records.create(physical, device, 64 * sizeof(MaterialRecord),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool = pool;
            a.descriptorSetCount = 1;
            a.pSetLayouts = &layout;
            check(vkAllocateDescriptorSets(device, &a, &f.set));
        }
    }
    void descriptors(Frame &f, int sampler) {
        VkDescriptorBufferInfo buffers[] = {{f.uniform.handle, 0, sizeof(MaterialUniform)},
                                            {f.records.handle, 0, 64 * sizeof(MaterialRecord)}};
        std::array<VkDescriptorImageInfo, 128> images{};
        std::array<VkDescriptorImageInfo, 16> arrays{};
        for (auto &i : images)
            i = {VK_NULL_HANDLE, fallback->images[0]->view2D, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (auto &i : arrays)
            i = {VK_NULL_HANDLE, fallback->images[0]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (int i = 0; i < 4; ++i) {
            arrays[i] = {VK_NULL_HANDLE, fallback->images[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            images[i] = {VK_NULL_HANDLE, fallback->images[i]->view2D,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        for (int i = 0; i < 4; ++i) {
            const auto slot = textureSlots.resolve(textureHandles[i]);
            if (!slot)
                continue;
            arrays[slot] = {VK_NULL_HANDLE, current->images[i]->view,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        for (uint32_t i = 0; i < caps.images; ++i) {
            auto &slot = meshImages[i];
            f.images[i] = (slot.live && slot.image) ? slot.image : meshImages[0].image;
            images[i] = {VK_NULL_HANDLE, f.images[i]->view2D, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        const auto textureIndex = [&](uint32_t semantic) {
            const auto slot = textureSlots.resolve(textureHandles[semantic]);
            return slot ? slot : semantic;
        };
        VkDescriptorImageInfo sample{samplers[sampler], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        VkDescriptorImageInfo macroSample{samplers[0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        std::array<VkWriteDescriptorSet, 9> writes{};
        uint32_t n = 0;
        auto write = [&](uint32_t binding, VkDescriptorType type, uint32_t count,
                         const VkDescriptorImageInfo *im, const VkDescriptorBufferInfo *buf) {
            auto &w = writes[n++];
            w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = f.set;
            w.dstBinding = binding;
            w.descriptorType = type;
            w.descriptorCount = count;
            w.pImageInfo = im;
            w.pBufferInfo = buf;
        };
        write(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, nullptr, &buffers[0]);
        write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, nullptr, &buffers[1]);
        write(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, caps.images, images.data(), nullptr);
        if (caps.bindless) {
            write(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, caps.arrays, arrays.data(), nullptr);
        } else
            for (uint32_t i = 0; i < 4; ++i)
                write(i == 3 ? 7 : 3 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, &arrays[textureIndex(i)],
                      nullptr);
        write(6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, &sample, nullptr);
        write(8, VK_DESCRIPTOR_TYPE_SAMPLER, 1, &macroSample, nullptr);
        vkUpdateDescriptorSets(device, n, writes.data(), 0, nullptr);
        auto *records = static_cast<MaterialRecord *>(f.records.mapped);
        for (int i = 0; i < 64; ++i)
            records[i] = MaterialRecord{};
        for (uint32_t i = 0; i < 4; ++i) {
            const auto slot = materialSlots.resolve(materialHandles[i]);
            if (slot) {
                records[slot].maps = {textureIndex(0), textureIndex(1), textureIndex(2), i};
                records[slot].macro = {textureIndex(3), 0, 0, 0};
            }
        }
        for (uint32_t i = 0; i < 64; ++i) {
            const auto &m = meshMaterials[i];
            auto &r = records[i];
            r.base = m.baseColour;
            r.emission = glm::vec4(m.emissive, 0);
            r.factors = {m.roughness, m.metallic, m.ao, m.normalStrength};
            r.meshMaps = {this->textureIndex(m.colourTexture, 0), this->textureIndex(m.normalTexture, 1),
                          this->textureIndex(m.ormTexture, 2), this->textureIndex(m.emissiveTexture, 0)};
            r.alpha = {float(m.alpha), m.cutoff, m.doubleSided ? 1.f : 0.f, m.unlit ? 1.f : 0.f};
            r.temporal.x = m.thinGeometry && m.alpha != AlphaMode::Blend ? 1u : 0u;
        }
        f.revision = revision;
        f.palette = current;
        f.sampler = sampler;
    }
};
VulkanMaterials::VulkanMaterials(VkPhysicalDevice p, VkDevice d, MaterialCapabilities c, uint64_t b)
    : impl_(std::make_unique<Impl>(p, d, c, b)) {
    impl_->init();
}
VulkanMaterials::~VulkanMaterials() = default;
VkDescriptorSetLayout VulkanMaterials::layout() const { return impl_->layout; }
VkDescriptorSet VulkanMaterials::set(uint32_t f) const { return impl_->frames.at(f).set; }
const MaterialStats &VulkanMaterials::stats() const { return impl_->stats; }
bool VulkanMaterials::load(const std::filesystem::path &root, bool rgba) {
    auto &s = *impl_;
    try {
        auto free = std::find_if(s.versions.begin(), s.versions.end(), [](auto &w) { return w.expired(); });
        if (free == s.versions.end())
            throw std::runtime_error("Material versions exhausted");
        std::array<TextureData, 4> data;
        const char *names[] = {"colour", "normal", "scalar", "macro"};
        const bool hasMacro =
            std::filesystem::exists(root / "macro.dds") || std::filesystem::exists(root / "macro_rgba.dds");
        uint64_t total = 0;
        for (int i = 0; i < 4; ++i) {
            if (i == 3 && !hasMacro) {
                data[i].layers = 4;
                data[i].format = TextureFormat::Srgb8;
                for (int j = 0; j < 4; ++j)
                    data[i].bytes.insert(data[i].bytes.end(), {188, 188, 188, 128});
                total += data[i].bytes.size();
                continue;
            }
            bool raw = rgba;
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(s.physical,
                                                (i == 0 || i == 3) ? VK_FORMAT_BC7_SRGB_BLOCK
                                                : i == 1           ? VK_FORMAT_BC5_UNORM_BLOCK
                                                                   : VK_FORMAT_BC7_UNORM_BLOCK,
                                                &props);
            const auto flags = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                               VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                               VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            raw |= (props.optimalTilingFeatures & flags) != flags;
            data[i] = readTexture(root / (std::string(names[i]) + (raw ? "_rgba.dds" : ".dds")));
            const auto expected = raw ? ((i == 0 || i == 3) ? TextureFormat::Srgb8 : TextureFormat::Rgba8)
                                      : ((i == 0 || i == 3) ? TextureFormat::Bc7Srgb
                                         : i == 1           ? TextureFormat::Bc5
                                                            : TextureFormat::Bc7);
            if (data[i].format != expected || data[i].layers != 4 ||
                (i != 3 && (data[i].width != data[0].width || data[i].height != data[0].height)) ||
                (i == 3 && (data[i].width != data[i].height || data[i].width > 512)))
                throw std::runtime_error("Invalid material palette semantics");
            total += data[i].bytes.size();
        }
        uint64_t used = s.resident();
        if (used > s.budget || total > s.budget - used)
            throw std::runtime_error("Material budget exhausted; retaining palette");
        auto next = std::make_shared<Palette>();
        next->macro = hasMacro;
        for (int i = 0; i < 4; ++i) {
            next->images[i] = s.image(data[i], s.budget - used - next->bytes);
            next->bytes += next->images[i]->bytes;
        }
        *free = next;
        s.current = std::move(next);
        s.stats.loaded = true;
        s.stats.macroLoaded = hasMacro;
        s.stats.error.clear();
        s.stats.imageBytes = s.resident();
        return true;
    } catch (const std::exception &e) {
        s.stats.error = e.what();
        std::cerr << "Materials: " << e.what() << '\n';
        return false;
    }
}
void VulkanMaterials::prepare(uint32_t frame, const Camera &camera, TerrainGrid grid,
                              const TerrainSettings &terrain, const TerrainMaterialSettings &settings,
                              bool terrainCoordinates, const VegetationSettings *vegetation) {
    auto &s = *impl_;
    auto &f = s.frames.at(frame);
    if (!std::isfinite(settings.repeatMetres) || !std::isfinite(settings.normalStrength) ||
        !std::isfinite(settings.roughness) || !std::isfinite(settings.tint.x) ||
        !std::isfinite(settings.tint.y) || !std::isfinite(settings.tint.z) ||
        !std::isfinite(settings.heightInfluence) || !std::isfinite(settings.blendWidth) ||
        !std::isfinite(settings.macroRepeatMetres) || !std::isfinite(settings.macroStrength))
        throw std::invalid_argument("Nonfinite terrain material settings");
    const auto sampler = std::clamp(settings.filtering, 0, 2);
    const bool uploadCompleted = f.palette == s.current && s.current->uploaded;
    for (auto &im : f.images)
        if (im && im->uploaded)
            im->staging.reset();
    if (f.palette != s.current || f.sampler != sampler || f.revision != s.revision)
        s.descriptors(f, sampler);
    s.retireImages();
    // Staging buffers stay with the palette until both slots have passed its
    // upload.
    if (uploadCompleted)
        for (auto &i : s.current->images)
            i->staging.reset();
    MaterialUniform u{};
    if (vegetation && terrainCoordinates) {
        u.habitat = {vegetation->habitat.weights, 1};
        const auto clearing = relativePosition(vegetation->exclusion.centre, worldPosition(camera, grid));
        u.clearing = {float(clearing.x), float(clearing.z), vegetation->exclusion.radius, 0};
    }
    TerrainRequest request;
    request.chunk = terrainCoordinates ? camera.chunk : ChunkCoord{};
    request.settings = terrain;
    u.geography = terrainParameters(grid, request);
    u.geography.sampling.w = 1;
    // Appearance patches have a fixed 64 m wavelength, independent of erosion
    // settings.
    u.geography.domains[12].fractionFrequency.z = 1.f / 64.f;
    u.geography.domains[12].lattice.z = 0x425741;
    const float repeat = std::clamp(settings.repeatMetres, .25f, 128.f);
    u.controls = {std::clamp(settings.normalStrength, 0.f, 4.f), std::clamp(settings.roughness, .1f, 2.f),
                  float(camera.local.x), float(camera.local.z)};
    u.tint = glm::vec4(glm::clamp(settings.tint, glm::vec3(0), glm::vec3(2)), 0);
    u.options = {settings.enabled && s.stats.loaded ? 1u : 0u, settings.weights ? 1u : 0u,
                 uint32_t(std::clamp(settings.isolate, -1, 3) + 1), 0};
    u.coordinates = materialCoordinates(camera.chunk.x, camera.chunk.z, grid.extent(), camera.local, repeat,
                                        std::clamp(settings.macroRepeatMetres, 16.f, 1024.f));
    u.blend = {settings.heightBlend ? std::clamp(settings.heightInfluence, 0.f, 1.f) : 0.f,
               std::clamp(settings.blendWidth, .01f, 1.f),
               settings.macro && s.current->macro ? std::clamp(settings.macroStrength, 0.f, 1.f) : 0.f,
               settings.heightBlend || settings.cliffs || settings.macro ? 1.f : 0.f};
    u.features = {settings.cliffs ? 1u : 0u, settings.twoAxis ? 1u : 0u, 0u,
                  uint32_t(std::clamp(settings.diagnostic, 0, 4))};
    uint32_t reads = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (u.options.z > 0 && u.options.z != i + 1)
            continue;
        const uint32_t axes = i == 2 && settings.cliffs ? 3 : 1;
        reads += axes * (3 + (u.blend.z > 0 ? 1 : 0));
    }
    s.stats.maximumTextureReads = u.options.x ? reads : 0;
    std::memcpy(f.uniform.mapped, &u, sizeof(u));
    s.stats.effectiveAnisotropy = sampler ? std::min(s.caps.maxAnisotropy, sampler == 1 ? 8.f : 16.f) : 1;
    s.stats.imageBytes = s.resident();
}
void VulkanMaterials::recordUploads(VkCommandBuffer cmd) {
    auto &s = *impl_;
    const auto upload = [&](Image *im) {
        if (im->uploaded)
            return;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = im->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im->levels, 0, im->layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        uint64_t offset = 0;
        for (uint32_t layer = 0; layer < im->layers; ++layer)
            for (uint32_t mip = 0; mip < im->levels; ++mip) {
                VkBufferImageCopy copy{};
                copy.bufferOffset = offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
                copy.imageExtent = {std::max(1u, im->width >> mip), std::max(1u, im->height >> mip), 1};
                vkCmdCopyBufferToImage(cmd, im->staging->handle, im->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                offset += textureMipBytes(im->fmt, copy.imageExtent.width, copy.imageExtent.height);
            }
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
        im->uploaded = true;
    };
    for (auto palette : {s.fallback, s.current}) {
        for (auto &im : palette->images)
            upload(im.get());
        palette->uploaded = true;
    }
    for (auto &im : s.meshImages)
        if (im.image && im.live)
            upload(im.image.get());
}
uint64_t VulkanMaterials::textureAllocationRequirements(const TextureData &t) const {
    auto &s = *impl_;
    if (!t.width || !t.height || !t.levels || t.layers != 1 || t.bytes.size() != textureBytes(t))
        throw std::invalid_argument("Invalid texture requirements");
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format(t.format);
    ci.extent = {t.width, t.height, 1};
    ci.mipLevels = t.levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImage image{};
    check(vkCreateImage(s.device, &ci, nullptr, &image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(s.device, image, &req);
    vkDestroyImage(s.device, image, nullptr);
    auto bytes = req.size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = t.bytes.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer{};
    check(vkCreateBuffer(s.device, &bi, nullptr, &buffer));
    vkGetBufferMemoryRequirements(s.device, buffer, &req);
    vkDestroyBuffer(s.device, buffer, nullptr);
    return bytes + req.size;
}
TextureHandle VulkanMaterials::uploadTexture(const TextureData &t) {
    auto &s = *impl_;
    s.retireImages();
    if (t.layers != 1 || !t.width || !t.height || !t.levels || t.levels > 15 ||
        t.bytes.size() != textureBytes(t))
        throw std::invalid_argument("Invalid mesh texture");
    for (uint32_t i = 3; i < s.caps.images; ++i)
        if (!s.meshImages[i].image) {
            auto &slot = s.meshImages[i];
            const auto used = s.resident();
            if (used >= s.budget)
                return {};
            slot.image = s.image(t, s.budget - used);
            slot.live = true;
            if (!++slot.generation)
                ++slot.generation;
            ++s.revision;
            return {i, slot.generation};
        }
    return {};
}
bool VulkanMaterials::releaseTexture(TextureHandle h) {
    auto &s = *impl_;
    if (h.slot < 3 || s.textureIndex(h, ~0u) == ~0u)
        return false;
    s.meshImages[h.slot].live = false;
    ++s.revision;
    return true;
}
MaterialHandle VulkanMaterials::createMaterial(const MeshMaterial &m) {
    validate(m);
    auto &s = *impl_;
    const auto checkTexture = [&](TextureHandle h, bool colour) {
        const auto slot = s.textureIndex(h, ~0u);
        if (slot == ~0u)
            return;
        const auto format = s.meshImages[slot].image->fmt;
        const bool srgb = format == TextureFormat::Srgb8 || format == TextureFormat::Bc7Srgb;
        if (srgb != colour)
            throw std::invalid_argument("Mesh texture colour space does not match its semantic");
    };
    checkTexture(m.colourTexture, true);
    checkTexture(m.emissiveTexture, true);
    checkTexture(m.normalTexture, false);
    checkTexture(m.ormTexture, false);
    auto h = s.materialSlots.acquire();
    if (h.slot != ~0u) {
        s.meshMaterials[h.slot] = m;
        ++s.revision;
    }
    return h;
}
bool VulkanMaterials::releaseMaterial(MaterialHandle h) {
    auto &s = *impl_;
    auto slot = s.materialSlots.resolve(h);
    if (slot < 5)
        return false;
    s.materialSlots.release(h);
    s.meshMaterials[slot] = MeshMaterial{};
    ++s.revision;
    return true;
}
uint32_t VulkanMaterials::resolve(MaterialHandle h) const { return impl_->materialSlots.resolve(h); }
const MeshMaterial &VulkanMaterials::meshMaterial(MaterialHandle h) const {
    return impl_->meshMaterials[resolve(h)];
}

} // namespace ow

#include "vulkan_trees_internal.hpp"
namespace ow {
VulkanTrees::VulkanTrees(VkPhysicalDevice p, VkDevice d, uint32_t family, VkDescriptorSetLayout lighting,
                         const std::filesystem::path &shaders, bool narrow, bool anisotropy,
                         const TreeOptions &o)
    : impl_(std::make_unique<Impl>(p, d, o)) {
    auto &x = *impl_;
    x.fixture = !o.fixture.empty();
    x.library = o.library ? *o.library : readTreeLibrary(o.directory / "library.bin");
    validate(x.library);
    auto capacity = o.streaming.cellCapacity;
    uint64_t count = uint64_t(capacity) * 128;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(p, &props);
    if (count * sizeof(TreeInstance) > props.limits.maxStorageBufferRange)
        throw std::runtime_error("Tree population exceeds device storage range");
    x.timestampPeriod = props.limits.timestampPeriod;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, queues.data());
    x.timestampBits = queues.at(family).timestampValidBits;
    x.signatures.resize(capacity);
    x.generations.resize(capacity);
    x.previousGenerations.resize(capacity);
    x.heights.resize(capacity);
    VkDescriptorSetLayoutBinding bindings[13]{};
    for (uint32_t i = 0; i < 13; ++i) {
        bindings[i] = {i,
                       i == 3   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                       : i == 9 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       1, VK_SHADER_STAGE_ALL, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sc.bindingCount = 13;
    sc.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(d, &sc, nullptr, &x.setLayout));
    VkDescriptorSetLayoutBinding asset[7]{};
    for (uint32_t i = 0; i < 7; ++i)
        asset[i] = {i, i < 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    sc.bindingCount = 7;
    sc.pBindings = asset;
    check(vkCreateDescriptorSetLayout(d, &sc, nullptr, &x.assetLayout));
    VkDescriptorSetLayoutBinding update[4]{};
    for (uint32_t i = 0; i < 4; ++i)
        update[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    sc.bindingCount = 4;
    sc.pBindings = update;
    check(vkCreateDescriptorSetLayout(d, &sc, nullptr, &x.updateLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 38},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 24 * 5 + 2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 48}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 28;
    dp.poolSizeCount = 4;
    dp.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(d, &dp, nullptr, &x.pool));
    auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet &set) {
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        a.descriptorPool = x.pool;
        a.descriptorSetCount = 1;
        a.pSetLayouts = &layout;
        check(vkAllocateDescriptorSets(d, &a, &set));
    };
    VkSamplerCreateInfo sam{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sam.minFilter = sam.magFilter = VK_FILTER_LINEAR;
    sam.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sam.addressModeU = sam.addressModeV = sam.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sam.maxLod = 16;
    sam.anisotropyEnable = anisotropy;
    sam.maxAnisotropy = anisotropy ? std::min(8.f, props.limits.maxSamplerAnisotropy) : 1.f;
    check(vkCreateSampler(d, &sam, nullptr, &x.sampler));
    sam.anisotropyEnable = false;
    sam.maxAnisotropy = 1;
    sam.addressModeU = sam.addressModeV = sam.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(d, &sam, nullptr, &x.atlasSampler));
    VkDescriptorSetLayout layouts[] = {x.setLayout, lighting, x.assetLayout};
    VkPushConstantRange push{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 3;
    pl.pSetLayouts = layouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &x.layout));
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &x.updateLayout;
    pl.pushConstantRangeCount = 0;
    check(vkCreatePipelineLayout(d, &pl, nullptr, &x.populateLayout));
    x.select = compute(d, x.layout, shaders / "tree_select.comp.spv");
    x.canopyPipeline = compute(d, x.layout, shaders / "tree_canopy.comp.spv");
    x.populate = compute(d, x.populateLayout, shaders / "tree_populate.comp.spv");
    ShaderModule vs(d, shaders / "tree.vert.spv"), fs(d, shaders / "tree.frag.spv"),
        temporal(d, shaders / "tree_temporal.frag.spv"), depth(d, shaders / "tree_depth.frag.spv");
    x.pipelines[0] =
        treeGraphicsPipeline(d, x.layout, vs.handle, fs.handle, {VK_FORMAT_R16G16B16A16_SFLOAT}, true);
    x.pipelines[1] = treeGraphicsPipeline(d, x.layout, vs.handle, fs.handle, {VK_FORMAT_R16G16B16A16_SFLOAT},
                                          false, VK_COMPARE_OP_EQUAL);
    x.pipelines[2] = treeGraphicsPipeline(d, x.layout, vs.handle, depth.handle, {}, true);
    x.pipelines[3] = treeGraphicsPipeline(d, x.layout, vs.handle, depth.handle, {}, true);
    for (uint32_t i = 0; i < 2; ++i)
        x.pipelines[4 + i] =
            treeGraphicsPipeline(d, x.layout, vs.handle, temporal.handle,
                                 {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                  narrow ? VK_FORMAT_R8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT},
                                 i == 0, i == 0 ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_EQUAL);
    // Blocking transfers are confined to construction. Reuse one small startup
    // submission.
    struct Upload {
        VkDevice d;
        VkCommandPool pool{};
        VkCommandBuffer cmd{};
        VkQueue queue{};
        ~Upload() {
            if (pool)
                vkDestroyCommandPool(d, pool, nullptr);
        }
        void begin() {
            check(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(vkBeginCommandBuffer(cmd, &b));
        }
        void end() {
            check(vkEndCommandBuffer(cmd));
            VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            s.commandBufferCount = 1;
            s.pCommandBuffers = &cmd;
            check(vkQueueSubmit(queue, 1, &s, {}));
            check(vkQueueWaitIdle(queue));
        }
    } upload{d};
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = family;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(d, &cp, nullptr, &upload.pool));
    VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = upload.pool;
    cb.commandBufferCount = 1;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    check(vkAllocateCommandBuffers(d, &cb, &upload.cmd));
    vkGetDeviceQueue(d, family, 0, &upload.queue);
    auto loadImage = [&](const std::filesystem::path &path, uint32_t fallback) {
        TextureData t;
        if (!o.directory.empty()) {
            t = readTexture(path, true);
            VkFormatProperties supported{};
            vkGetPhysicalDeviceFormatProperties(p, TreeImage::format(t.format), &supported);
            const bool compressed = t.format == TextureFormat::Bc7 || t.format == TextureFormat::Bc7Srgb ||
                                    t.format == TextureFormat::Bc5;
            if (compressed && (o.forceRgba || !(supported.optimalTilingFeatures &
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))
                t = readTexture(path.parent_path() / (path.stem().string() + "_rgba.dds"), true);
        } else {
            t.width = t.height = 4;
            t.format = fallback == 4                                     ? TextureFormat::R16
                       : fallback == 0 || fallback == 1 || fallback == 2 ? TextureFormat::Srgb8
                                                                         : TextureFormat::Rgba8;
            t.bytes.resize(16 * (fallback == 4 ? 2 : 4), 128);
            if (fallback != 4)
                for (uint32_t i = 0; i < 16; ++i) {
                    t.bytes[i * 4] = fallback == 3 ? 128 : 100;
                    t.bytes[i * 4 + 1] = fallback == 3 ? 128 : 150;
                    t.bytes[i * 4 + 2] = fallback == 3 ? 210 : 70;
                    t.bytes[i * 4 + 3] = 255;
                }
        }
        auto image = std::make_unique<TreeImage>();
        image->budgetLimit = o.streaming.budgetBytes - x.bytes;
        image->load(p, d, t);
        if (x.bytes + image->bytes + image->staging.bytes > o.streaming.budgetBytes)
            throw std::runtime_error("Tree texture upload exceeds budget");
        upload.begin();
        image->upload(upload.cmd);
        upload.end();
        image->staging.reset();
        x.bytes += image->bytes;
        auto view = image->view;
        x.images.push_back(std::move(image));
        return view;
    };
    std::array<std::array<VkImageView, 2>, 4> species{};
    for (uint32_t s = 0; s < 4; ++s) {
        std::string name = treeSpeciesName(TreeSpecies(s));
        species[s][0] = loadImage(o.directory / (name + "_bark.dds"), 0);
        species[s][1] = loadImage(o.directory / (name + "_leaf.dds"), 1);
    }
    for (uint32_t a = 0; a < x.library.assets.size(); ++a) {
        allocSet(x.assetLayout, x.assetSets[a]);
        imageDescriptor(d, x.assetSets[a], 0, species[uint32_t(x.library.assets[a].species)][0]);
        imageDescriptor(d, x.assetSets[a], 1, species[uint32_t(x.library.assets[a].species)][1]);
        std::string prefix = "tree" + std::to_string(a);
        const char *names[] = {"_colour.dds", "_normal.dds", "_depth.dds"};
        for (uint32_t k = 0; k < 3; ++k)
            imageDescriptor(d, x.assetSets[a], k + 2, loadImage(o.directory / (prefix + names[k]), k + 2));
        imageDescriptor(d, x.assetSets[a], 5, {}, x.sampler);
        imageDescriptor(d, x.assetSets[a], 6, {}, x.atlasSampler);
    }
    std::vector<TreeVertex> vertices;
    std::vector<uint32_t> indices;
    for (uint32_t a = 0; a < x.library.assets.size(); ++a) {
        const auto &asset = x.library.assets[a];
        x.current.capture[a] = asset.capture;
        float radius = std::max({std::abs(asset.minimum.x), std::abs(asset.maximum.x),
                                 std::abs(asset.minimum.z), std::abs(asset.maximum.z)}) *
                           1.42f +
                       asset.displacement;
        x.current.bounds[a] = {radius, asset.minimum.y - asset.displacement,
                               asset.maximum.y + asset.displacement, asset.maximum.y - asset.minimum.y};
        for (uint32_t lod = 0; lod < 4; ++lod)
            for (uint32_t part = 0; part < 2; ++part) {
                uint32_t bin = a * 4 + lod;
                auto &info = x.current.geometry[bin * 2 + part];
                info = {0, uint32_t(indices.size()), uint32_t(vertices.size()), asset.frames};
                if (lod < 3) {
                    auto &p = asset.lods[lod][part];
                    info.x = p.indices.size();
                    vertices.insert(vertices.end(), p.vertices.begin(), p.vertices.end());
                    indices.insert(indices.end(), p.indices.begin(), p.indices.end());
                } else if (part == 0) {
                    info.x = 6;
                    for (auto q : {glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1)}) {
                        TreeVertex v;
                        v.position = {q, 0};
                        v.uv = q * .5f + .5f;
                        vertices.push_back(v);
                    }
                    indices.insert(indices.end(), {0, 1, 2, 0, 2, 3});
                }
            }
    }
    x.indexOffset = vertices.size() * sizeof(TreeVertex);
    x.allocate(x.geometry, x.indexOffset + indices.size() * 4,
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    {
        Buffer staging;
        x.allocate(staging, x.geometry.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, vertices.data(), x.indexOffset);
        std::memcpy(static_cast<char *>(staging.mapped) + x.indexOffset, indices.data(), indices.size() * 4);
        upload.begin();
        VkBufferCopy copy{0, 0, x.geometry.size};
        vkCmdCopyBuffer(upload.cmd, staging.buffer, x.geometry.buffer, 1, &copy);
        barrier(upload.cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
        upload.end();
        x.bytes -= staging.bytes;
    }
    x.allocate(x.population, count * sizeof(TreeInstance),
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    x.allocate(x.attachment, count * 16,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    x.allocate(x.lodHistory, count * 4,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    x.allocate(x.emptyHeight, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    std::memset(x.emptyHeight.mapped, 0, 16);
    upload.begin();
    vkCmdFillBuffer(upload.cmd, x.lodHistory.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(upload.cmd, x.population.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(upload.cmd, x.attachment.buffer, 0, VK_WHOLE_SIZE, 0);
    barrier(upload.cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    upload.end();
    if (x.fixture) {
        if (o.fixture.size() > capacity)
            throw std::invalid_argument("Tree fixture exceeds capacity");
        Buffer staging;
        x.allocate(staging, o.fixture.size() * 128 * (32 + 16), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        auto *pop = static_cast<TreeInstance *>(staging.mapped);
        auto *roots = reinterpret_cast<glm::vec4 *>(pop + o.fixture.size() * 128);
        for (uint32_t cell = 0; cell < o.fixture.size(); ++cell) {
            auto &slot = x.residency.slot(cell);
            slot.key = o.fixture[cell].key;
            slot.state = TreeResidency::State::Active;
            slot.generated = true;
            x.heights[cell] = {0, 0};
            for (uint32_t lane = 0; lane < 128; ++lane) {
                auto t = o.fixture[cell].instances[lane];
                if (!std::isfinite(t.yaw) || !std::isfinite(t.variation) || !std::isfinite(t.rootScale.x) ||
                    !std::isfinite(t.rootScale.y) || !std::isfinite(t.rootScale.z) ||
                    !std::isfinite(t.rootScale.w) || t.rootScale.w < 0 || t.rootScale.w > 1.2f ||
                    t.rootScale.x < 0 || t.rootScale.x >= 64 || t.rootScale.z < 0 || t.rootScale.z >= 64)
                    throw std::invalid_argument("Invalid fixture tree transform");
                if ((t.assetCell & 31) >= x.library.assets.size())
                    throw std::invalid_argument("Invalid fixture tree asset");
                t.assetCell = (cell << 5) | (t.assetCell & 31);
                pop[cell * 128 + lane] = t;
                roots[cell * 128 + lane] = {t.rootScale.y, t.rootScale.y, 0, t.rootScale.w > 0 ? 1 : 0};
                x.heights[cell].x = std::min(x.heights[cell].x, t.rootScale.y);
                x.heights[cell].y = std::max(x.heights[cell].y, t.rootScale.y);
            }
        }
        upload.begin();
        VkBufferCopy a{0, 0, o.fixture.size() * 128 * 32},
            b{o.fixture.size() * 128 * 32, 0, o.fixture.size() * 128 * 16};
        vkCmdCopyBuffer(upload.cmd, staging.buffer, x.population.buffer, 1, &a);
        vkCmdCopyBuffer(upload.cmd, staging.buffer, x.attachment.buffer, 1, &b);
        barrier(upload.cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        upload.end();
        x.bytes -= staging.bytes;
    }
    x.listCapacity = std::min<uint64_t>(count * 2, 1048576);
    for (uint32_t v = 0; v < 6; ++v)
        x.current.lists[v] = {v * x.listCapacity, x.listCapacity, 0, 0};
    for (auto &f : x.frames) {
        allocSet(x.setLayout, f.set);
        allocSet(x.updateLayout, f.updateSet);
        x.allocate(f.uniform, sizeof(Uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        x.allocate(f.cells, uint64_t(capacity) * sizeof(Cell), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        x.allocate(f.groups, capacity * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        x.allocate(f.lists, uint64_t(x.listCapacity) * 6 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        x.allocate(f.work, words * 6 * 4,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        x.allocate(f.prefixes, uint64_t(capacity) * 96 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        x.allocate(f.choices, count * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        x.allocate(f.indirect, 6 * 192 * 20,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        x.allocate(f.jobs, 32 * sizeof(Job), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        x.allocate(f.readback, words * 6 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        x.allocate(f.canopy, 128 * 128 * 4,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        descriptor(d, f.set, 11, f.canopy);
        descriptor(d, f.set, 12, x.lodHistory);
        descriptor(d, f.set, 0, x.population);
        descriptor(d, f.set, 1, x.attachment);
        descriptor(d, f.set, 2, f.cells);
        descriptor(d, f.set, 3, f.uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        descriptor(d, f.set, 4, f.lists);
        descriptor(d, f.set, 5, f.work);
        descriptor(d, f.set, 6, f.groups);
        descriptor(d, f.set, 7, f.prefixes);
        descriptor(d, f.set, 8, f.choices);
        descriptor(d, f.set, 10, f.indirect);
        imageDescriptor(d, f.set, 9, x.images[0]->view);
        descriptor(d, f.updateSet, 0, x.population);
        descriptor(d, f.updateSet, 1, x.attachment);
        descriptor(d, f.updateSet, 2, f.jobs);
        descriptor(d, f.updateSet, 3, x.emptyHeight);
    }
    if (x.timestampBits) {
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = queryCount * 2;
        check(vkCreateQueryPool(d, &q, nullptr, &x.queries));
    }
}

} // namespace ow

#include "texture_import.hpp"
#include "vegetation_library.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
// Import is intentionally independent of renderer and production artwork
// generation.
Image padded(Image source, float threshold = .01f) {
    // Multi-source flood fills transparent RGB without changing source opacity.
    std::vector<uint32_t> queue;
    queue.reserve(source.p.size());
    std::vector<uint8_t> visited(source.p.size());
    for (uint32_t i = 0; i < source.p.size(); ++i)
        if (source.p[i].a > threshold) {
            visited[i] = 1;
            queue.push_back(i);
        }
    if (queue.empty())
        throw std::runtime_error("Empty foliage opacity");
    for (size_t head = 0; head < queue.size(); ++head) {
        uint32_t i = queue[head], x = i % source.size, y = i / source.size;
        for (int axis = 0; axis < 4; ++axis) {
            int nx = int(x) + (axis == 0   ? 1
                               : axis == 1 ? -1
                                           : 0),
                ny = int(y) + (axis == 2   ? 1
                               : axis == 3 ? -1
                                           : 0);
            if (nx < 0 || ny < 0 || nx >= int(source.size) || ny >= int(source.size))
                continue;
            uint32_t j = ny * source.size + nx;
            if (visited[j])
                continue;
            source.p[j] = glm::vec4(glm::vec3(source.p[i]), source.p[j].a);
            visited[j] = 1;
            queue.push_back(j);
        }
    }
    return source;
}
Image reduce(const Image &a, bool normal) {
    Image b{std::max(1u, a.size / 2), {}};
    b.p.resize(b.size * b.size);
    for (uint32_t y = 0; y < b.size; ++y)
        for (uint32_t x = 0; x < b.size; ++x) {
            glm::vec4 total(0);
            glm::vec3 colour(0);
            float alpha = 0;
            for (uint32_t dy = 0; dy < 2; ++dy)
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    auto v = a.p[(y * 2 + dy) * a.size + x * 2 + dx];
                    total += v;
                    colour += glm::vec3(v) * v.a;
                    alpha += v.a;
                }
            b.p[y * b.size + x] =
                normal ? glm::vec4(glm::normalize(glm::vec3(total)), 1)
                       : glm::vec4(alpha > 1e-8f ? colour / alpha : glm::vec3(total) * .25f, alpha * .25f);
        }
    return normal ? b : padded(b, 0);
}
std::pair<TextureData, TextureData> importMap(Image im, bool normal, float cutoff) {
    TextureData bc, rgba;
    bc.width = bc.height = rgba.width = rgba.height = im.size;
    bc.levels = rgba.levels = 1;
    for (uint32_t n = im.size; n > 1; n >>= 1) {
        ++bc.levels;
        ++rgba.levels;
    }
    bc.format = normal ? TextureFormat::Bc5 : TextureFormat::Bc7Srgb;
    rgba.format = normal ? TextureFormat::Rgba8 : TextureFormat::Srgb8;
    double coverage = 0;
    for (auto v : im.p)
        coverage += v.a >= cutoff;
    coverage /= im.p.size();
    for (;;) {
        Image encoded = im;
        if (!normal) {
            float low = 0, high = 64, best = 1;
            double error = 1;
            for (int step = 0; step < 24; ++step) {
                float scale = (low + high) * .5f;
                size_t hits = 0;
                for (auto v : im.p)
                    hits += byte(v.a * scale) >= byte(cutoff);
                double fraction = double(hits) / im.p.size(), e = std::abs(fraction - coverage);
                if (e < error) {
                    error = e;
                    best = scale;
                }
                if (fraction < coverage)
                    low = scale;
                else
                    high = scale;
            }
            for (auto &v : encoded.p)
                v.a = std::clamp(v.a * best, 0.f, 1.f);
        }
        append(bc, encoded, normal ? 1 : 0, true);
        append(rgba, encoded, normal ? 1 : 0, false);
        if (im.size == 1)
            break;
        im = reduce(im, normal);
    }
    return {std::move(bc), std::move(rgba)};
}
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("Usage: vegetation_import recipe.json output-directory");
        const std::filesystem::path recipe = argv[1], out = argv[2];
        std::ifstream in(recipe);
        nlohmann::json j;
        in >> j;
        if (j.at("version") != 1)
            throw std::runtime_error("Unsupported vegetation recipe");
        VegetationRecipe r;
        r.blades = j.at("grass").at("blades");
        r.width = j.at("grass").at("width");
        r.height = j.at("grass").at("height");
        r.spread = j.at("grass").at("spread");
        auto library = buildVegetationLibrary(r), rgba = library;
        uint32_t size = j.value("size", 1024u);
        if (size < 16 || size > 2048 || (size & (size - 1)))
            throw std::runtime_error("Invalid vegetation texture size");
        if (j.at("leaves").size() != 2)
            throw std::runtime_error("Two leaf sources required");
        bc7enc_compress_block_init();
        for (uint32_t k = 0; k < 2; ++k) {
            auto &entry = j.at("leaves").at(k);
            auto source = padded(load(recipe.parent_path() / entry.at("colour").get<std::string>(), true));
            Image colour = padded(resize(source, size)), normal{size, {}};
            normal.p.resize(size * size);
            float dome = entry.value("normalDome", .12f);
            if (!std::isfinite(dome) || dome < 0 || dome > .3f)
                throw std::runtime_error("Invalid authored normal dome");
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x)
                    normal.p[y * size + x] =
                        glm::vec4(glm::normalize(glm::vec3(((x + .5f) / size - .5f) * dome,
                                                           ((y + .5f) / size - .5f) * dome, 1)),
                                  1);
            auto c = importMap(std::move(colour), false, .5f), n = importMap(std::move(normal), true, .5f);
            library.textures[k * 2] = std::move(c.first);
            rgba.textures[k * 2] = std::move(c.second);
            library.textures[k * 2 + 1] = std::move(n.first);
            rgba.textures[k * 2 + 1] = std::move(n.second);
        }
        // Revision fingerprints actual geometry, recipe and imported texture bytes.
        uint32_t revision = 2166136261u;
        const auto hashBytes = [&](const void *ptr, size_t size) {
            auto bytes = static_cast<const unsigned char *>(ptr);
            for (size_t i = 0; i < size; ++i) {
                revision ^= bytes[i];
                revision *= 16777619u;
            }
        };
        auto recipeText = j.dump();
        hashBytes(recipeText.data(), recipeText.size());
        for (const auto &t : library.textures)
            hashBytes(t.bytes.data(), t.bytes.size());
        for (const auto &a : library.assets)
            for (const auto &lod : a.lods)
                for (const auto &part : lod) {
                    for (const auto &v : part.vertices)
                        for (int k = 0; k < 3; ++k)
                            hashBytes(&v.position[k], sizeof(float));
                    for (auto index : part.indices)
                        hashBytes(&index, sizeof(index));
                }
        library.revision = rgba.revision = revision;
        writeVegetationLibrary(out, library);
        writeVegetationLibrary(out, rgba, "_rgba");
        if (j.contains("ground")) {
            auto ground = resize(load(recipe.parent_path() / j.at("ground").get<std::string>(), true), 512);
            auto maps = importMap(std::move(ground), false, .5f);
            writeTexture(out / "ground.dds", maps.first);
            writeTexture(out / "ground_rgba.dds", maps.second);
        }
        std::ofstream manifest(out / "recipe.json");
        manifest << j.dump(2) << '\n';
        if (!manifest)
            throw std::runtime_error("Writing vegetation manifest failed");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

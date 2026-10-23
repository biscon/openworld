#include "renderer.hpp"
#include "terrain_generation_budget.hpp"
#include "test_support.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <iostream>
#include <thread>
namespace {
struct Temp {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ow-stream-gpu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directories(path); }
    ~Temp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
ow::RendererOptions options(const Temp &temp) {
    ow::RendererOptions o;
    o.ui = false;
    o.requireValidation = true;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.terrainGrid = {64, .5f};
    o.terrainCapacity = 64;
    o.lod = {true, 8, 64};
    o.streaming.shadowCasterBytes = 0;
    o.streaming.enabled = true;
    o.streaming.transferBytesPerFrame = 64 * 1024;
    o.streaming.cacheDirectory = temp.path;
    o.streaming.gpuBytes = 40 * ow::terrainMiB;
    o.streaming.hostBytes = 64 * ow::terrainMiB;
    return o;
}
void settle(ow::Renderer &r, ow::Camera &camera, ow::SceneSettings &scene, uint32_t coverage,
            uint32_t detail) {
    for (unsigned frame = 0; frame < 2400; ++frame) {
        r.render(camera, scene);
        const auto &s = r.terrainStreamingStats();
        require(s.dispatches <= ow::TerrainGenerationBudget::maximumDispatches, "bounded dispatches");
        require(s.generationGroups <= s.generationGroupBudget &&
                    s.generationGroupBudget <= ow::TerrainGenerationBudget::maximumGroups,
                "bounded measured compute work");
        require(s.transferBytes <= 8 * ow::terrainMiB, "bounded transfers");
        if (s.coverage >= coverage && s.detail >= detail && s.pending == 0 && s.detailPending == 0) {
            // Allow outstanding write jobs, shading fades and LOD-range publication
            // to finish.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (std::chrono::steady_clock::now() < until)
                r.render(camera, scene);
            return;
        }
    }
    const auto &s = r.terrainStreamingStats();
    std::cerr << "settle coverage=" << s.coverage << " detail=" << s.detail << " pending=" << s.pending
              << " generations=" << s.generations << " hits=" << s.hits << " misses=" << s.misses << '\n';
    throw std::runtime_error("Streaming convergence deadline");
}
uint64_t integer(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
std::vector<uint8_t> product(const Temp &temp, ow::TerrainProduct type, ow::ChunkCoord coord) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(temp.path))
        if (entry.path().extension() == ".owc") {
            std::ifstream f(entry.path(), std::ios::binary);
            std::array<uint8_t, 144> h{};
            f.read(reinterpret_cast<char *>(h.data()), h.size());
            if (integer(h.data() + 16, 4) != uint32_t(type) ||
                int64_t(integer(h.data() + 40, 8)) != coord.x ||
                int64_t(integer(h.data() + 48, 8)) != coord.z)
                continue;
            require(integer(h.data() + 8, 4) == 3, "Expected current cache fixture format");
            const auto size = integer(h.data() + 100, 8);
            require(size <= 64 * ow::terrainMiB, "Owned cache fixture size bounded");
            std::vector<uint8_t> data(static_cast<size_t>(size));
            f.read(reinterpret_cast<char *>(data.data()), std::streamsize(data.size()));
            require(bool(f), "complete cache fixture");
            return data;
        }
    throw std::runtime_error("Expected generated cache product");
}
std::vector<char> fileBytes(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    require(bool(f), "capture exists");
    std::vector<char> data(size_t(f.tellg()));
    f.seekg(0);
    f.read(data.data(), std::streamsize(data.size()));
    return data;
}
void test() {
    Temp temp;
    auto o = options(temp);
    ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.shading = ow::ShadingMode::Legacy;
    scene.viewRadius = 1;
    ow::Camera camera;
    camera.reset(o.terrainGrid, {-4, 1000000});
    camera.local = {16, 240, 16};
    camera.pitch = -1.2f;
    const ow::ChunkCoord origin = camera.chunk;
    std::vector<glm::vec2> cold;
    std::array<std::vector<glm::vec4>, 8> coldNormals;
    {
        ow::Renderer r(o);
        settle(r, camera, scene, 25, 9);
        cold = r.readStreamedHeights(origin);
        for (uint32_t l = 0; l < 4; ++l)
            coldNormals[l] = r.readStreamedNormals(origin, l);
        require(r.terrainStreamingStats().generations == 25, "one cold generation per coverage chunk");
        require(r.terrainStreamingStats().gpuBytes <= o.streaming.gpuBytes, "GPU budget");
        require(r.terrainStreamingStats().hostBytes <= o.streaming.hostBytes, "host budget");
        const auto allocations = r.terrainAllocatedBytes(), generations = r.terrainGenerationCount();
        for (int i = 0; i < 20; ++i) {
            camera.yaw += .02f;
            r.render(camera, scene);
        }
        require(r.terrainAllocatedBytes() == allocations && r.terrainGenerationCount() == generations,
                "draw-only motion reuses residency");
        require(r.validationErrors() == 0, "cold streaming Vulkan validation");
    }
    // Compare both products against the established independent full-generation
    // path.
    auto legacy = o;
    legacy.streaming.enabled = false;
    legacy.terrainChunkCapacity = 1;
    {
        ow::Renderer reference(legacy);
        reference.requestTerrain({origin, {}});
        for (int i = 0; i < 4; ++i)
            reference.render(camera, scene);
        const auto expected = reference.readTerrainSnapshot();
        for (size_t i = 0; i < cold.size(); ++i) {
            near(cold[i].x, expected[i].base.w, 1e-5, "streamed base height");
            near(cold[i].y, expected[i].eroded.w, 1e-5, "streamed eroded height");
        }
        ow::TerrainPayloadLayout layout(o.terrainGrid, 2, 8);
        const auto coverage = product(temp, ow::TerrainProduct::Coverage, origin),
                   detail = product(temp, ow::TerrainProduct::Detail, origin);
        require(layout.valid(ow::TerrainProduct::Coverage, coverage.data(), coverage.size()),
                "GPU coverage validates");
        for (uint32_t l = 0; l < layout.levels; ++l) {
            const auto normals = reference.readTerrainNormals(0, l);
            const auto width = (o.terrainGrid.cells * 2 >> l) + 1;
            const auto &data = l + 1 == layout.levels ? coverage : detail;
            const auto offset = l + 1 == layout.levels ? layout.coarseOffset : layout.offsets[l];
            for (uint32_t layer = 0; layer < 2; ++layer)
                for (uint32_t y = 0; y < width; ++y)
                    for (uint32_t x = 0; x < width; ++x) {
                        const auto index = (uint64_t(layer) * layout.widths[l] * layout.widths[l] +
                                            (y + layout.halos[l]) * layout.widths[l] + x + layout.halos[l]) *
                                               8 +
                                           offset;
                        uint32_t packed[2];
                        std::memcpy(packed, data.data() + index, 8);
                        const glm::vec4 actual(glm::unpackHalf2x16(packed[0]),
                                               glm::unpackHalf2x16(packed[1]));
                        const auto activeIndex = (uint64_t(layer) * width + y) * width + x;
                        const auto expectedNormal = normals[activeIndex];
                        require(glm::length(coldNormals[l][activeIndex] - expectedNormal) < .0001f,
                                "uploaded generated normal pyramid");
                        require(glm::length(actual - expectedNormal) < .0001f,
                                "tiled generation preserves filtered normal pyramid");
                    }
        }
        camera.local = {16, cold[32 * 65 + 32].y + 8, 16};
        camera.pitch = -1.55f;
        camera.yaw = 0;
        for (int i = 0; i < 3; ++i)
            reference.render(camera, scene);
        reference.capture(temp.path / "reference.ppm");
        require(reference.validationErrors() == 0, "reference Vulkan validation");
    }
    {
        ow::Renderer r(o);
        settle(r, camera, scene, 25, 9);
        const auto warm = r.readStreamedHeights(origin);
        require(warm.size() == cold.size() &&
                    std::memcmp(warm.data(), cold.data(), warm.size() * sizeof(glm::vec2)) == 0,
                "warm-cache heights exact");
        require(r.terrainGenerationCount() == 0 && r.terrainStreamingStats().hits >= 34,
                "warm cache avoids generation");
        for (uint32_t l = 0; l < 4; ++l) {
            const auto normals = r.readStreamedNormals(origin, l);
            require(normals.size() == coldNormals[l].size() &&
                        std::memcmp(normals.data(), coldNormals[l].data(),
                                    normals.size() * sizeof(glm::vec4)) == 0,
                    "uploaded cached normals match generation");
        }
        r.capture(temp.path / "streaming.ppm");
        const auto a = fileBytes(temp.path / "reference.ppm"), b = fileBytes(temp.path / "streaming.ppm");
        require(a.size() == b.size(), "comparison capture dimensions");
        size_t different = 0;
        uint64_t totalDifference = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            int d = std::abs(int(uint8_t(a[i])) - int(uint8_t(b[i])));
            different += d > 2;
            totalDifference += d;
        }
        require(different < 32 && double(totalDifference) / a.size() < .01,
                "streaming preserves established near-ground shading");
        // The shared PBR path also works for streamed terrain at a large world
        // origin.
        scene.shading = ow::ShadingMode::Pbr;
        r.render(camera, scene);
        const auto pbr = r.readHdrSnapshot();
        scene.output.compensation = 1;
        r.render(camera, scene);
        const auto exposed = r.readHdrSnapshot();
        const size_t centre = size_t(r.height() / 2) * r.width() + r.width() / 2;
        require(pbr.pixels[centre].x > 0, "Streamed PBR has visible illumination");
        near(exposed.pixels[centre].x, 2 * pbr.pixels[centre].x, .002, "Streamed PBR uses shared exposure");
        const auto residency = r.readTerrainResidency();
        const auto generation = r.terrainGenerationCount();
        scene.terrainVisible = false;
        ow::Camera preview;
        preview.local = {0, 0, 16};
        for (int i = 0; i < 6; ++i)
            r.render(preview, scene);
        const auto after = r.readTerrainResidency();
        require(residency.size() == after.size(), "Hidden terrain retains terrain residency size");
        for (size_t i = 0; i < after.size(); ++i)
            require(after[i].key == residency[i].key && after[i].resident == residency[i].resident,
                    "Hidden terrain camera does not relocate terrain residency");
        require(r.terrainGenerationCount() == generation, "Hidden terrain generates no replacement terrain");
        scene.terrainVisible = true;
        scene.output.compensation = 0;
        scene.shading = ow::ShadingMode::Legacy;
        // Move while products and old frame submissions are pending, then return to
        // known coverage.
        const auto allocated = r.terrainAllocatedBytes();
        for (int i = 0; i < 32; ++i) {
            camera.chunk = {origin.x + (i % 2 ? 100 : -100) + i, origin.z - i};
            scene.viewRadius = i % 3;
            if (i % 7 == 0) {
                ow::TerrainSettings s;
                s.seed = uint32_t(i + 1);
                s.ridgeStrength = float(i % 3) * .5f;
                s.baseDetail = 1 - s.ridgeStrength;
                r.setTerrainSettings(s);
            }
            r.render(camera, scene);
        }
        r.setTerrainSettings({});
        camera.chunk = origin;
        scene.viewRadius = 1;
        settle(r, camera, scene, 25, 9);
        const auto restored = r.readStreamedHeights(origin);
        require(std::memcmp(restored.data(), cold.data(), cold.size() * sizeof(glm::vec2)) == 0,
                "obsolete results cannot overwrite new epoch");
        require(r.terrainAllocatedBytes() == allocated,
                "radius changes and teleports keep allocations bounded");
        require(r.validationErrors() == 0, "reuse/epoch streaming Vulkan validation");
    }
    {
        auto limited = o;
        limited.streaming.gpuBytes = 6 * ow::terrainMiB;
        ow::Renderer r(limited);
        scene.viewRadius = 1;
        camera.chunk = origin;
        settle(r, camera, scene, 25, 1);
        require(r.terrainStreamingStats().detailCapacity < 9, "fixture forces mixed detail residency");
        require(r.terrainStreamingStats().detail < r.terrainStreamingStats().coverage,
                "coverage retained without fine normals");
        const auto allocated = r.terrainAllocatedBytes();
        for (int i = 0; i < 40; ++i) {
            camera.chunk.x = origin.x + i / 5;
            r.render(camera, scene);
        }
        camera.chunk = origin;
        settle(r, camera, scene, 25, 1);
        const auto restored = r.readStreamedHeights(origin);
        require(std::memcmp(restored.data(), cold.data(), cold.size() * sizeof(glm::vec2)) == 0,
                "detail pressure preserves full heights");
        require(r.terrainAllocatedBytes() == allocated && allocated <= limited.streaming.gpuBytes,
                "detail eviction obeys fixed budget");
        require(r.validationErrors() == 0, "partial uploads and fine-slot retirement validation");
    }
    {
        auto broken = o;
        broken.streaming.cacheDirectory = temp.path / "blocked";
        {
            std::ofstream f(broken.streaming.cacheDirectory);
            f << "not a directory";
        }
        ow::Renderer r(broken);
        scene.viewRadius = 0;
        settle(r, camera, scene, 9, 1);
        require(r.terrainStreamingStats().errors > 0 && r.terrainGenerationCount() > 0,
                "I/O failure falls back to generation");
        require(r.validationErrors() == 0, "I/O fallback Vulkan validation");
    }
}
} // namespace
int main() {
    try {
        test();
        std::cout << "Terrain streaming GPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

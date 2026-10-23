#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace ow;
namespace {
struct Outputs {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openworld-neighbourhood-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Outputs() { std::filesystem::create_directories(path); }
    ~Outputs() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
std::vector<unsigned char> capture(Renderer &renderer, const Camera &camera, const SceneSettings &scene,
                                   const std::filesystem::path &path) {
    renderer.render(camera, scene);
    renderer.capture(path);
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    unsigned width, height, maximum;
    file >> magic >> width >> height >> maximum;
    file.get();
    require(magic == "P6" && width == renderer.width() && height == renderer.height() && maximum == 255,
            "Valid neighbourhood capture");
    std::vector<unsigned char> result(size_t(width) * height * 3);
    require(bool(file.read(reinterpret_cast<char *>(result.data()), std::streamsize(result.size()))),
            "Complete neighbourhood capture");
    return result;
}
void compare(const TerrainSample &a, const TerrainSample &b, double tolerance) {
    near(a.base.w, b.base.w, tolerance, "Base height continuity");
    near(a.eroded.w, b.eroded.w, tolerance, "Eroded height continuity");
    near(glm::length(glm::vec3(a.base - b.base)), 0, tolerance, "Base normal continuity");
    near(glm::length(glm::vec3(a.eroded - b.eroded)), 0, tolerance, "Eroded normal continuity");
}
} // namespace
int main() {
    try {
        Outputs outputs;
        RendererOptions options;
        options.ui = false;
        options.requireValidation = true;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.terrainGrid = {16, 0.5f};
        options.terrainCapacity = 24; // Also check active images smaller than capacity.
        options.terrainChunkCapacity = 9;
        Renderer tiled(options);
        Camera camera;
        SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        scene.shading = ShadingMode::Legacy;
        TerrainRequest centre;
        centre.settings = {17, 128, 60, 0.22f, 0.3f};
        const auto allocated = tiled.terrainAllocatedBytes();
        uint32_t shape = 0;
        for (const ChunkCoord location : {ChunkCoord{0, 0}, {-3, -2}, {1000000, -1000000}}) {
            centre.settings.ridgeStrength = centre.settings.baseDetail = .5f * shape++;
            centre.chunk = location;
            camera.reset(options.terrainGrid, location, 30);
            auto batch = terrainNeighbourhood(centre);
            tiled.requestTerrainBatch(batch);
            bool pendingReadRejected = false;
            try {
                tiled.readTerrainSnapshot(8);
            } catch (const std::runtime_error &) {
                pendingReadRejected = true;
            }
            require(pendingReadRejected, "Pending slot cannot expose stale snapshot");
            tiled.render(camera, scene);
            std::array<std::vector<TerrainSample>, 9> samples;
            std::array<std::vector<glm::vec4>, 9> normals;
            for (uint32_t i = 0; i < 9; ++i) {
                samples[i] = tiled.readTerrainSnapshot(i);
                normals[i] = tiled.readTerrainNormals(i);
            }
            const uint32_t n = 16, w = n + 1, dense = n * 2 + 1;
            auto edge = [&](uint32_t a, uint32_t b, bool horizontal) {
                for (uint32_t v = 0; v <= n; ++v) {
                    const auto ia = horizontal ? v * w + n : n * w + v;
                    const auto ib = horizontal ? v * w : v;
                    compare(samples[a][ia], samples[b][ib], 0.002);
                }
                for (uint32_t layer = 0; layer < 2; ++layer)
                    for (uint32_t v = 0; v < dense; ++v) {
                        const auto ia = layer * dense * dense +
                                        (horizontal ? v * dense + dense - 1 : (dense - 1) * dense + v);
                        const auto ib = layer * dense * dense + (horizontal ? v * dense : v);
                        near(glm::length(normals[a][ia] - normals[b][ib]), 0, 0.002,
                             "Dense normal texture edge");
                    }
            };
            for (uint32_t z = 0; z < 3; ++z)
                for (uint32_t x = 0; x < 3; ++x) {
                    const auto i = z * 3 + x;
                    if (x < 2)
                        edge(i, i + 1, true);
                    if (z < 2)
                        edge(i, i + 3, false);
                    if (x < 2 && z < 2)
                        compare(samples[i].back(), samples[i + 4].front(), 0.002);
                }
            std::reverse(batch.chunks.begin(), batch.chunks.end());
            tiled.requestTerrainBatch(batch);
            tiled.render(camera, scene);
            for (uint32_t i = 0; i < 9; ++i) {
                const auto reversed = tiled.readTerrainSnapshot(8 - i);
                const auto reversedNormals = tiled.readTerrainNormals(8 - i);
                for (size_t j = 0; j < reversed.size(); ++j)
                    compare(samples[i][j], reversed[j], 0);
                require(normals[i] == reversedNormals, "Exact same-device normal texture order independence");
            }
        }
        // Equivalent region: chunks (0..2,0..2) versus one 48-cell patch at (0,0).
        centre.chunk = {1, 1};
        auto batch = terrainNeighbourhood(centre);
        tiled.requestTerrainBatch(batch);
        RendererOptions wholeOptions = options;
        wholeOptions.terrainGrid = {48, 0.5f};
        wholeOptions.terrainCapacity = 48;
        wholeOptions.terrainChunkCapacity = 1;
        Renderer whole(wholeOptions);
        whole.requestTerrain({{}, centre.settings});
        camera.chunk = {};
        camera.local = {12, 100, 12};
        camera.pitch = -1.5707f;
        camera.yaw = 0;
        tiled.render(camera, scene);
        whole.render(camera, scene);
        const auto wholeSamples = whole.readTerrainSnapshot();
        for (uint32_t slot = 0; slot < 9; ++slot) {
            const auto samples = tiled.readTerrainSnapshot(slot);
            for (uint32_t z = 0; z <= 16; ++z)
                for (uint32_t x = 0; x <= 16; ++x)
                    compare(samples[z * 17 + x], wholeSamples[(slot / 3 * 16 + z) * 49 + slot % 3 * 16 + x],
                            0.002);
        }
        const auto count = tiled.terrainGenerationCount();
        for (bool eroded : {false, true})
            for (bool filtered : {false, true}) {
                scene.eroded = eroded;
                scene.filteredNormals = filtered;
                const auto a = capture(tiled, camera, scene, outputs.path / "tiled.ppm");
                const auto b = capture(whole, camera, scene, outputs.path / "whole.ppm");
                size_t differing = 0, foreground = 0;
                for (size_t pixel = 0; pixel < a.size(); pixel += 3) {
                    int error = 0;
                    for (size_t c = 0; c < 3; ++c)
                        error = std::max(error, std::abs(int(a[pixel + c]) - int(b[pixel + c])));
                    differing += error > 3;
                    foreground += b[pixel] != b[0] || b[pixel + 1] != b[1] || b[pixel + 2] != b[2];
                }
                require(foreground > 10000, "Comparison includes substantial visible terrain");
                // Allow only sparse silhouette rasterization differences; cracks
                // crossing the region fail.
                require(differing < 50, "Tiled capture agrees with continuous "
                                        "single-patch lighting/coverage");
            }
        scene.normalView = true;
        scene.wireframe = true;
        scene.chunkBorders = true;
        camera.local.x += 9;
        camera.update({}, 0, options.terrainGrid);
        tiled.render(camera, scene);
        require(tiled.terrainGenerationCount() == count, "Camera and draw-only views do not regenerate");
        auto bad = batch;
        bad.chunks[8] = bad.chunks[0];
        rejects([&] { tiled.requestTerrainBatch(bad); }, "Reject entire invalid batch");
        require(tiled.readTerrainSnapshot(8).size() == 17 * 17, "Rejected batch preserves readable outputs");
        // Exercise real in-flight scratch reuse and topology changes, with no
        // readbacks between draws.
        for (uint32_t i = 0; i < 12; ++i) {
            tiled.setTerrainGrid({i % 2 ? 24u : 8u, 0.5f});
            batch.settings.seed = i;
            tiled.requestTerrainBatch(batch);
            tiled.render(camera, scene);
        }
        require(tiled.terrainGenerationCount() == count + 12 * 9,
                "Each edit generates each active slot once");
        require(tiled.readTerrainIndices() == terrainIndices({24, 0.5f}),
                "Shared topology after in-flight edits");
        // Shrink active count, change grid, then restore slots (must not reuse
        // stale geometry).
        tiled.requestTerrain({{}, centre.settings});
        bool inactiveReadRejected = false;
        try {
            tiled.readTerrainSnapshot(8);
        } catch (const std::out_of_range &) {
            inactiveReadRejected = true;
        }
        require(inactiveReadRejected, "Inactive slot cannot expose stale snapshot");
        tiled.setTerrainGrid({16, 0.5f});
        tiled.render(camera, scene);
        tiled.requestTerrainBatch(batch);
        tiled.render(camera, scene);
        require(tiled.readTerrainSnapshot(8).size() == 17 * 17, "Reactivated slot uses new grid");
        require(tiled.terrainAllocatedBytes() == allocated,
                "Resident allocation stable through diagnostics and edits");
        require(tiled.validationErrors() == 0 && whole.validationErrors() == 0,
                "Neighbourhood Vulkan validation");
        std::cout << "Neighbourhood continuity, ordering, captures and in-flight "
                     "reuse passed; validation errors: 0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

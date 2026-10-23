#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <iostream>
using namespace ow;
namespace {
struct Outputs {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openworld-lod-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Outputs() { std::filesystem::create_directories(path); }
    ~Outputs() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
std::vector<unsigned char> capture(Renderer &r, const std::filesystem::path &path) {
    r.capture(path);
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    uint32_t w, h, maximum;
    f >> magic >> w >> h >> maximum;
    f.get();
    require(magic == "P6" && w == r.width() && h == r.height() && maximum == 255, "LOD capture header");
    std::vector<unsigned char> pixels(size_t(w) * h * 3);
    require(bool(f.read(reinterpret_cast<char *>(pixels.data()), std::streamsize(pixels.size()))),
            "LOD capture data");
    return pixels;
}
void settle(Renderer &r, const Camera &c, const SceneSettings &s) {
    for (int i = 0; i < 3; ++i)
        r.render(c, s);
}
bool samePatches(const std::vector<TerrainPatchDraw> &a, const std::vector<TerrainPatchDraw> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].slot != b[i].slot || a[i].x != b[i].x || a[i].z != b[i].z || a[i].step != b[i].step ||
            a[i].mask != b[i].mask)
            return false;
    return true;
}
std::vector<glm::vec4> filter(const std::vector<glm::vec4> &input, uint32_t width) {
    const uint32_t outWidth = (width - 1) / 2 + 1;
    std::vector<glm::vec4> output(size_t(outWidth) * outWidth * 2);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t z = 0; z < outWidth; ++z)
            for (uint32_t x = 0; x < outWidth; ++x) {
                glm::vec3 sum(0);
                for (int v = -1; v <= 1; ++v)
                    for (int u = -1; u <= 1; ++u) {
                        const int sx = std::clamp(int(x * 2) + u, 0, int(width) - 1),
                                  sz = std::clamp(int(z * 2) + v, 0, int(width) - 1);
                        sum += glm::vec3(input[layer * width * width + sz * width + sx]) *
                               float((u == 0 ? 2 : 1) * (v == 0 ? 2 : 1));
                    }
                auto n = glm::vec4(glm::normalize(sum), 0);
                // GPU stores each intermediate level in RGBA16F; preserve that
                // rounding.
                n = glm::vec4(glm::unpackHalf2x16(glm::packHalf2x16(glm::vec2(n))),
                              glm::unpackHalf2x16(glm::packHalf2x16(glm::vec2(n.z, n.w))));
                output[layer * outWidth * outWidth + z * outWidth + x] = n;
            }
    return output;
}
void normalPyramid(Renderer &tiled, RendererOptions options, const Camera &camera, SceneSettings scene) {
    TerrainRequest centre{{1, 1}, {17, 64, 25, .22f, .3f}};
    auto batch = terrainNeighbourhood(centre);
    tiled.requestTerrainBatch(batch);
    settle(tiled, camera, scene);
    options.lod.enabled = false;
    options.terrainGrid = {96, .5f};
    options.terrainCapacity = 96;
    options.terrainChunkCapacity = 1;
    Renderer reference(options);
    reference.requestTerrain({{0, 0}, centre.settings});
    reference.render(camera, scene);
    auto continuous = reference.readTerrainNormals();
    uint32_t width = 193;
    for (uint32_t level = 0; level < 4; ++level) {
        const uint32_t active = (64u >> level) + 1, offset = 64u >> level;
        const auto centreNormals = tiled.readTerrainNormals(4, level);
        require(centreNormals.size() == size_t(active) * active * 2,
                "Readback strips padding at every level");
        for (uint32_t layer = 0; layer < 2; ++layer)
            for (uint32_t z = 0; z < active; ++z)
                for (uint32_t x = 0; x < active; ++x) {
                    const auto a = centreNormals[layer * active * active + z * active + x];
                    const auto b = continuous[layer * width * width + (z + offset) * width + x + offset];
                    near(glm::length(a - b), 0, .002, "Independent continuous-field normal filter reference");
                    near(glm::length(glm::vec3(a)), 1, .001, "Filtered normals remain unit length");
                }
        std::array<std::vector<glm::vec4>, 9> normals;
        for (uint32_t slot = 0; slot < 9; ++slot)
            normals[slot] = tiled.readTerrainNormals(slot, level);
        for (uint32_t z = 0; z < 3; ++z)
            for (uint32_t x = 0; x < 3; ++x) {
                const uint32_t slot = z * 3 + x;
                for (uint32_t layer = 0; layer < 2; ++layer)
                    for (uint32_t v = 0; v < active; ++v) {
                        const auto base = layer * active * active;
                        if (x < 2)
                            near(glm::length(normals[slot][base + v * active + active - 1] -
                                             normals[slot + 1][base + v * active]),
                                 0, .002, "Filtered X seam");
                        if (z < 2)
                            near(glm::length(normals[slot][base + (active - 1) * active + v] -
                                             normals[slot + 3][base + v]),
                                 0, .002, "Filtered Z seam");
                    }
            }
        if (level < 3) {
            continuous = filter(continuous, width);
            width = (width - 1) / 2 + 1;
        }
    }
    require(reference.validationErrors() == 0, "Reference validation clean");
    // Reverse generation order; both height and filtered normal identities
    // survive slot relocation.
    const auto before = tiled.readTerrainNormals(0, 2);
    std::reverse(batch.chunks.begin(), batch.chunks.end());
    tiled.requestTerrainBatch(batch);
    settle(tiled, camera, scene);
    const auto after = tiled.readTerrainNormals(8, 2);
    require(before == after, "LOD normal generation order independent");
}
void cameraPaths(Renderer &renderer, Outputs &outputs) {
    Camera camera;
    camera.chunk = {1, 1};
    camera.local = {0, 14, 0};
    camera.pitch = -1.5707963f;
    SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.shading = ShadingMode::Legacy;
    scene.frustumCulling = false;
    TerrainRequest request{{1, 1}, {17, 64, 0, .22f, .3f}};
    renderer.requestTerrainBatch(terrainNeighbourhood(request));
    settle(renderer, camera, scene);
    const auto generations = renderer.terrainGenerationCount();
    const auto memory = renderer.terrainAllocatedBytes();
    bool mixed = false, reduced = false;
    for (float position : {-8.f, -.01f, .01f, 4.f, 8.f, 12.f, 16.f, 24.f}) {
        camera.local.x = position;
        camera.local.z = 8;
        renderer.render(camera, scene);
        const auto stats = renderer.terrainRenderStats();
        uint32_t levels = 0;
        for (auto n : stats.perLevel)
            levels += n != 0;
        mixed |= levels > 1;
        const auto withoutCull = capture(renderer, outputs.path / "unculled.ppm");
        scene.frustumCulling = true;
        renderer.render(camera, scene);
        const auto pixels = capture(renderer, outputs.path / "culled.ppm");
        require(pixels == withoutCull, "Frustum culling preserves visible morphed surface");
        scene.opaqueDepthPrepass = true;
        renderer.render(camera, scene);
        require(capture(renderer, outputs.path / "depth-prepass.ppm") == pixels,
                "depth prepass preserves mixed-level morphed surface");
        scene.opaqueDepthPrepass = false;
        const auto lodTriangles = renderer.terrainRenderStats().triangles;
        scene.lod = false;
        renderer.render(camera, scene);
        const auto full = capture(renderer, outputs.path / "full.ppm");
        reduced |= lodTriangles < renderer.terrainRenderStats().triangles;
        require(pixels == full, "Flat terrain is pixel-identical through mixed-LOD transitions");
        // Project pixels onto the owned flat fixture. Interior pixels must never
        // show sky.
        const auto inverse = glm::inverse(camera.viewProjection(float(renderer.width()) / renderer.height()));
        uint32_t checked = 0;
        for (uint32_t y = 2; y < renderer.height(); y += 7)
            for (uint32_t x = 2; x < renderer.width(); x += 7) {
                auto p = inverse * glm::vec4((x + .5f) / renderer.width() * 2 - 1,
                                             (y + .5f) / renderer.height() * 2 - 1, 1, 1);
                const auto direction = glm::vec3(p) / p.w;
                const auto hit = glm::vec3(camera.local) + direction * float(-camera.local.y / direction.y);
                const float wx = 16 + hit.x, wz = 16 + hit.z;
                if (wx > .1f && wx < 47.9f && wz > .1f && wz < 47.9f) {
                    const auto i = (size_t(y) * renderer.width() + x) * 3;
                    require(pixels[i] > 100 && pixels[i + 1] > 100, "No sky leaks inside mixed-LOD terrain");
                    ++checked;
                }
            }
        require(checked > 500, "Camera fixture inspects substantial visible terrain");
        scene.lod = true;
        scene.frustumCulling = false;
    }
    require(mixed && reduced, "Camera path exercises mixed levels and measurable reduction");
    // Freeze affects the distance observer, not the view or frustum.
    renderer.render(camera, scene);
    const auto selected = renderer.readTerrainPatches();
    scene.freezeLod = true;
    camera.local.x += 10;
    renderer.render(camera, scene);
    require(samePatches(selected, renderer.readTerrainPatches()), "Frozen LOD observer preserves selection");
    scene.frustumCulling = true;
    camera.pitch = 1.5f;
    renderer.render(camera, scene);
    require(renderer.terrainRenderStats().triangles == 0, "Frozen observer retains live visibility culling");
    camera.pitch = -1.5707963f;
    scene.freezeLod = false;
    scene.frustumCulling = false;
    scene.wireframe = true;
    scene.lodColours = true;
    renderer.render(camera, scene);
    const auto wire = capture(renderer, outputs.path / "wire.ppm");
    require(std::any_of(wire.begin(), wire.end(), [](unsigned char v) { return v > 150; }),
            "Wireframe diagnostic produces an image");
    require(renderer.terrainGenerationCount() == generations,
            "Camera/debug controls do not regenerate terrain");
    require(renderer.terrainAllocatedBytes() == memory, "Movement/debug controls reuse allocated GPU memory");
}
void boundReduction(Renderer &renderer, const RendererOptions &options, const Camera &camera,
                    const SceneSettings &scene) {
    TerrainRequest request{{-2, -3}, {31, 64, 180, .7f, .3f}};
    renderer.requestTerrainBatch(terrainNeighbourhood(request));
    settle(renderer, camera, scene);
    float expected = options.lod.firstRange;
    const uint32_t n = options.terrainGrid.cells, w = n + 1;
    for (uint32_t slot = 0; slot < 9; ++slot) {
        const auto samples = renderer.readTerrainSnapshot(slot);
        for (uint32_t step = 1; step * options.lod.patchCells <= n; step *= 2) {
            const auto side = step * options.lod.patchCells;
            for (uint32_t z = 0; z < n; z += side)
                for (uint32_t x = 0; x < n; x += side) {
                    float lo = INFINITY, hi = -INFINITY;
                    for (uint32_t v = z; v <= z + side; ++v)
                        for (uint32_t u = x; u <= x + side; ++u) {
                            const auto &sample = samples[v * w + u];
                            lo = std::min({lo, sample.base.w, sample.eroded.w});
                            hi = std::max({hi, sample.base.w, sample.eroded.w});
                        }
                    const float horizontal = side * options.terrainGrid.spacing;
                    expected = std::max(
                        expected,
                        2.01f * glm::length(glm::vec3(horizontal, hi - lo + .02f, horizontal)) / float(step));
                }
        }
    }
    near(renderer.terrainRenderStats().firstRange, expected, .002,
         "GPU bounds include every base/eroded height and CPU parent unions");
}
void curvedTransitions(Renderer &renderer, Outputs &outputs) {
    TerrainRequest request{{1, 1}, {37, 64, 12, .3f, .3f}};
    SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.shading = ShadingMode::Legacy;
    scene.frustumCulling = false;
    Camera observer;
    observer.chunk = {1, 1};
    observer.local = {-16, 8, 8};
    observer.pitch = -1.5f;
    renderer.requestTerrainBatch(terrainNeighbourhood(request));
    settle(renderer, observer, scene);
    auto selected = renderer.readTerrainPatches();
    double low = observer.local.x, high = low;
    bool found = false;
    for (int i = 1; i < 100; ++i) {
        observer.local.x = -16 + i * .5;
        renderer.render(observer, scene);
        if (!samePatches(selected, renderer.readTerrainPatches())) {
            high = observer.local.x;
            found = true;
            break;
        }
        low = observer.local.x;
    }
    require(found, "Curved fixture crosses an actual selection boundary");
    for (int i = 0; i < 12; ++i) {
        observer.local.x = (low + high) * .5;
        renderer.render(observer, scene);
        if (samePatches(selected, renderer.readTerrainPatches()))
            low = observer.local.x;
        else
            high = observer.local.x;
    }
    Camera view;
    view.chunk = {1, 1};
    view.local = {8, 35, 8};
    view.pitch = -1.5707963f;
    auto fixedView = [&](double x) {
        observer.local.x = x;
        scene.freezeLod = false;
        renderer.render(observer, scene);
        scene.freezeLod = true;
        renderer.render(view, scene);
        return capture(renderer, outputs.path / "transition.ppm");
    };
    const auto before = fixedView(low - .0001), after = fixedView(high + .0001);
    uint64_t sum = 0;
    uint32_t changed = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        const int difference = std::abs(int(before[i]) - int(after[i]));
        sum += difference;
        changed += difference > 4;
    }
    require(double(sum) / before.size() < .05 && changed < 100,
            "Curved geometry/lighting stays continuous across a selection boundary");
    scene.frustumCulling = true;
    renderer.render(view, scene);
    require(capture(renderer, outputs.path / "curved-culled.ppm") == after,
            "Conservative bounds preserve curved morphed geometry when culled");
    // Independent padded filters must agree even at negative and distant chunk
    // origins.
    for (auto origin : {ChunkCoord{-3, -2}, ChunkCoord{1000000, -1000000}}) {
        request.chunk = origin;
        renderer.requestTerrainBatch(terrainNeighbourhood(request));
        settle(renderer, view, scene);
        for (uint32_t level = 0; level < 4; ++level) {
            const auto left = renderer.readTerrainNormals(0, level),
                       right = renderer.readTerrainNormals(1, level);
            const auto below = renderer.readTerrainNormals(3, level);
            const uint32_t w = (64u >> level) + 1;
            for (uint32_t layer = 0; layer < 2; ++layer)
                for (uint32_t i = 0; i < w; ++i) {
                    const auto base = layer * w * w;
                    near(glm::length(left[base + i * w + w - 1] - right[base + i * w]), 0, .002,
                         "Distant padded X normal seam");
                    near(glm::length(left[base + (w - 1) * w + i] - below[base + i]), 0, .002,
                         "Distant padded Z normal seam");
                }
        }
    }
}
void metadata(Renderer &renderer, RendererOptions options) {
    Camera camera;
    SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.shading = ShadingMode::Legacy;
    TerrainRequest request{{-2, -3}, {1, 64, 10, .22f, .3f}};
    for (uint32_t i = 0; i < 12; ++i) {
        const TerrainGrid grid{i % 2 ? 64u : 16u, .5f};
        renderer.setTerrainGrid(grid);
        request.settings.seed = i + 1;
        request.settings.heightScale = i % 3 ? 10.f : 150.f;
        request.chunk = {i % 2 ? 1000000 : -2, -3};
        renderer.requestTerrainBatch(terrainNeighbourhood(request));
        camera.reset(grid, request.chunk, 70);
        renderer.render(camera, scene);
        require(!renderer.terrainRenderStats().boundsReady, "Regeneration immediately invalidates bounds");
        require(renderer.terrainRenderStats().triangles == uint64_t(grid.cells) * grid.cells * 2 * 9,
                "Pending neighbourhood is coherent full detail");
    }
    renderer.render(camera, scene);
    require(!renderer.terrainRenderStats().boundsReady,
            "Older frame's bounds cannot replace latest metadata");
    renderer.render(camera, scene);
    require(renderer.terrainRenderStats().boundsReady, "Current bounds consumed after owning frame fence");
    scene.lod = false;
    scene.frustumCulling = false;
    renderer.render(camera, scene);
    const auto full = renderer.terrainRenderStats().triangles;
    renderer.requestTerrain({request.chunk, request.settings});
    renderer.setTerrainGrid({16, .5f});
    renderer.render(camera, scene);
    require(renderer.terrainRenderStats().triangles == 512, "Shrunk batch uses only active slots");
    renderer.setTerrainGrid(options.terrainGrid);
    renderer.requestTerrainBatch(terrainNeighbourhood(request));
    settle(renderer, camera, scene);
    require(renderer.terrainRenderStats().boundsReady && renderer.terrainRenderStats().triangles < full,
            "Reactivated slots refresh bounds for new grid");
    const auto generations = renderer.terrainGenerationCount();
    renderer.setTerrainGrid({16, .5f});
    renderer.setTerrainGrid(options.terrainGrid);
    renderer.render(camera, scene);
    require(renderer.terrainRenderStats().boundsReady && renderer.terrainGenerationCount() == generations,
            "Cancelled grid edits preserve bounds and generation cache");
    auto single = terrainNeighbourhood(request);
    single.count = 1; // Retain the same slot-zero identity.
    renderer.requestTerrainBatch(single);
    renderer.setTerrainGrid({16, .5f});
    settle(renderer, camera, scene);
    renderer.setTerrainGrid(options.terrainGrid);
    settle(renderer, camera, scene);
    const auto beforeReactivation = renderer.terrainGenerationCount();
    renderer.requestTerrainBatch(terrainNeighbourhood(request));
    settle(renderer, camera, scene);
    require(renderer.terrainRenderStats().boundsReady,
            "Unchanged inactive data reacquires bounds after hierarchy rebuild");
    require(renderer.terrainGenerationCount() == beforeReactivation,
            "Metadata refresh does not regenerate unchanged inactive slots");
}
} // namespace
int main() {
    try {
        Outputs outputs;
        RendererOptions options;
        options.ui = false;
        options.requireValidation = true;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.terrainGrid = {32, .5f};
        options.terrainCapacity = 64;
        options.terrainChunkCapacity = 9;
        options.lod = {true, 8, 8};
        Renderer renderer(options);
        Camera camera;
        camera.reset(options.terrainGrid, {1, 1}, 30);
        SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        scene.shading = ShadingMode::Legacy;
        const auto memory = renderer.terrainAllocatedBytes();
        normalPyramid(renderer, options, camera, scene);
        cameraPaths(renderer, outputs);
        boundReduction(renderer, options, camera, scene);
        curvedTransitions(renderer, outputs);
        metadata(renderer, options);
        require(renderer.terrainAllocatedBytes() == memory, "Generation/grid changes retain bounded memory");
        renderer.waitIdle();
        require(renderer.validationErrors() == 0, "LOD Vulkan validation clean");
        std::cout << "Terrain LOD GPU tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

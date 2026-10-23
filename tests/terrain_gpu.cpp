#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace ow;
struct OutputDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openworld-terrain-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    OutputDirectory() { std::filesystem::create_directories(path); }
    ~OutputDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
std::vector<unsigned char> pixels(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::string magic;
    unsigned w = 0, h = 0, max = 0;
    file >> magic >> w >> h >> max;
    file.get();
    require(magic == "P6" && w == 1280 && h == 800 && max == 255, "Capture dimensions/format");
    std::vector<unsigned char> out(size_t(w) * h * 3);
    require(bool(file.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size()))),
            "Capture payload");
    return out;
}
void checkSamples(const std::vector<TerrainSample> &samples) {
    for (const auto &sample : samples)
        for (const auto v : {sample.base, sample.eroded}) {
            require(std::isfinite(v.w), "Finite height");
            near(glm::length(glm::vec3(v)), 1, 2e-5, "Unit normal");
            require(v.y > 0, "Upward normal");
        }
}
// Rasterize sparse pixel centres on the CPU to verify our coordinate mapping,
// texture-layer selection, normal filtering, and draw-only comparison end to
// end. Full-precision normals are the reference; allow 3 byte levels for
// RGBA16F, sampler interpolation precision, and sRGB conversion differences
// across drivers.
void checkLighting(Renderer &renderer, const TerrainGrid &grid, const std::vector<TerrainSample> &samples,
                   const std::filesystem::path &capture, bool expectVisibleDifference = false,
                   const std::vector<TerrainSample> *denseNormals = nullptr, ChunkCoord chunk = {}) {
    Camera camera;
    camera.chunk = chunk;
    double highest = 0;
    for (const auto &s : samples)
        highest = std::max(highest, double(std::max(s.base.w, s.eroded.w)));
    camera.local = {grid.extent() * 0.5, highest + grid.extent() * 2, grid.extent() * 0.5};
    camera.pitch = -1.5707f;
    const auto view = camera.viewProjection(1.6f);
    const auto indices = terrainIndices(grid);
    const auto generationCount = renderer.terrainGenerationCount();
    size_t totalChecked = 0;
    size_t changedPixels = 0;
    for (bool eroded : {false, true}) {
        struct Hit {
            float depth = 1;
            glm::vec2 grid{};
            glm::vec3 vertexNormal{};
        };
        constexpr int step = 16, columns = 1280 / step, rows = 800 / step;
        std::vector<Hit> hits(columns * rows);
        auto sample = [&](uint32_t x, uint32_t z) {
            const auto &s = samples[size_t(z) * (grid.cells + 1) + x];
            return eroded ? s.eroded : s.base;
        };
        auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };
        for (size_t i = 0; i < indices.size(); i += 3) {
            glm::vec2 screen[3], coords[3];
            glm::vec3 normals[3];
            glm::vec4 clip[3];
            for (int j = 0; j < 3; ++j) {
                const auto vertex = indices[i + j];
                const uint32_t x = vertex % (grid.cells + 1), z = vertex / (grid.cells + 1);
                coords[j] = {x, z};
                const auto s = sample(x, z);
                normals[j] = glm::vec3(s);
                clip[j] = view * glm::vec4(glm::vec3(x * grid.spacing, s.w, z * grid.spacing) -
                                               glm::vec3(camera.local),
                                           1);
                screen[j] = (glm::vec2(clip[j]) / clip[j].w + 1.0f) * glm::vec2(640, 400);
            }
            const float area = cross(screen[1] - screen[0], screen[2] - screen[0]);
            if (std::abs(area) < 1e-5f)
                continue;
            const auto lower = glm::min(screen[0], glm::min(screen[1], screen[2]));
            const auto upper = glm::max(screen[0], glm::max(screen[1], screen[2]));
            const int minX = std::clamp(int(lower.x) / step, 0, columns - 1);
            const int maxX = std::clamp(int(upper.x) / step, 0, columns - 1);
            const int minY = std::clamp(int(lower.y) / step, 0, rows - 1);
            const int maxY = std::clamp(int(upper.y) / step, 0, rows - 1);
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x) {
                    const glm::vec2 p(x * step + 0.5f, y * step + 0.5f);
                    const float a = cross(screen[1] - p, screen[2] - p) / area;
                    const float b = cross(screen[2] - p, screen[0] - p) / area;
                    const float c = 1 - a - b;
                    if (std::min({a, b, c}) < 0)
                        continue;
                    auto &hit = hits[y * columns + x];
                    const float depth =
                        a * clip[0].z / clip[0].w + b * clip[1].z / clip[1].w + c * clip[2].z / clip[2].w;
                    if (depth >= hit.depth)
                        continue;
                    hit.depth = depth;
                    glm::vec3 weights(a / clip[0].w, b / clip[1].w, c / clip[2].w);
                    weights /= weights.x + weights.y + weights.z;
                    hit.grid = coords[0] * weights.x + coords[1] * weights.y + coords[2] * weights.z;
                    hit.vertexNormal =
                        normals[0] * weights.x + normals[1] * weights.y + normals[2] * weights.z;
                }
        }
        std::vector<unsigned char> legacy;
        for (bool filtered : {false, true}) {
            SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
            scene.gtao.mode = ow::GtaoMode::Off;
            scene.shading = ShadingMode::Legacy;
            scene.eroded = eroded;
            scene.filteredNormals = filtered;
            require(renderer.render(camera, scene), "Lighting comparison draw");
            renderer.capture(capture);
            const auto image = pixels(capture);
            if (!filtered)
                legacy = image;
            else
                for (size_t i = 0; i < image.size(); i += 3)
                    changedPixels += std::abs(int(image[i]) - int(legacy[i])) > 2;
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < columns; ++x) {
                    const auto &hit = hits[y * columns + x];
                    if (hit.depth == 1)
                        continue;
                    glm::vec3 normal = hit.vertexNormal;
                    if (filtered) {
                        const uint32_t factor = denseNormals ? 2 : 1;
                        const auto &normalSamples = denseNormals ? *denseNormals : samples;
                        const uint32_t cells = grid.cells * factor;
                        const auto p =
                            glm::clamp(hit.grid * float(factor), glm::vec2(0), glm::vec2(float(cells)));
                        const auto ix = uint32_t(p.x), iz = uint32_t(p.y);
                        const auto nx = std::min(ix + 1, cells), nz = std::min(iz + 1, cells);
                        auto n = [&](uint32_t x, uint32_t z) {
                            const auto &v = normalSamples[size_t(z) * (cells + 1) + x];
                            return glm::vec3(eroded ? v.eroded : v.base);
                        };
                        normal = glm::mix(glm::mix(n(ix, iz), n(nx, iz), p.x - ix),
                                          glm::mix(n(ix, nz), n(nx, nz), p.x - ix), p.y - iz);
                    }
                    const float light =
                        0.22f +
                        0.78f * std::max(0.0f, glm::dot(glm::normalize(normal),
                                                        glm::normalize(glm::vec3(-0.6f, 0.8f, 0.35f))));
                    const auto color = glm::vec3(0.40f, 0.46f, 0.38f) * light;
                    for (int channel = 0; channel < 3; ++channel) {
                        const float linear = color[channel];
                        const float srgb = linear <= 0.0031308f
                                               ? linear * 12.92f
                                               : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
                        near(image[(size_t(y * step) * 1280 + x * step) * 3 + channel],
                             std::round(srgb * 255), 3,
                             filtered ? "Filtered per-pixel normal lighting"
                                      : "Vertex-normal lighting comparison");
                    }
                    ++totalChecked;
                }
        }
    }
    require(totalChecked > 100, "Lighting comparison covers visible terrain");
    if (expectVisibleDifference)
        require(changedPixels > 100, "Filtered normals visibly differ on curved terrain");
    require(renderer.terrainGenerationCount() == generationCount,
            "Lighting/variant toggles keep cached terrain");
    std::cout << "Verified " << totalChecked << " shaded samples against CPU lighting reference\n";
}

void denseNormalTests(const std::filesystem::path &output) {
    RendererOptions options;
    options.ui = false;
    options.requireValidation = true;
    options.shaderDirectory = OPENWORLD_SHADER_DIR;
    options.terrainGrid = {32, 0.5f};
    options.terrainCapacity = 40; // active image smaller than capacity
    options.normalSubdivisions = 2;
    Renderer renderer(options);
    auto referenceOptions = options;
    referenceOptions.terrainGrid = {64, 0.25f};
    referenceOptions.terrainCapacity = 64;
    referenceOptions.normalSubdivisions = 1;
    Renderer reference(referenceOptions);
    Camera camera;
    camera.reset(options.terrainGrid);
    SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
    scene.gtao.mode = ow::GtaoMode::Off;
    scene.shading = ShadingMode::Legacy;
    renderer.render(camera, scene);
    reference.render(camera, scene);
    const auto mesh = renderer.readTerrainSnapshot(), dense = reference.readTerrainSnapshot();
    require(mesh.size() == 33 * 33 && renderer.readTerrainIndices().size() == 32 * 32 * 6,
            "Finer shading does not increase mesh density");
    for (uint32_t z = 0; z <= 32; ++z)
        for (uint32_t x = 0; x <= 32; ++x) {
            const auto &a = mesh[z * 33 + x], &b = dense[(z * 2) * 65 + x * 2];
            near(a.eroded.w, b.eroded.w, 1e-4, "Mesh samples shared dense terrain heights");
            near(glm::length(a.eroded - b.eroded), 0, 1e-4, "Mesh normals use correct dense differences");
        }
    checkLighting(renderer, options.terrainGrid, mesh, output / "dense-lighting.ppm", false, &dense);
    camera.reset(options.terrainGrid, {}, mesh[16 * 33 + 16].eroded.w);
    const auto count = renderer.terrainGenerationCount();
    scene.normalView = true;
    renderer.render(camera, scene);
    renderer.capture(output / "normals.ppm");
    scene.wireframe = true;
    renderer.render(camera, scene);
    renderer.capture(output / "wire.ppm");
    require(pixels(output / "normals.ppm") != pixels(output / "wire.ppm"),
            "Wire overlay changes diagnostic view");
    require(renderer.terrainGenerationCount() == count, "Debug views do not regenerate");
    std::vector<glm::vec2> plane(67 * 67);
    for (uint32_t z = 0; z < 67; ++z)
        for (uint32_t x = 0; x < 67; ++x)
            plane[z * 67 + x] = {7, 2 * (float(x) - 1) * 0.25f - 3 * (float(z) - 1) * 0.25f + 9};
    const auto fixture = renderer.normalsForFixture(plane);
    for (uint32_t z = 0; z <= 32; ++z)
        for (uint32_t x = 0; x <= 32; ++x) {
            const auto &v = fixture[z * 33 + x];
            near(v.eroded.w, 2 * x * 0.5 - 3 * z * 0.5 + 9, 1e-5,
                 "Dense fixture writes correct mesh samples");
            near(glm::length(glm::vec3(v.eroded) - glm::normalize(glm::vec3(-2, 1, 3))), 0, 1e-6,
                 "Dense fixture normal");
        }
    TerrainRequest request;
    for (int i = 0; i < 8; ++i) {
        renderer.setTerrainGrid({i % 2 ? 32u : 8u, 0.5f});
        request.settings.creaseRounding = i % 2 ? 0.3f : 0.0f;
        renderer.requestTerrain(request);
        renderer.render(camera, scene);
    }
    renderer.waitIdle();
    checkSamples(renderer.readTerrainSnapshot());
    require(renderer.validationErrors() == 0 && reference.validationErrors() == 0, "Dense-normal validation");
}

// Guard the measured fix on one deterministic high-curvature patch. This is a
// geometric-error test against independent dense evaluations, not a fixed
// image.
void creaseSamplingRegression() {
    RendererOptions options;
    options.ui = false;
    options.requireValidation = true;
    options.shaderDirectory = OPENWORLD_SHADER_DIR;
    options.terrainGrid = {128, 0.125f};
    options.terrainCapacity = 128;
    options.normalSubdivisions = 1;
    Renderer renderer(options);
    TerrainRequest request;
    request.chunk = {165, 165}; // Owned mountain fixture around world (2640, 2640).
    request.settings.ridgeStrength = request.settings.baseDetail = 1;
    Camera camera;
    camera.reset(options.terrainGrid, request.chunk);
    double rms[2]{}, maximum[2]{};
    for (int mode = 0; mode < 2; ++mode) {
        request.settings.creaseRounding = mode ? 0.3f : 0.0f;
        renderer.requestTerrain(request);
        renderer.setTerrainGrid({128, 0.125f});
        renderer.render(camera, {});
        const auto fine = renderer.readTerrainSnapshot();
        renderer.setTerrainGrid({32, 0.5f});
        renderer.render(camera, {});
        const auto coarse = renderer.readTerrainSnapshot();
        double sum = 0;
        for (uint32_t z = 0; z <= 128; ++z)
            for (uint32_t x = 0; x <= 128; ++x) {
                const uint32_t ix = std::min(x / 4, 31u), iz = std::min(z / 4, 31u);
                const double fx = x / 4.0 - ix, fz = z / 4.0 - iz;
                const double a = coarse[iz * 33 + ix].eroded.w, b = coarse[iz * 33 + ix + 1].eroded.w,
                             c = coarse[(iz + 1) * 33 + ix].eroded.w,
                             d = coarse[(iz + 1) * 33 + ix + 1].eroded.w;
                const double interpolated = fx + fz <= 1 ? a + fx * (b - a) + fz * (c - a)
                                                         : d + (1 - fx) * (c - d) + (1 - fz) * (b - d);
                const double error = std::abs(interpolated - fine[z * 129 + x].eroded.w);
                maximum[mode] = std::max(maximum[mode], error);
                sum += error * error;
            }
        rms[mode] = std::sqrt(sum / fine.size());
    }
    require(rms[1] < rms[0] * 0.4 && maximum[1] < 0.25,
            "Rounded creases remain representable on the 0.5 m grid");
    require(renderer.validationErrors() == 0, "Sampling regression validation");
    std::cout << "Crease geometry RMS: " << rms[0] << " -> " << rms[1] << " m\n";
}

// The reported ridge failed without LOD: retain the original world patch and
// both erosion configurations, using independently evaluated dense heights.
void ridgeDirectionRegression() {
    RendererOptions options;
    options.ui = false;
    options.requireValidation = true;
    options.shaderDirectory = OPENWORLD_SHADER_DIR;
    options.terrainGrid = {256, 0.0625f};
    options.terrainCapacity = 256;
    options.normalSubdivisions = 1;
    Renderer renderer(options);
    TerrainRequest request{{-406, 42}, {}}; // 16 m at (-6496,672).
    request.settings.seed = 34434;
    request.settings.ridgeStrength = request.settings.baseDetail = 1;
    Camera camera;
    camera.reset(options.terrainGrid, request.chunk, 760);
    const double previousRms[2] = {0.33468796, 0.18862842};
    for (int setting = 0; setting < 2; ++setting) {
        request.settings.erosionWavelength = setting ? 1572.f : 1024.f;
        request.settings.erosionStrength = setting ? .229f : .22f;
        renderer.requestTerrain(request);
        renderer.setTerrainGrid({256, .0625f});
        renderer.render(camera, {});
        const auto fine = renderer.readTerrainSnapshot();
        renderer.setTerrainGrid({32, .5f});
        renderer.render(camera, {});
        const auto coarse = renderer.readTerrainSnapshot();
        double sum = 0, maximum = 0;
        for (uint32_t z = 0; z <= 256; ++z)
            for (uint32_t x = 0; x <= 256; ++x) {
                const uint32_t ix = std::min(x / 8, 31u), iz = std::min(z / 8, 31u);
                const double fx = x / 8.0 - ix, fz = z / 8.0 - iz;
                const double a = coarse[iz * 33 + ix].eroded.w, b = coarse[iz * 33 + ix + 1].eroded.w,
                             c = coarse[(iz + 1) * 33 + ix].eroded.w,
                             d = coarse[(iz + 1) * 33 + ix + 1].eroded.w;
                const double mesh = fx + fz <= 1 ? a + fx * (b - a) + fz * (c - a)
                                                 : d + (1 - fx) * (c - d) + (1 - fz) * (b - d);
                const double error = std::abs(mesh - fine[z * 257 + x].eroded.w);
                sum += error * error;
                maximum = std::max(maximum, error);
            }
        const double rms = std::sqrt(sum / fine.size());
        std::cout << "Ridge setting " << setting << " RMS=" << rms << " max=" << maximum << " m\n";
        require(rms < previousRms[setting] * .2 && maximum < .5,
                "Ridge erosion is representable without sawtooth geometry");
    }
    require(renderer.validationErrors() == 0, "Ridge regression Vulkan validation");
}

int main() {
    try {
        RendererOptions options;
        options.ui = false;
        options.requireValidation = true;
        options.shaderDirectory = OPENWORLD_SHADER_DIR;
        options.normalSubdivisions = 1; // Existing fixtures exercise mesh-rate normals.
        options.terrainCapacity = 64;
        options.terrainGrid = {33, 4}; // non-multiple workgroups + halo
        Renderer renderer(options);
        Camera camera;
        camera.reset(options.terrainGrid, {}, 30);
        SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        scene.shading = ShadingMode::Legacy;
        TerrainRequest request;
        request.chunk = {20, 20}; // Regional mountain interior, not the origin plain.
        request.settings.ridgeStrength = request.settings.baseDetail = 1; // Curved lighting fixture.
        camera.chunk = request.chunk;
        request.settings.erosionWavelength = 128;
        request.settings.wavelength = 128;
        request.settings.heightScale = 60;
        renderer.requestTerrain(request);
        auto draw = [&] { require(renderer.render(camera, scene), "Offscreen submission"); };
        draw();
        require(renderer.terrainGenerationCount() == 1, "Initial generation");
        auto first = renderer.readTerrainSnapshot();
        require(first.size() == 34 * 34, "Readback size");
        checkSamples(first);
        double erosionDifference = 0;
        for (const auto &v : first)
            erosionDifference += std::abs(v.eroded.w - v.base.w);
        require(erosionDifference > 1, "Erosion actually affects terrain");
        OutputDirectory outputs;
        denseNormalTests(outputs.path);
        creaseSamplingRegression();
        ridgeDirectionRegression();
        checkLighting(renderer, options.terrainGrid, first, outputs.path / "lighting.ppm", true, nullptr,
                      request.chunk);
        draw();
        renderer.capture(outputs.path / "eroded.ppm");
        scene.eroded = false;
        draw();
        renderer.capture(outputs.path / "base.ppm");
        const auto a = pixels(outputs.path / "eroded.ppm"), b = pixels(outputs.path / "base.ppm");
        size_t changed = 0, foreground = 0;
        for (size_t i = 0; i < a.size(); i += 3) {
            changed += a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2];
            foreground += a[i] != a[0] || a[i + 1] != a[1] || a[i + 2] != a[2];
        }
        require(changed > 100 && foreground > 1000, "Both variants rasterize meaningful terrain");
        CameraInput movement;
        movement.movement = {1, 0, 1};
        for (int i = 0; i < 12; ++i) {
            camera.update(movement, 0.01f, options.terrainGrid);
            scene.eroded = !scene.eroded;
            renderer.requestTerrain(request);
            draw();
        }
        require(renderer.terrainGenerationCount() == 1, "Camera and display edits keep cached terrain");
        request.settings.erosionStrength = 0;
        renderer.requestTerrain(request);
        draw();
        for (const auto &v : renderer.readTerrainSnapshot())
            near(v.eroded.w, v.base.w, 0, "Zero strength identity");
        request.settings.erosionStrength = 0.22f;
        request.settings.seed = 2;
        renderer.requestTerrain(request);
        draw();
        auto changedSeed = renderer.readTerrainSnapshot();
        double difference = 0;
        for (size_t i = 0; i < first.size(); ++i)
            difference += std::abs(first[i].base.w - changedSeed[i].base.w);
        require(difference > 10, "Seed changes terrain");
        request.settings.seed = 1;
        renderer.requestTerrain(request);
        draw();
        auto repeated = renderer.readTerrainSnapshot();
        for (size_t i = 0; i < first.size(); ++i) {
            near(first[i].base.w, repeated[i].base.w, 0, "Same-device base repeatability");
            near(first[i].eroded.w, repeated[i].eroded.w, 0, "Same-device eroded repeatability");
        }
        // Shared edge includes gradients obtained from independent halos.
        request.chunk = {-1, 0};
        renderer.requestTerrain(request);
        draw();
        auto left = renderer.readTerrainSnapshot();
        request.chunk = {0, 0};
        renderer.requestTerrain(request);
        draw();
        auto right = renderer.readTerrainSnapshot();
        for (uint32_t z = 0; z <= 33; ++z) {
            const auto &l = left[z * 34 + 33];
            const auto &r = right[z * 34];
            near(l.base.w, r.base.w, 0.002, "Base edge");
            near(l.eroded.w, r.eroded.w, 0.002, "Eroded edge");
            near(glm::length(glm::vec3(l.eroded - r.eroded)), 0, 0.002, "Edge normal");
        }
        request.chunk = {1000000, -1000000};
        renderer.requestTerrain(request);
        camera.reset(options.terrainGrid, request.chunk, 30);
        draw();
        auto distant = renderer.readTerrainSnapshot();
        checkSamples(distant);
        double distantVariation = 0;
        for (const auto &v : distant)
            distantVariation += std::abs(v.base.w - distant.front().base.w);
        require(distantVariation > 10, "Distant coordinates retain local terrain variation");
        request.chunk = {};
        camera.reset(options.terrainGrid, {}, 30);
        // No diagnostic waits between these updates: exercise actual in-flight
        // reuse.
        const auto count = renderer.terrainGenerationCount();
        for (uint32_t i = 0; i < 24; ++i) {
            request.settings.seed = i + 8;
            renderer.requestTerrain(request);
            draw();
        }
        renderer.waitIdle();
        require(renderer.terrainGenerationCount() == count + 24, "Each submitted edit generated once");
        // Our normal pass consumes synthetic heights, not a duplicated reference
        // algorithm.
        std::vector<glm::vec2> fixture(36 * 36);
        for (uint32_t z = 0; z < 36; ++z)
            for (uint32_t x = 0; x < 36; ++x)
                fixture[z * 36 + x] = {7, 2 * (float(x) - 1) * 4 - 3 * (float(z) - 1) * 4 + 9};
        auto normals = renderer.normalsForFixture(fixture);
        const auto expected = glm::normalize(glm::vec3(-2, 1, 3));
        for (const auto &v : normals) {
            near(glm::length(glm::vec3(v.base) - glm::vec3(0, 1, 0)), 0, 1e-6, "Flat normal");
            near(glm::length(glm::vec3(v.eroded) - expected), 0, 1e-6, "Planar normal with metre spacing");
        }
        draw();
        checkSamples(renderer.readTerrainSnapshot()); // fixture invalidates cached terrain
        require(renderer.terrainGenerationCount() == count + 25, "Diagnostic fixture forces restoration");
        // Footprint changes at the demo's fixed spacing reuse capacity-sized
        // storage.
        request = {{0, 0}, {17, 128, 60, 0.22f}};
        renderer.requestTerrain(request);
        for (uint32_t cells : {8u, 33u, 64u}) {
            const TerrainGrid grid{cells, 0.5f};
            renderer.setTerrainGrid(grid);
            bool rejectedPendingRead = false;
            try {
                renderer.readTerrainSnapshot();
            } catch (const std::runtime_error &) {
                rejectedPendingRead = true;
            }
            require(rejectedPendingRead, "Readback must not mislabel old grid data");
            camera.reset(grid, request.chunk, 30);
            draw();
            const auto samples = renderer.readTerrainSnapshot();
            require(samples.size() == size_t(grid.cells + 1) * (grid.cells + 1),
                    "Readback uses active extent");
            checkSamples(samples);
            require(renderer.readTerrainIndices() == terrainIndices(grid), "GPU topology bounds and winding");
            checkLighting(renderer, grid, samples, outputs.path / "resized-lighting.ppm");
            const auto generated = renderer.terrainGenerationCount();
            renderer.setTerrainGrid(grid);
            draw();
            require(renderer.terrainGenerationCount() == generated, "Identical grid is cached");
        }
        rejects([&] { renderer.setTerrainGrid({65, 1}); }, "Preallocated capacity enforced");
        require(renderer.readTerrainSnapshot().size() == 65 * 65, "Rejected grid leaves current data intact");
        const auto resizeCount = renderer.terrainGenerationCount();
        for (uint32_t i = 0; i < 24; ++i) {
            const TerrainGrid grid = i % 2 ? TerrainGrid{64, 0.5f} : TerrainGrid{8, 0.5f};
            renderer.setTerrainGrid(grid);
            camera.reset(grid, request.chunk, 30);
            draw(); // No readback/idle between resizes; test previous draw -> compute
                    // -> next draw.
        }
        renderer.waitIdle();
        require(renderer.terrainGenerationCount() == resizeCount + 24,
                "In-flight grid changes regenerate once");
        require(renderer.readTerrainIndices() == terrainIndices({64, 0.5f}),
                "Final topology after repeated resizes");
        checkSamples(renderer.readTerrainSnapshot());
        require(renderer.validationErrors() == 0, "Vulkan validation errors");
        std::cout << "GPU terrain tests passed, validation errors: 0\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
namespace {
thread_local uint64_t allocations = 0;
}
void *operator new(size_t n) {
    ++allocations;
    if (auto *p = std::malloc(std::max(n, size_t(1))))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
void *operator new(size_t n, std::align_val_t a) {
    ++allocations;
    void *p = nullptr;
    if (!posix_memalign(&p, std::max(size_t(a), sizeof(void *)), std::max(n, size_t(1))))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { std::free(p); }
using namespace ow;
int main(int argc, char **argv) {
    try {
        std::filesystem::path out = "/tmp/tree-review", directory = OPENWORLD_TREE_LIBRARY_DIR;
        std::string sceneName = "specimen", sequence = "stationary";
        uint32_t frames = 600, warmup = 120, width = 1920, height = 1080, asset = 2, capacity = 0, span = 0;
        bool quick = false, validation = false, enabled = true, rgba = false, vegetation = false, taa = true,
             shadows = true;
        int lod = -1;
        float sunY = .8f, distance = 0, yaw = 0;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (++i == argc)
                    throw std::runtime_error("Missing probe argument");
                return std::string(argv[i]);
            };
            if (a == "--output")
                out = value();
            else if (a == "--assets")
                directory = value();
            else if (a == "--scene")
                sceneName = value();
            else if (a == "--sequence")
                sequence = value();
            else if (a == "--span")
                span = std::stoul(value());
            else if (a == "--no-shadows")
                shadows = false;
            else if (a == "--capacity")
                capacity = std::stoul(value());
            else if (a == "--rgba")
                rgba = true;
            else if (a == "--vegetation")
                vegetation = true;
            else if (a == "--taa-off")
                taa = false;
            else if (a == "--asset")
                asset = std::stoul(value());
            else if (a == "--lod")
                lod = std::stoi(value());
            else if (a == "--frames")
                frames = std::stoul(value());
            else if (a == "--warmup")
                warmup = std::stoul(value());
            else if (a == "--width")
                width = std::stoul(value());
            else if (a == "--height")
                height = std::stoul(value());
            else if (a == "--distance")
                distance = std::stof(value());
            else if (a == "--yaw")
                yaw = std::stof(value());
            else if (a == "--sun")
                sunY = std::stof(value());
            else if (a == "--quick")
                quick = true;
            else if (a == "--validation")
                validation = true;
            else if (a == "--no-trees")
                enabled = false;
            else
                throw std::runtime_error("Unknown tree probe option: " + a);
        }
        if (quick) {
            if (width == 1920)
                width = 640;
            if (height == 1080)
                height = 360;
            if (frames == 600)
                frames = 8;
            if (warmup == 120)
                warmup = 4;
        }
        if (sceneName != "specimen" && sceneName != "canopy" && sceneName != "edge" &&
            sceneName != "panorama" && sceneName != "stream")
            throw std::runtime_error("Scene must be specimen, canopy, edge, panorama or stream");
        auto library = std::make_shared<TreeLibrary>(readTreeLibrary(directory / "library.bin"));
        if (asset >= library->assets.size())
            throw std::runtime_error("Invalid tree asset");
        RendererOptions o;
        o.ui = false;
        o.validation = validation;
        o.requireValidation = validation;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.width = width;
        o.height = height;
        o.terrainGrid = {32, .5f};
        o.terrainCapacity = 32;
        o.shadowCapacity.resolution = quick ? 512 : 2048;
        o.trees.directory = directory;
        o.trees.library = library;
        o.trees.streaming.maximumDistance = 4000;
        uint32_t side = sceneName == "specimen" ? 1 : sceneName == "panorama" ? (quick ? 8 : 128) : 4;
        if (span) {
            if (span > 128)
                throw std::invalid_argument("Probe span exceeds 128 cells");
            side = span;
        }
        o.trees.streaming.cellCapacity = sceneName == "stream" ? 512 : std::max(16u, side * side);
        o.trees.streaming.budgetBytes = 768ull << 20;
        if (sceneName == "stream") {
            o.streaming.enabled = o.streaming.hierarchical = true;
            o.streaming.diskCache = false;
            o.streaming.maximumDistance = quick ? 256 : 4000;
            o.streaming.gpuBytes = 128ull << 20;
            o.streaming.hostBytes = 128ull << 20;
            o.streaming.shadowCasterBytes = 16ull << 20;
            o.lod = {true, 8, 4096};
        } else {
            for (uint32_t z = 0; z < side; ++z)
                for (uint32_t x = 0; x < side; ++x) {
                    TreeFixtureCell cell;
                    cell.key = {int64_t(x) - side / 2, int64_t(z) - side / 2};
                    uint32_t count = sceneName == "specimen" ? 1 : 48;
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t h = treeCandidateHash(cell.key, 91, i);
                        auto random = [&](uint32_t salt) {
                            return float(treeHash(h + salt) >> 8) / 16777216.f;
                        };
                        auto &t = cell.instances[i];
                        t.rootScale = {sceneName == "specimen" ? 0 : (i % 8 + random(1)) * 8, 0,
                                       sceneName == "specimen" ? 0 : (i / 8 + random(2)) * 10,
                                       .9f + random(3) * .2f};
                        t.identity = h;
                        t.yaw = random(4) * 6.2831853;
                        t.variation = random(5);
                        uint32_t species = (x / 3 + z / 3) % 4;
                        t.assetCell = sceneName == "specimen" ? asset : species * 6 + 2 + (h % 2);
                        if (sceneName == "canopy" && std::abs(t.rootScale.x + cell.key.x * 64) < 3)
                            t.rootScale.w = 0;
                        if (sceneName == "edge" && cell.key.x >= 0)
                            t.rootScale.w = 0;
                    }
                    o.trees.fixture.push_back(cell);
                }
        }
        if (capacity)
            o.trees.streaming.cellCapacity = capacity;
        o.forceRgbaMaterials = rgba;
        if (vegetation) {
            o.vegetation.library = std::make_shared<VegetationLibrary>(
                readVegetationLibrary(OPENWORLD_VEGETATION_LIBRARY_DIR, rgba));
            if (sceneName == "stream") {
                o.vegetation.streaming.enabled = true;
            } else {
                o.vegetation.population =
                    std::make_shared<VegetationPopulation>(makeVegetationPopulation(64, true));
            }
        }
        if (sequence != "stationary" && sequence != "walk" && sequence != "travel" && sequence != "teleport")
            throw std::invalid_argument("Invalid probe sequence");
        Renderer renderer(o);
        SceneSettings s;
        s.terrainVisible = sceneName == "stream";
        s.trees.enabled = enabled;
        s.trees.forceLod = lod;
        s.trees.distance = sceneName == "stream" && quick ? 256 : 4000;
        s.trees.occlusion = true;
        s.trees.wind = .5;
        s.antialiasing = taa ? AntialiasingMode::Taa : AntialiasingMode::Off;
        s.vegetation.shadowDistance = 35;
        if (!shadows)
            s.shadows.mode = ShadowMode::Off;
        s.lighting.sunDirection = glm::normalize(glm::vec3(-.6f, sunY, .35f));
        s.background[0] = .30;
        s.background[1] = .48;
        s.background[2] = .72;
        Camera camera;
        if (sceneName == "specimen") {
            camera.local = {0, library->assets[asset].maximum.y * .45, 40};
            camera.pitch = -.03;
            if (distance > 0)
                camera.local.z = distance;
            double z = camera.local.z;
            camera.local.x = std::sin(yaw) * z;
            camera.local.z = std::cos(yaw) * z;
            camera.yaw = -yaw;
        } else if (sceneName == "panorama") {
            camera.local = {0, 130, quick ? 300 : 1100};
            camera.pitch = -.13;
        } else if (sceneName == "stream") {
            camera.local = {0, 50, 80};
            camera.pitch = -.4;
            TerrainRequest request;
            request.settings.heightScale = 60;
            renderer.requestTerrain(request);
        } else {
            camera.local = {sceneName == "edge" ? 10 : 0, 1.8, 20};
            camera.pitch = .03;
        }
        if (sceneName != "stream") {
            auto mesh = makeQuad();
            for (auto &v : mesh.vertices) {
                v.position = {v.position.x * 5000, -.05f, v.position.y * 5000};
                v.normal = {0, 1, 0};
                v.tangent = {1, 0, 0, 1};
            }
            for (size_t i = 0; i < mesh.indices.size(); i += 3)
                std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
            MeshMaterial material;
            material.baseColour = {.19, .25, .09, 1};
            material.roughness = .95;
            material.doubleSided = true;
            auto mh = renderer.uploadMesh(mesh);
            auto mat = renderer.createMaterial(material);
            MeshInstance ground;
            ground.mesh = mh;
            ground.material = mat;
            renderer.createInstance(ground);
        }
        std::filesystem::create_directories(out);
        std::ofstream csv(out / "frames.csv");
        csv << "frame,cpu_ms,gpu_ms,tree_cpu_ms,update_ms,selection_ms,depth_ms,"
               "colour_ms,shadow_ms,visible,casters,bytes,allocations,pending,"
               "overflow\n";
        std::vector<double> cpu, gpu;
        uint64_t allAllocations = 0;
        const Camera initialCamera = camera;
        for (uint32_t i = 0; i < warmup + frames; ++i) {
            if (sequence == "walk")
                camera.local.z = initialCamera.local.z - i / 60.0 * 1.5;
            if (sequence == "travel")
                camera.local.z = initialCamera.local.z - i / 60.0 * 150;
            if (sequence == "teleport")
                camera.local.x = initialCamera.local.x +
                                 (i >= warmup + frames / 3 && i < warmup + frames * 2 / 3 ? 6000 : 0);
            s.vegetation.time = i / 60.f;
            s.trees.time = i / 60.f;
            auto before = allocations;
            auto start = std::chrono::steady_clock::now();
            renderer.render(camera, s);
            double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            auto calls = allocations - before;
            const auto &t = renderer.treeStats();
            if (i >= warmup) {
                allAllocations += calls;
                cpu.push_back(ms);
                gpu.push_back(renderer.renderingStats().frameMilliseconds);
                csv << i << ',' << ms << ',' << gpu.back() << ',' << t.cpuMilliseconds << ','
                    << t.updateMilliseconds << ',' << t.selectionMilliseconds << ',' << t.depthMilliseconds
                    << ',' << t.colourMilliseconds << ',' << t.shadowMilliseconds << ',' << t.visible << ','
                    << t.casters << ',' << t.bytes << ',' << calls << ',' << t.pending << ',' << t.overflow
                    << '\n';
                csv.flush();
            }
            if (t.overflow)
                throw std::runtime_error("Tree list overflow invalidates run");
        }
        renderer.capture(out / "view.ppm");
        std::sort(cpu.begin(), cpu.end());
        std::sort(gpu.begin(), gpu.end());
        auto percentile = [](const auto &v, double q) {
            return v.empty() ? 0 : v[size_t((v.size() - 1) * q)];
        };
        const auto &t = renderer.treeStats();
        std::ofstream summary(out / "summary.txt");
        summary << "device " << renderer.deviceName() << "\nresolution " << width << 'x' << height
                << "\nvalidation " << validation << "\nframes " << frames << " warmup " << warmup
                << "\nsequence " << sequence << "\nasset " << asset << " lod " << lod << "\nvegetation "
                << vegetation << " rgba " << rgba << " shadows " << shadows << " trees " << enabled << '\n';
        summary << "scene " << sceneName << "\nCPU median/p95 " << percentile(cpu, .5) << " / "
                << percentile(cpu, .95) << " ms\nGPU median/p95 " << percentile(gpu, .5) << " / "
                << percentile(gpu, .95) << " ms\nTree memory " << t.bytes
                << " bytes\nWarm render allocations " << allAllocations << "\nVisible entries " << t.visible
                << " casters " << t.casters << "\nPending " << t.pending << " deferred " << t.deferred
                << "\n";
        std::cout << "Review saved to " << out << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

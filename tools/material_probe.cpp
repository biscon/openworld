#include "mesh_primitives.hpp"
#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <vector>
// Explicit production-art review/performance tool, never part of routine tests.
int main(int argc, char **argv) {
    try {
        if (argc < 3)
            throw std::runtime_error(
                "Usage: material_probe palette-directory capture-directory [width "
                "height] [--quick] [--fixed] [--rgba] [--motion] [--fixture-only] [--mode NAME]");
        const std::filesystem::path out = argv[2];
        std::filesystem::create_directories(out);
        ow::RendererOptions o;
        o.ui = false;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.materialDirectory = argv[1];
        o.requireValidation = true;
        int argument = 3;
        if (argc > 4 && argv[3][0] != '-') {
            o.width = std::stoul(argv[3]);
            o.height = std::stoul(argv[4]);
            argument = 5;
        } else {
            o.width = 1920;
            o.height = 1080;
        }
        bool quick = false, motion = false, fixtureOnly = false;
        std::string selectedMode;
        for (; argument < argc; ++argument) {
            const std::string flag = argv[argument];
            if (flag == "--quick")
                quick = true;
            else if (flag == "--fixed")
                o.forceFixedMaterials = true;
            else if (flag == "--rgba")
                o.forceRgbaMaterials = true;
            else if (flag == "--fixture-only")
                fixtureOnly = true;
            else if (flag == "--mode" && argument + 1 < argc)
                selectedMode = argv[++argument];
            else if (flag == "--motion")
                motion = true;
            else
                throw std::runtime_error("Unknown option: " + flag);
        }
        o.terrainGrid = {256, .5f};
        o.terrainCapacity = 256;
        // This probe owns its shader override; no fixture mode enters the application renderer.
        const auto fixtureShaders = out / "fixture-shaders";
        std::filesystem::create_directories(fixtureShaders);
        for (const auto &entry : std::filesystem::directory_iterator(o.shaderDirectory)) {
            if (entry.path().extension() != ".spv")
                continue;
            auto source = entry.path();
            if (source.filename() == "mesh.frag.spv")
                source = source.parent_path() / "mesh_terrain_fixture.frag.spv";
            if (source.filename() == "mesh_bindless.frag.spv")
                source = source.parent_path() / "mesh_terrain_fixture_bindless.frag.spv";
            std::filesystem::copy_file(source, fixtureShaders / entry.path().filename(),
                                       std::filesystem::copy_options::overwrite_existing);
        }
        o.shaderDirectory = fixtureShaders;
        ow::Renderer r(o);
        auto quad = r.uploadMesh(ow::makeQuad());
        std::vector<ow::InstanceHandle> planes;
        std::vector<ow::MaterialHandle> planeMaterials;
        const auto clearPlanes = [&] {
            for (auto h : planes)
                r.removeInstance(h);
            planes.clear();
            for (auto h : planeMaterials)
                r.releaseMaterial(h);
            planeMaterials.clear();
        };
        const auto addPlane = [&](glm::vec3 position, glm::vec3 normal, float scale, glm::vec4 weights) {
            ow::MeshMaterial m;
            m.baseColour = weights;
            m.doubleSided = true;
            auto mh = r.createMaterial(m);
            planeMaterials.push_back(mh);
            ow::MeshInstance i;
            i.mesh = quad;
            i.material = mh;
            i.position.local = position;
            i.scale = glm::vec3(scale);
            glm::vec3 z(0, 0, 1), axis = glm::cross(z, normal);
            float cosine = glm::dot(z, normal);
            i.rotation = glm::normalize(glm::quat(1 + cosine, axis.x, axis.y, axis.z));
            planes.push_back(r.createInstance(i));
        };
        ow::SceneSettings scene;
    scene.shadows.mode = ow::ShadowMode::Off;
        scene.gtao.mode = ow::GtaoMode::Off;
        ow::Camera camera;
        auto draw = [&](int n) {
            for (int i = 0; i < n; ++i)
                r.render(camera, scene);
        };
        auto capture = [&](const std::string &name) {
            draw(3);
            r.capture(out / (name + ".ppm"));
        };
        std::ofstream metadata(out / "review.txt");
        metadata << "device=" << r.deviceName() << "\nresolution=" << o.width << 'x' << o.height
                 << "\nquick=" << quick << "\nmacro=" << r.materialStats().macroLoaded
                 << "\nbinding=" << (r.materialStats().bindless ? "bindless" : "fixed")
                 << "\nimageBytes=" << r.materialStats().imageBytes << '\n';
        metadata.flush();
        if (!fixtureOnly) {
            camera.local = {0, -2, 18};
            scene.terrainVisible = false;
            for (int layer = 0; layer < 4; ++layer) {
                glm::vec4 weights(0);
                weights[layer] = 1;
                addPlane({(layer - 1.5f) * 3.2f, -5.8f, 0}, {0, 1, 0}, 1.4f, weights);
                addPlane({(layer - 1.5f) * 3.2f, -9, 0}, glm::normalize(glm::vec3(0, 1, 1)), 1.4f, weights);
            }
            capture("palette");
            scene.lighting.sunDirection = {.6, .4, -.5};
            capture("palette_rotated_light");
            scene.lighting.sunDirection = {-.6, .8, .35};
            clearPlanes();
            scene.terrainVisible = true;
            ow::TerrainRequest request;
            request.settings.heightScale = 0;
            r.requestTerrain(request);
            camera.local = {64, 3, 118};
            camera.pitch = -.1f;
            for (int i = 0; i < 4; ++i) {
                scene.terrainMaterial.isolate = i;
                capture("surface_" + std::to_string(i));
            }
            scene.terrainMaterial.isolate = -1;
            // Locate a representative sloped region for each seed using an owned
            // small geography query, then render the real micro-detail terrain.
            for (uint32_t seed : {1u, 2u, 3u}) {
                request = {};
                request.settings.seed = seed;
                const auto geography = r.sampleTerrainGeography({32, 500}, request);
                size_t best = 0;
                float slope = -1;
                for (size_t i = 0; i < geography.size(); ++i) {
                    const float candidate = glm::length(glm::vec2(geography[i].base));
                    if (candidate > slope) {
                        slope = candidate;
                        best = i;
                    }
                }
                request.chunk = {int64_t(best % 33) * 500 / 128, int64_t(best / 33) * 500 / 128};
                const auto local = r.sampleTerrainGeography({8, 16}, request);
                const float height = local[4 * 9 + 4].detail.w;
                r.requestTerrain(request);
                camera.chunk = request.chunk;
                camera.local = {64, height + 90, 150};
                camera.pitch = -.65f;
                const auto prefix = "seed_" + std::to_string(seed);
                metadata << prefix << " chunk=" << request.chunk.x << ',' << request.chunk.z
                         << " centreHeight=" << height << '\n';
                ow::applyMaterialPreset(scene.terrainMaterial, ow::TerrainMaterialPreset::Baseline);
                capture(prefix + "_baseline");
                ow::applyMaterialPreset(scene.terrainMaterial, ow::TerrainMaterialPreset::Balanced);
                capture(prefix + "_balanced");
                scene.terrainMaterial.heightBlend = false;
                capture(prefix + "_no_height");
                scene.terrainMaterial.heightBlend = true;
                scene.terrainMaterial.cliffs = false;
                capture(prefix + "_no_cliffs");
                scene.terrainMaterial.cliffs = true;
                scene.terrainMaterial.macro = false;
                capture(prefix + "_no_macro");
                scene.terrainMaterial.macro = true;
                scene.terrainMaterial.twoAxis = true;
                capture(prefix + "_pruned");
                scene.terrainMaterial.twoAxis = false;
                ow::applyMaterialPreset(scene.terrainMaterial, ow::TerrainMaterialPreset::High);
                capture(prefix + "_high");
                scene.terrainMaterial.diagnostic = 2;
                capture(prefix + "_projections");
                scene.terrainMaterial.diagnostic = 4;
                capture(prefix + "_reads");
                scene.terrainMaterial.diagnostic = 0;
                camera.local = {64, height + 5, 64};
                camera.pitch = -.15f;
                capture(prefix + "_ground");
                camera.local = {64, height + 90, 150};
                camera.pitch = -.65f;
                if (motion) {
                    for (int frame = 0; frame < 32; ++frame) {
                        camera.local.x = 48 + frame;
                        draw(1);
                        r.capture(out / (prefix + "_motion_" + std::to_string(frame) + ".ppm"));
                    }
                    camera.local.x = 64;
                }
            }
        }
        std::ofstream stats(out / "timings.csv");
        stats << "workload,mode,scene_median_ms,scene_p95_ms,output_median_ms,output_p95_ms,"
                 "max_logical_reads,effective_af,samples\n";
        const char *names[] = {"constant",  "baseline",           "height",      "cliffs",
                               "macro",     "balanced_trilinear", "balanced_8x", "balanced_16x",
                               "pruned_8x", "depth_prepass"};
        if (!selectedMode.empty() &&
            std::find(std::begin(names), std::end(names), selectedMode) == std::end(names))
            throw std::runtime_error("Unknown timing mode: " + selectedMode);
        auto percentile = [](const std::vector<double> &v, double p) {
            const double index = (v.size() - 1) * p;
            const size_t lo = size_t(index), hi = std::min(lo + 1, v.size() - 1);
            return v[lo] + (v[hi] - v[lo]) * (index - lo);
        };
        for (int workload = fixtureOnly ? 1 : 0; workload < 2; ++workload) {
            if (workload) {
                clearPlanes();
                scene.terrainVisible = false;
                addPlane({0, 0, 0}, glm::normalize(glm::vec3(.6f, .5f, .6f)), 8, glm::vec4(.25f));
                camera = {};
                const auto n = glm::normalize(glm::vec3(.6f, .5f, .6f));
                camera.local = glm::dvec3(n) * 5.;
                camera.yaw = std::atan2(-n.x, n.z);
                camera.pitch = std::asin(-n.y);
            }
            for (int mode = 0; mode < 10; ++mode) {
                if (workload && mode == 9)
                    continue;
                if (!selectedMode.empty() && selectedMode != names[mode])
                    continue;
                auto &m = scene.terrainMaterial;
                ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::Baseline);
                m.enabled = mode != 0;
                if (mode == 2)
                    m.heightBlend = true;
                if (mode == 3)
                    m.cliffs = true;
                if (mode == 4)
                    m.macro = true;
                if (mode >= 5)
                    ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::Balanced);
                if (mode == 5)
                    m.filtering = 0;
                if (mode == 7)
                    m.filtering = 2;
                if (mode == 8)
                    m.twoAxis = true;
                scene.opaqueDepthPrepass = mode == 9;
                draw(quick ? 4 : 16);
                std::vector<double> a, b;
                const int count = quick ? 8 : 120;
                a.reserve(count);
                b.reserve(count);
                for (int frame = 0; frame < count; ++frame) {
                    draw(1);
                    if (r.renderingStats().timingsAvailable) {
                        a.push_back(r.renderingStats().sceneMilliseconds);
                        b.push_back(r.renderingStats().outputMilliseconds);
                    }
                }
                std::sort(a.begin(), a.end());
                std::sort(b.begin(), b.end());
                stats << (workload ? "four_layer_cliff" : "terrain") << ',' << names[mode] << ',';
                if (a.empty())
                    stats << "unavailable,unavailable,unavailable,unavailable,";
                else
                    stats << percentile(a, .5) << ',' << percentile(a, .95) << ',' << percentile(b, .5) << ','
                          << percentile(b, .95) << ',';
                stats << r.materialStats().maximumTextureReads << ',' << r.materialStats().effectiveAnisotropy
                      << ',' << a.size() << '\n';
                stats.flush();
                std::cout << "Measured " << names[mode] << std::endl;
                if (workload)
                    r.capture(out / (std::string("fixture_") + names[mode] + ".ppm"));
            }
        }
        if (r.validationErrors())
            throw std::runtime_error("Material probe validation errors");
        std::cout << "Material review completed: " << out << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

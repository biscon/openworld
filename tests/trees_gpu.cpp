#include "renderer.hpp"
#include "test_support.hpp"
#include "tree_baker.hpp"
#include "tree_generation.hpp"
#include <iostream>
#include <unistd.h>
using namespace ow;
int main() {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("openworld-tree-gpu-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(dir);
        TreeRecipe recipe;
        recipe.height = 7;
        recipe.crownRadius = 2;
        recipe.attractors = 80;
        auto asset = generateTree(recipe);
        asset.frames = 4;
        asset.tileSize = 32;
        auto library = std::make_shared<TreeLibrary>();
        library->assets.push_back(asset);
        TextureData colour;
        colour.width = colour.height = 8;
        colour.format = TextureFormat::Srgb8;
        colour.bytes.resize(8 * 8 * 4);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                auto at = (y * 8 + x) * 4;
                colour.bytes[at] = 85;
                colour.bytes[at + 1] = 145;
                colour.bytes[at + 2] = 55;
                colour.bytes[at + 3] = (x == 0 || y == 0 || x == 7 || y == 7) ? 0 : 255;
            }
        TreeBake baked;
        {
            TreeBaker baker(OPENWORLD_SHADER_DIR, true);
            baked = baker.bake(asset, colour, colour);
        }
        uint32_t opaque = 0;
        for (size_t i = 3; i < baked.colour.bytes.size(); i += 4)
            opaque += baked.colour.bytes[i] > 128;
        require(opaque > 100 && opaque < baked.colour.width * baked.colour.height / 2,
                "baker captures sparse silhouette rather than opaque quad");
        for (uint32_t s = 0; s < 4; ++s) {
            auto name = std::string(treeSpeciesName(TreeSpecies(s)));
            writeTexture(dir / (name + "_bark.dds"), colour, true);
            writeTexture(dir / (name + "_leaf.dds"), colour, true);
        }
        writeTexture(dir / "tree0_colour.dds", baked.colour, true);
        writeTexture(dir / "tree0_normal.dds", baked.normal, true);
        writeTexture(dir / "tree0_depth.dds", baked.depth, true);
        RendererOptions o;
        o.ui = false;
        o.forceFixedMaterials = true;
        o.forceRgbaMaterials = true;
        o.requireValidation = true;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.width = 160;
        o.height = 120;
        o.terrainGrid = {8, .5f};
        o.terrainCapacity = 8;
        o.shadowCapacity.resolution = 128;
        o.trees.library = library;
        o.trees.directory = dir;
        o.trees.streaming.cellCapacity = 4;
        o.trees.streaming.maximumDistance = 64;
        o.trees.streaming.budgetBytes = 32ull << 20;
        o.trees.fixture.resize(1);
        auto &t = o.trees.fixture[0].instances[0];
        t.rootScale = {0, 0, 0, 1};
        t.identity = 17;
        t.variation = .5;
        Renderer renderer(o);
        Camera camera;
        camera.local = {0, 3.5, 14};
        SceneSettings scene;
        scene.terrainVisible = false;
        scene.trees.distance = 64;
        scene.trees.occlusion = false;
        scene.trees.wind = 0;
        scene.shadows.mode = ShadowMode::Off;
        scene.trees.forceLod = 0;
        scene.antialiasing = AntialiasingMode::Off;
        scene.background[0] = scene.background[1] = scene.background[2] = 0;
        for (int i = 0; i < 5; ++i)
            require(renderer.render(camera, scene), "tree render submission");
        auto stats = renderer.treeStats();
        require(stats.completed && stats.visible == 1 && !stats.overflow, "mesh tree indirect selection");
        auto snapshot = renderer.readHdrSnapshot();
        uint32_t pixels = 0;
        for (auto p : snapshot.pixels)
            pixels += glm::length(glm::vec3(p)) > .001f;
        require(pixels > 100 && pixels < snapshot.pixels.size() / 2, "mesh tree has a lit sparse silhouette");
        scene.trees.forceLod = 3;
        scene.shadows.mode = ShadowMode::CSM;
        for (int i = 0; i < 5; ++i) {
            scene.trees.time += 1 / 60.f;
            require(renderer.render(camera, scene), "impostor shadow render");
        }
        stats = renderer.treeStats();
        require(stats.lodCounts[3] == 1 && !stats.overflow, "impostor indirect selection");
        require(stats.casters > 0, "independent impostor shadow casters");
        auto shadow = renderer.readShadowSnapshot();
        uint32_t shadowed = 0;
        for (auto p : shadow.depth)
            shadowed += p > 0;
        require(shadowed > 0, "impostor writes shadow depth");
        auto impostor = renderer.readHdrSnapshot();
        uint32_t covered = 0;
        for (auto p : impostor.pixels) {
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                    "finite reconstructed shading");
            covered += glm::length(glm::vec3(p)) > .001f;
        }
        require(covered > pixels / 3 && covered < pixels * 3, "impostor retains mesh silhouette coverage");
        scene.trees.occlusion = true;
        scene.antialiasing = AntialiasingMode::Taa;
        scene.trees.wind = 1;
        for (float angle : {0.f, 1.5707f, 3.14159f, 4.7123f}) {
            camera.local = {std::sin(angle) * 14, 3.5, std::cos(angle) * 14};
            camera.yaw = -angle;
            for (int i = 0; i < 3; ++i) {
                scene.trees.time += 1 / 60.f;
                renderer.render(camera, scene);
            }
            require(renderer.treeStats().visible == 1, "octahedral axis crossing retains tree");
        }
        for (float sign : {-1.f, 1.f}) {
            camera.local = {0, 3.5 + 14 * sign, .02};
            camera.yaw = 0;
            camera.pitch = -sign * 1.569f;
            for (int i = 0; i < 4; ++i)
                renderer.render(camera, scene);
            require(renderer.treeStats().visible == 1, "full sphere impostor pole retains tree");
            auto pole = renderer.readHdrSnapshot();
            uint32_t valid = 0;
            for (auto p : pole.pixels) {
                require(std::isfinite(p.x), "finite pole reconstruction");
                valid += glm::length(glm::vec3(p)) > .001f;
            }
            require(valid > 5, "pole view has reconstructed pixels");
        }
        camera.pitch = 0;
        camera.local = {0, 3.5, 14};
        camera.yaw = 0;
        scene.trees.forceLod = -1;
        for (int i = 0; i < 4; ++i)
            renderer.render(camera, scene);
        auto before = renderer.readTreeSnapshot();
        require(before.cells.size() == 1, "bounded diagnostic snapshot");
        camera.chunk.x -= 1;
        camera.local.x += o.terrainGrid.extent();
        for (int i = 0; i < 4; ++i)
            renderer.render(camera, scene);
        auto after = renderer.readTreeSnapshot();
        require(after.cells[0].population[0].identity == before.cells[0].population[0].identity,
                "rebase preserves tree identity");
        require(after.cells[0].attachments[0] == before.cells[0].attachments[0],
                "rebase preserves root attachment");
        require(renderer.validationErrors() == 0, "tree Vulkan validation clean");
        scene.trees.enabled = false;
        for (int i = 0; i < 4; ++i)
            renderer.render(camera, scene);
        require(renderer.treeStats().visible == 0, "disabled trees clear indirect counts");
        std::filesystem::remove_all(dir);
        std::cout << "Tree Vulkan checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::filesystem::remove_all(dir);
        std::cerr << e.what() << '\n';
        return 1;
    }
}

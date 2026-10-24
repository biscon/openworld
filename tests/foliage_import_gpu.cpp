#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
int main(int argc, char **argv) {
    try {
        require(argc == 2, "owned imported library required");
        std::array<HdrSnapshot, 4> images;
        for (uint32_t mode = 0; mode < 4; ++mode) {
            auto library = std::make_shared<VegetationLibrary>(readVegetationLibrary(argv[1], mode % 2));
            auto population = std::make_shared<VegetationPopulation>();
            population->cells.push_back({{}, 0, 1});
            population->instances.push_back({{0, 0, 0, 1}, 0, .5f, 173, 2});
            RendererOptions o;
            o.ui = false;
            o.width = 128;
            o.height = 96;
            o.terrainGrid = {8, .5f};
            o.terrainCapacity = 8;
            o.shaderDirectory = OPENWORLD_SHADER_DIR;
            o.requireValidation = true;
            o.forceFixedMaterials = mode < 2;
            o.vegetation.library = library;
            o.vegetation.population = population;
            Renderer r(o);
            SceneSettings s;
            s.terrainVisible = false;
            s.antialiasing = ow::AntialiasingMode::Off;
            s.shadows.mode = ShadowMode::Off;
            s.vegetation.wind = 0;
            s.vegetation.coverage = true;
            Camera c;
            c.local = {0, .6, 1.2};
            c.pitch = -.15f;
            r.render(c, s);
            images[mode] = r.readHdrSnapshot();
            uint32_t covered = 0;
            for (auto p : images[mode].pixels)
                covered += p.x > .5f && p.y > .5f && p.z > .5f;
            require(covered > 30, "masked imported leaf rendered");
            require(!r.validationErrors(), "imported foliage validation");
        }
        for (uint32_t mode = 1; mode < 4; ++mode) {
            uint32_t different = 0;
            for (size_t i = 0; i < images[0].pixels.size(); ++i)
                different += glm::length(images[0].pixels[i] - images[mode].pixels[i]) > .02f;
            require(different < images[0].pixels.size() / 50,
                    "compressed/RGBA and binding mask coverage agree within two percent of frame");
        }
        std::cout << "foliage import GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

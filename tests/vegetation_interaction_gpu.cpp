#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
float energy(const std::vector<glm::vec2> &f) {
    float sum = 0;
    for (auto p : f) {
        require(std::isfinite(p.x) && std::isfinite(p.y) && glm::length(p) <= 1.00001f,
                "bounded finite field");
        sum += glm::length(p);
    }
    return sum;
}
int main() {
    try {
        for (bool fixed : {true, false}) {
            RendererOptions o;
            o.ui = false;
            o.width = 192;
            o.height = 128;
            o.terrainGrid = {8, .5f};
            o.terrainCapacity = 8;
            o.shaderDirectory = OPENWORLD_SHADER_DIR;
            o.requireValidation = true;
            o.forceFixedMaterials = fixed;
            o.vegetation.library = std::make_shared<VegetationLibrary>(buildVegetationLibrary());
            o.vegetation.population =
                std::make_shared<VegetationPopulation>(makeVegetationPopulation(4, true));
            Renderer r(o);
            SceneSettings s;
            s.terrainVisible = false;
            s.shadows.mode = ShadowMode::Off;
            s.vegetation.wind = 0;
            Camera c;
            c.local = {0, 1, 4};
            c.pitch = -.1f;
            auto &v = s.vegetation;
            v.contacts.active = true;
            v.contacts.append({});
            auto frame = [&](float dt) {
                v.time += dt;
                require(r.render(c, s), "submitted frame");
            };
            for (int i = 0; i < 12; ++i)
                frame(.05f);
            auto contact = r.readVegetationSnapshot();
            float e = energy(contact.interaction);
            require(e > 3, "contact bends field");
            auto before = r.readHdrSnapshot();
            s.opaqueDepthPrepass = true;
            frame(0);
            auto depth = r.readHdrSnapshot();
            double passError = 0;
            for (size_t i = 0; i < before.pixels.size(); ++i)
                passError += glm::length(glm::vec3(before.pixels[i] - depth.pixels[i]));
            require(passError / before.pixels.size() < 1e-5, "interaction depth/colour agreement");
            s.antialiasing = AntialiasingMode::Taa;
            v.contacts.count = 0;
            frame(.01f);
            frame(.01f);
            uint32_t moving = 0;
            for (auto motion : r.readPostSnapshot().motion) {
                require(std::isfinite(motion.x) && std::isfinite(motion.y), "finite interaction motion");
                moving += motion.w > 0 && glm::length(glm::vec2(motion)) > 1e-6f;
            }
            require(moving > 0, "recovery uses previous submitted deformation");
            s.antialiasing = AntialiasingMode::Off;
            e = energy(r.readVegetationSnapshot().interaction);
            v.contacts.count = 0;
            for (int i = 0; i < 30; ++i)
                frame(.05f);
            auto recover = r.readVegetationSnapshot();
            near(energy(recover.interaction) / e, .5, 2e-4, "one half-life recovery");
            // Camera-only changes must not alter world field, including a large rebase.
            c.chunk.x = 100000;
            c.local.x = 13;
            frame(0);
            auto moved = r.readVegetationSnapshot();
            for (size_t i = 0; i < moved.interaction.size(); ++i)
                near(glm::length(moved.interaction[i] - recover.interaction[i]), 0, 1e-6,
                     "camera-independent field");
            c.chunk.x = 0;
            c.local.x = 0;
            // One-texel shifts retain exact overlapping values, zero newly exposed strips.
            for (glm::dvec3 shift : {glm::dvec3(.25, 0, 0), glm::dvec3(0, 0, -.25), glm::dvec3(-.25, 0, 0),
                                     glm::dvec3(0, 0, .25)}) {
                auto old = r.readVegetationSnapshot().interaction;
                v.contacts.anchor = translated(v.contacts.anchor, shift);
                frame(0);
                auto next = r.readVegetationSnapshot().interaction;
                int dx = int(shift.x * 4), dz = int(shift.z * 4);
                for (int z = 0; z < 128; ++z)
                    for (int x = 0; x < 128; ++x) {
                        int xx = x + dx, zz = z + dz;
                        auto expected =
                            xx >= 0 && xx < 128 && zz >= 0 && zz < 128 ? old[zz * 128 + xx] : glm::vec2(0);
                        near(glm::length(next[z * 128 + x] - expected), 0, 1e-6,
                             "scroll overlap/exposed strips");
                    }
            }
            v.contacts.anchor = translated(v.contacts.anchor, {64, 0, 0});
            frame(.05f);
            require(energy(r.readVegetationSnapshot().interaction) == 0, "teleport clears field");
            v.contacts.anchor = {};
            frame(.05f);
            require(energy(r.readVegetationSnapshot().interaction) == 0, "return cannot resurrect trail");
            v.contacts.append({});
            for (int i = 0; i < 6; ++i)
                frame(.05f);
            v.interaction.enabled = false;
            frame(.05f);
            require(energy(r.readVegetationSnapshot().interaction) == 0, "disable clears field");
            v.interaction.enabled = true;
            v.contacts.count = 0;
            frame(.05f);
            require(energy(r.readVegetationSnapshot().interaction) == 0, "reenable clean");
            // Repeated frames without readback exercise actual ring reuse with two frames in flight.
            v.contacts.append({});
            for (int i = 0; i < 40; ++i)
                frame(.01f);
            require(energy(r.readVegetationSnapshot().interaction) > 3, "ring reuse keeps contact");
            ++v.contacts.reset;
            v.contacts.count = 0;
            frame(.01f);
            require(energy(r.readVegetationSnapshot().interaction) == 0, "explicit reset");
            require(r.validationErrors() == 0, "interaction Vulkan synchronization validation");
        }
        std::cout << "Vegetation interaction GPU checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

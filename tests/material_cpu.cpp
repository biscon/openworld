#include "material_assets.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace ow;
int main() {
    try {
        auto path = std::filesystem::temp_directory_path() /
                    ("ow-material-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dds");
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove(p, e);
            }
        } cleanup{path};
        TextureData t;
        t.width = t.height = 8;
        t.levels = 4;
        t.layers = 4;
        t.format = TextureFormat::Srgb8;
        t.bytes.resize(textureBytes(t));
        for (size_t i = 0; i < t.bytes.size(); ++i)
            t.bytes[i] = uint8_t(i * 13);
        writeTexture(path, t);
        auto loaded = readTexture(path);
        require(loaded.bytes == t.bytes && loaded.layers == 4, "DDS roundtrip");
        auto reject = [&]() {
            try {
                readTexture(path);
            } catch (const std::exception &) {
                return;
            }
            throw std::runtime_error("Malformed DDS accepted");
        };
        std::filesystem::resize_file(path, 150);
        reject();
        writeTexture(path, t);
        {
            std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
            uint32_t bad = 0xffffffff;
            f.seekp(16);
            f.write(reinterpret_cast<char *>(&bad), 4);
        }
        reject();
        writeTexture(path, t);
        {
            std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
            uint32_t bad = 1;
            f.seekp(28);
            f.write(reinterpret_cast<char *>(&bad), 4);
        }
        reject();
        MaterialSlots<TextureHandle, 3> slots;
        auto a = slots.acquire(), b = slots.acquire();
        require(slots.resolve(a) && slots.resolve(b), "Live handles");
        require(slots.resolve(slots.acquire()) == 0, "Bounded slots");
        slots.release(a);
        auto c = slots.acquire();
        require(slots.resolve(a) == 0 && slots.resolve(c) != 0, "Recycled handle generation");
        for (auto x : {int64_t(-1000000000), int64_t(-1), int64_t(0), int64_t(1000000000)})
            for (double repeat : {.25, 3.7, 4., 128.}) {
                auto a0 = materialPhase(x, -x, 128, {129.125, -.125}, repeat),
                     a1 = materialPhase(x + 1, -x - 1, 128, {1.125, 127.875}, repeat);
                require(glm::length(a0 - a1) < 2e-6, "Phase survives rebase at negative/distant origins");
                require(a0.x >= 0 && a0.x < 1 && a0.y >= 0 && a0.y < 1, "Euclidean repeat phase");
            }
        for (int64_t x : {-1000000000ll, -1ll, 0ll, 1000000000ll}) {
            const auto a = materialCoordinates(x, -x, 128, {129.125, -17.25, -.125}, 3.7, 64);
            const auto b = materialCoordinates(x + 1, -x - 1, 128, {1.125, -17.25, 127.875}, 3.7, 64);
            require(glm::length(a.microPhase - b.microPhase) < 2e-6f &&
                        glm::length(a.macroPhase - b.macroPhase) < 2e-6f,
                    "All projection phases survive rebasing");
        }
        std::cout << "Material CPU invariants passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

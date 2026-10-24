#include "test_support.hpp"
#include "vegetation.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
using namespace ow;
int main() {
    try {
        auto assets = makeVegetationAssets();
        auto a = makeVegetationFixture(assets, 8, true), b = makeVegetationFixture(assets, 8, true);
        require(a.instances.size() == 512, "bounded population");
        for (size_t i = 0; i < a.instances.size(); ++i) {
            auto &p = a.instances[i];
            near(glm::length(p.rootScale - b.instances[i].rootScale), 0, 0, "deterministic population");
            float x = p.rootScale.x + 4, z = p.rootScale.z + 4;
            uint32_t cellX = uint32_t(x), cellZ = uint32_t(z);
            size_t triangle = (cellZ * 8 + cellX) * 6 + (x - cellX > z - cellZ ? 0 : 3);
            auto p0 = a.ground.vertices[a.ground.indices[triangle]].position;
            auto p1 = a.ground.vertices[a.ground.indices[triangle + 1]].position;
            auto p2 = a.ground.vertices[a.ground.indices[triangle + 2]].position;
            auto normal = glm::cross(p1 - p0, p2 - p0);
            near(glm::dot(glm::vec3(p.rootScale) - p0, normal), 0, 1e-5,
                 "root lies in rendered triangle plane");
            require(p.identity == i && p.cell == i / 128, "stable cell identities");
        }
        for (const auto &m : assets.meshes) {
            require(!m.indices.empty() && m.indices.size() % 3 == 0, "triangles");
            for (auto i : m.indices)
                require(i < m.vertices.size(), "valid index");
            for (auto &v : m.vertices)
                require(v.position.y >= 0 && v.position.y <= 1.001, "anchored blade envelope");
        }
        const auto &t = assets.cards;
        require(t.bytes.size() == textureBytes(t), "complete mip chain");
        size_t offset = 0;
        double base = 0;
        for (uint32_t n = t.width; n; n >>= 1) {
            uint32_t covered = 0;
            for (uint32_t i = 0; i < n * n; ++i)
                covered += t.bytes[offset + 4 * i + 3] >= 128;
            double fraction = double(covered) / (n * n);
            if (n == t.width)
                base = fraction;
            if (n >= 8)
                near(fraction, base, .06, "coverage-preserving mips");
            offset += n * n * 4;
        }
        require(base > .03 && base < .6, "nontrivial card silhouette");
        bool failed = false;
        try {
            makeVegetationFixture({}, 260);
        } catch (const std::invalid_argument &) {
            failed = true;
        }
        require(failed, "oversized meadow rejected");
        failed = false;
        auto settings = VegetationSettings{};
        settings.wind = 3;
        try {
            validate(settings);
        } catch (const std::invalid_argument &) {
            failed = true;
        }
        require(failed, "wind bound enforced");
        auto path = std::filesystem::temp_directory_path() /
                    ("ow-vegetation-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        } cleanup{path};
        writeVegetationAssets(path, assets);
        auto loaded = readVegetationAssets(path);
        require(loaded.cards.bytes == assets.cards.bytes, "saved texture round trip");
        for (uint32_t i = 0; i < vegetationBins; ++i) {
            require(loaded.meshes[i].indices == assets.meshes[i].indices, "saved mesh topology");
            require(loaded.meshes[i].vertices.size() == assets.meshes[i].vertices.size(),
                    "saved vertex count");
        }
        std::ofstream(path / "tufts.bin", std::ios::binary | std::ios::trunc) << "bad";
        failed = false;
        try {
            readVegetationAssets(path);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        require(failed, "truncated fixture rejected");
        auto maximum = makeVegetationFixture({}, 256);
        require(maximum.instances.size() == vegetationMaximum, "maximum fixed capacity");
        std::cout << "vegetation CPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

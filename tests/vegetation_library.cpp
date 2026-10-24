#include "vegetation_library.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
using namespace ow;
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "invalid asset/population rejected");
}
int main() {
    try {
        auto library = buildVegetationLibrary();
        require(library.assets.size() == 4, "representative asset set");
        auto second = buildVegetationLibrary();
        for (uint32_t a = 0; a < 4; ++a)
            for (uint32_t lod = 0; lod < 3; ++lod) {
                const auto &part = library.assets[a].lods[lod][0];
                require(part.indices == second.assets[a].lods[lod][0].indices, "deterministic topology");
                if (lod)
                    require(part.indices.size() < library.assets[a].lods[lod - 1][0].indices.size(),
                            "LOD reduces triangles");
                for (auto &v : part.vertices)
                    if (v.position.y == 0)
                        require(v.deformation.y == 0 && v.deformation.z == 0, "roots anchored");
            }
        // First blade root and tip persist through changing segment counts.
        for (uint32_t a = 0; a < 2; ++a)
            for (uint32_t lod = 1; lod < 3; ++lod) {
                const auto &full = library.assets[a].lods[0][0].vertices;
                const auto &low = library.assets[a].lods[lod][0].vertices;
                near(glm::length(full[0].position - low[0].position), 0, .004, "nested blade root");
                near(glm::length(full[6].position - low[(3 - lod) * 2].position), 0, .001,
                     "nested blade tip");
            }
        auto population = makeVegetationPopulation(8, true, true);
        validate(population, library);
        for (auto &cell : population.cells)
            cell.origin = translated(cell.origin, {1e12, 0, -1e12});
        validate(population, library);
        auto bad = population;
        bad.instances[0].assetCell = 7;
        rejects([&] { validate(bad, library); });
        bad = population;
        bad.cells[1].first = 0;
        rejects([&] { validate(bad, library); });
        bad = population;
        bad.instances[0].rootScale.x = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { validate(bad, library); });
        bad = population;
        bad.instances[0].identity = bad.instances[1].identity;
        rejects([&] { validate(bad, library); });
        require(packVegetationAsset({7}, 4095) == 32767, "bounded asset/cell packing");
        rejects([] { packVegetationAsset({8}, 0); });
        auto grassCaster = library;
        grassCaster.assets[0].ranges.w = 25;
        rejects([&] { validate(grassCaster); });
        auto malformed = library;
        malformed.assets[0].lods[0][0].indices[0] = 999999;
        rejects([&] { validate(malformed); });
        malformed = library;
        malformed.assets[0].maximum.y = 0;
        rejects([&] { validate(malformed); });
        malformed = library;
        malformed.materials[0].normalTexture = 99;
        rejects([&] { validate(malformed); });
        malformed = library;
        malformed.assets[0].lods[0][0].vertices[0].normal = {};
        rejects([&] { validate(malformed); });
        auto dir =
            std::filesystem::temp_directory_path() /
            ("ow-foliage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code e;
                std::filesystem::remove_all(path, e);
            }
        } cleanup{dir};
        writeVegetationLibrary(dir, library);
        auto loaded = readVegetationLibrary(dir);
        require(loaded.textures[0].bytes == library.textures[0].bytes, "saved texture round trip");
        auto &a = loaded.assets[3].lods[1][1];
        auto &b = library.assets[3].lods[1][1];
        require(a.indices == b.indices && a.vertices.size() == b.vertices.size(), "saved multipart LOD");
        for (size_t i = 0; i < a.vertices.size(); ++i)
            near(glm::length(a.vertices[i].deformation - b.vertices[i].deformation), 0, 0,
                 "saved deformation metadata");
        {
            std::ofstream f(dir / "library.bin", std::ios::binary | std::ios::app);
            f.put('x');
        }
        rejects([&] { readVegetationLibrary(dir); });
        {
            std::ofstream f(dir / "library.bin", std::ios::binary | std::ios::trunc);
            f << "bad";
        }
        rejects([&] { readVegetationLibrary(dir); });
        std::cout << "vegetation library checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

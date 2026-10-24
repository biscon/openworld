#include "vegetation_library.hpp"
#include "test_support.hpp"
#include <chrono>
#include <cstring>
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
// Independent version-2 encoder: compatibility fixtures own their geometry/textures.
void writeVersion2(const std::filesystem::path &path, const VegetationLibrary &l) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    auto u = [&](uint32_t x) {
        for (int k = 0; k < 4; ++k)
            f.put(char(x >> (8 * k)));
    };
    auto scalar = [&](float x) {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        u(bits);
    };
    auto vec = [&](auto v) {
        for (int k = 0; k < v.length(); ++k)
            scalar(v[k]);
    };
    u(0x3256474f);
    u(2);
    u(l.revision);
    u(uint32_t(l.assets.size()));
    u(uint32_t(l.materials.size()));
    u(uint32_t(l.textures.size()));
    for (auto &m : l.materials) {
        vec(m.colour);
        scalar(m.roughness);
        scalar(m.cutoff);
        scalar(m.transmission);
        u(uint32_t(m.colourTexture));
        u(uint32_t(m.normalTexture));
        u(m.masked);
    }
    for (auto &a : l.assets) {
        u(uint32_t(a.category));
        vec(a.minimum);
        vec(a.maximum);
        vec(a.ranges);
        scalar(a.stiffness);
        scalar(a.displacement);
        for (auto &lod : a.lods) {
            u(uint32_t(lod.size()));
            for (auto &p : lod) {
                u(p.material);
                u(uint32_t(p.vertices.size()));
                u(uint32_t(p.indices.size()));
                for (auto &v : p.vertices) {
                    vec(v.position);
                    vec(v.normal);
                    vec(v.tangent);
                    vec(v.uv);
                    vec(v.deformation);
                }
                for (auto index : p.indices)
                    u(index);
            }
        }
    }
}
void checkGrass(const VegetationLibrary &library, uint32_t blades) {
    for (uint32_t a = 0; a < 2; ++a) {
        const auto &asset = library.assets[a];
        require(asset.grass.counts == glm::uvec4(blades, (blades + 2) / 3, (blades + 5) / 6, 0),
                "nested rounded blade counts");
        for (uint32_t lod = 0; lod < 3; ++lod) {
            const auto &p = asset.lods[lod][0];
            const auto &target = asset.lods[std::min(2u, lod + 1)][0];
            for (size_t i = 0; i < p.vertices.size(); ++i) {
                const auto &m = p.grassMorph[i];
                const auto &v = p.vertices[i];
                const uint32_t b = uint32_t(m.centreBlade.w);
                auto end = glm::vec3(m.nextCentreWidth) +
                           (v.position - glm::vec3(m.centreBlade)) * m.nextCentreWidth.w;
                if (b >= asset.grass.counts[std::min(2u, lod + 1)]) {
                    near(m.nextCentreWidth.w, 0, 0, "retired blade collapses completely");
                } else {
                    const auto &root = target.vertices[b * 4 + i % 2];
                    const auto &tip = target.vertices[b * 4 + 2 + i % 2];
                    near(glm::length(end - glm::mix(root.position, tip.position, v.uv.y)), 0, 1e-6,
                         "morph endpoint matches coarse surface");
                    near(glm::length(m.nextDeformation - glm::mix(root.deformation, tip.deformation, v.uv.y)),
                         0, 1e-6, "morph wind weights match coarse surface");
                }
                // Convex morph/fade envelope and finite anchored roots throughout the band.
                for (float t : {0.f, .25f, .5f, 1.f})
                    for (float coverage : {0.f, .4f, 1.f}) {
                        auto q = glm::mix(glm::vec3(m.centreBlade), glm::vec3(m.nextCentreWidth), t) +
                                 (v.position - glm::vec3(m.centreBlade)) *
                                     glm::mix(1.f, m.nextCentreWidth.w, t) * coverage;
                        for (int k = 0; k < 3; ++k)
                            require(q[k] >= asset.minimum[k] - 1e-6 && q[k] <= asset.maximum[k] + 1e-6,
                                    "intermediate geometry stays in asset envelope");
                        if (v.uv.y == 0)
                            near(q.y, 0, 1e-6, "morph and fade preserve grounded roots");
                    }
            }
        }
    }
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
        checkGrass(library, 24);
        for (uint32_t count : {8u, 25u, 64u}) {
            VegetationRecipe recipe;
            recipe.blades = count;
            auto varied = buildVegetationLibrary(recipe);
            checkGrass(varied, count);
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
        malformed = library;
        malformed.assets[0].lods[0][0].grassMorph.pop_back();
        rejects([&] { validate(malformed); });
        malformed = library;
        malformed.assets[0].lods[0][0].grassMorph[0].nextCentreWidth.x = 20;
        rejects([&] { validate(malformed); });
        malformed = library;
        malformed.assets[0].grass.bands.y = 10;
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
        checkGrass(loaded, 24);
        require(loaded.textures[0].bytes == library.textures[0].bytes, "saved texture round trip");
        auto &a = loaded.assets[3].lods[1][1];
        auto &b = library.assets[3].lods[1][1];
        require(a.indices == b.indices && a.vertices.size() == b.vertices.size(), "saved multipart LOD");
        for (size_t i = 0; i < a.vertices.size(); ++i)
            near(glm::length(a.vertices[i].deformation - b.vertices[i].deformation), 0, 0,
                 "saved deformation metadata");
        writeVersion2(dir / "library.bin", library);
        auto legacy = readVegetationLibrary(dir);
        require(legacy.assets[0].grass.counts.x == 0 && legacy.assets[0].lods[0][0].grassMorph.empty(),
                "version 2 keeps legacy rendering without morph metadata");
        require(legacy.assets[0].lods[0][0].indices == library.assets[0].lods[0][0].indices,
                "version 2 preserves stored mesh topology");
        writeVegetationLibrary(dir, legacy);
        require(readVegetationLibrary(dir).assets[0].grass.counts.x == 0,
                "legacy re-save does not opt into morphing");
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

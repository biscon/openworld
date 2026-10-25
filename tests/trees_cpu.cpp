#include "test_support.hpp"
#include "tree_generation.hpp"
#include "trees.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unistd.h>
using namespace ow;
int main() {
    try {
        for (uint32_t s = 0; s < 4; ++s) {
            TreeRecipe r;
            r.species = TreeSpecies(s);
            r.height = 7;
            r.crownRadius = 2;
            r.attractors = 96;
            auto a = generateTree(r), b = generateTree(r);
            validate(TreeLibrary{{a}});
            for (uint32_t lod = 0; lod < 3; ++lod)
                for (uint32_t part = 0; part < 2; ++part) {
                    auto &x = a.lods[lod][part];
                    auto &y = b.lods[lod][part];
                    require(x.indices == y.indices && x.vertices.size() == y.vertices.size(),
                            "reproducible tree topology");
                    for (size_t i = 0; i < x.vertices.size(); ++i)
                        require(x.vertices[i].position == y.vertices[i].position,
                                "reproducible tree positions");
                    if (lod)
                        require(x.indices.size() < a.lods[lod - 1][part].indices.size(),
                                "tree LOD reduces geometry");
                }
            {
                auto bad = a;
                bad.capture.w = .1f;
                bool rejected = false;
                try {
                    validate(TreeLibrary{{bad}});
                } catch (const std::invalid_argument &) {
                    rejected = true;
                }
                require(rejected, "capture must enclose all geometry");
                bad = a;
                bad.lods[0][0].indices[0] = uint32_t(bad.lods[0][0].vertices.size());
                rejected = false;
                try {
                    validate(TreeLibrary{{bad}});
                } catch (const std::invalid_argument &) {
                    rejected = true;
                }
                require(rejected, "out-of-range mesh index rejected");
            }
            auto path = std::filesystem::temp_directory_path() /
                        ("openworld-tree-" + std::to_string(getpid()) + ".bin");
            writeTreeLibrary(path, TreeLibrary{{a}});
            auto copy = readTreeLibrary(path);
            require(copy.assets[0].lods[0][0].indices == a.lods[0][0].indices,
                    "tree serialization round trip");
            {
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                f << "OWTR";
            }
            bool failed = false;
            try {
                readTreeLibrary(path);
            } catch (...) {
                failed = true;
            }
            std::filesystem::remove(path);
            require(failed, "truncated tree file rejected");
        }
        for (int z = -10; z <= 10; ++z)
            for (int y = -10; y <= 10; ++y)
                for (int x = -10; x <= 10; ++x) {
                    glm::vec3 d(x, y, z);
                    if (glm::length(d) < .1f)
                        continue;
                    d = glm::normalize(d);
                    near(glm::length(treeOctDecode(treeOctEncode(d)) - d), 0, 1e-5,
                         "octahedral full sphere round trip");
                    auto b = treeViewBlend(d, 12);
                    near(b.weights.x + b.weights.y + b.weights.z, 1, 1e-6, "impostor weights sum to one");
                    for (int i = 0; i < 3; ++i)
                        require(b.weights[i] >= -1e-6 && b.frames[i] < 144,
                                "bounded impostor frame selection");
                }
        for (auto k : {TreeCellKey{-1, -17}, TreeCellKey{0, 0}, TreeCellKey{1LL << 40, -(1LL << 40)}}) {
            auto p = treeCellOrigin(k);
            require(treeCellAt(p) == k, "large signed cell round trip");
            require(treeCellAt(translated(p, {63.99, 0, 63.99})) == k, "half-open cell extent");
            require(treeCandidateHash(k, 17, 0) != treeCandidateHash(k, 17, 1),
                    "candidate identity distinguishes lanes");
        }
        for (auto value : {INT64_MIN, INT64_MAX}) {
            WorldPosition p;
            p.x = value;
            bool rejected = false;
            try {
                treeCellAt(p);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "extreme coordinates rejected without signed overflow");
        }
        TreeSettings invalid;
        invalid.distance = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try {
            validate(invalid);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "nonfinite settings rejected");
        TreeStreamConfig config;
        config.cellCapacity = 64;
        config.maximumDistance = 64;
        TreeResidency residency(config);
        residency.demand({}, 1, 0, true, 64);
        require(!residency.ordered().empty(), "tree demand populated");
        auto slot = residency.ordered()[0];
        TreeCellHandle handle{slot, residency.slot(slot).generation};
        residency.used(slot, 3);
        residency.demand({}, 2, 2, false, 64);
        require(!residency.valid(handle) && residency.slot(slot).state == TreeResidency::State::Retiring,
                "submitted cell cannot be reused");
        residency.demand({}, 2, 3, false, 64);
        require(residency.slot(slot).state == TreeResidency::State::Empty, "completed cell retires");
        TextureData depth;
        depth.width = depth.height = 4;
        depth.format = TextureFormat::R16;
        depth.levels = 3;
        require(textureBytes(depth) == 42, "16-bit depth mip accounting");
        std::cout << "Tree CPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

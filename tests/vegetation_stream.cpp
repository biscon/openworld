#include "vegetation_stream.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <iostream>
#include <map>
using namespace ow;
int main() {
    try {
        for (auto k : {VegetationCellKey{-1, -257}, {0, 0}, {(int64_t(1) << 40) + 17, -(int64_t(1) << 39)}}) {
            require(vegetationCellAt(vegetationCellOrigin(k)) == k, "signed geographic cell roundtrip");
            auto p = translated(vegetationCellOrigin(k), {3.999, 0, 3.999});
            require(vegetationCellAt(p) == k, "half-open cell ownership");
            require(vegetationCellAt(translated(p, {.002, 0, .002})) == VegetationCellKey{k.x + 1, k.z + 1},
                    "cell edge crossing");
        }
        require(vegetationCandidateHash({0, 0}, 173, 0, 4) !=
                    vegetationCandidateHash({int64_t(1) << 32, 0}, 173, 0, 4),
                "high coordinate words participate");
        VegetationStreamConfig config;
        config.cellCapacity = 32;
        config.demandRadius = 6;
        config.retentionRadius = 8;
        VegetationResidency a(config), b(config);
        WorldPosition origin;
        origin.local = {1, 0, 1};
        a.demand(origin, 1, 0, true);
        b.demand(translated(origin, {400, 0, 400}), 1, 0, true);
        b.demand(origin, 1, 0, true);
        std::map<std::pair<int64_t, int64_t>, uint32_t> identities;
        for (auto i : a.ordered()) {
            auto &s = a.slot(i);
            identities[{s.key.x, s.key.z}] = vegetationCandidateHash(s.key, 173, 0, 17);
        }
        for (auto i : b.ordered()) {
            auto &s = b.slot(i);
            require(identities.at({s.key.x, s.key.z}) == vegetationCandidateHash(s.key, 173, 0, 17),
                    "loading order independence");
        }
        auto index = a.ordered().front();
        auto old = VegetationCellHandle{index, a.slot(index).generation};
        a.used(index, 7);
        a.demand(translated(origin, {1000, 0, 0}), 1, 6, true);
        require(!a.valid(old) && a.slots()[index].state == VegetationResidency::State::Retiring,
                "submitted slot cannot be reused");
        a.demand(origin, 2, 7, true);
        require(!a.valid(old), "recycled generation rejects stale handle");
        for (auto i : a.ordered())
            require(a.slot(i).revision == 2 && !a.slot(i).generated, "input revision invalidates population");
        a.demand(origin, 2, 7, false);
        require(a.ordered().empty(), "disable cancels demand");
        config.cellCapacity = 2;
        VegetationResidency tiny(config);
        tiny.demand(origin, 1, 0, true);
        require(tiny.ordered().size() == 2 && tiny.deferred() > 0, "bounded pressure with explicit deferral");
        bool rejected = false;
        try {
            config.retentionRadius = 113;
            VegetationResidency invalid(config);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "invalid bounded config rejected");
        RendererOptions invalidRenderer;
        invalidRenderer.vegetation.streaming.enabled = true;
        rejects([&] { Renderer renderer(invalidRenderer); },
                "unsupported terrain path rejected before Vulkan startup");
        std::cout << "Vegetation residency checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

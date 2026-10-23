#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
using namespace ow;

int main() {
    try {
        RendererOptions o;
        o.ui = false;
        o.requireValidation = true;
        o.shaderDirectory = OPENWORLD_SHADER_DIR;
        o.terrainGrid = {8, .25f};
        o.terrainCapacity = 128;
        o.normalSubdivisions = 1;
        Renderer r(o);
        Camera camera;
        camera.reset(o.terrainGrid, {}, 60);
        r.render(camera, {});
        const auto original = r.readTerrainSnapshot();
        for (uint32_t seed : {1u, 7u, 42u}) {
            TerrainRequest request;
            request.settings.ridgeStrength = request.settings.baseDetail = 1;
            request.settings.seed = seed;
            const auto map = r.sampleTerrainGeography({128, 500}, request);
            size_t plains = 0, mountains = 0;
            double plainSlope = 0, mountainSlope = 0;
            float low = 1e10f, high = -1e10f;
            std::array<size_t, 5> locations{};
            std::array<bool, 5> found{};
            float highest = -1e10f, lowestMountain = 1e10f;
            for (size_t i = 0; i < map.size(); ++i) {
                const auto &v = map[i].base;
                const float slope = glm::length(glm::vec2(v));
                require(std::isfinite(slope) && std::isfinite(v.w), "Finite regional height and gradient");
                require(v.z >= 0 && v.z <= 1, "Bounded landform weight");
                require(v.w <= request.settings.heightScale * 1.25f &&
                            map[i].detail.w <= request.settings.heightScale * 1.25f,
                        "Camera reset bound covers both surfaces");
                low = std::min(low, v.w);
                high = std::max(high, v.w);
                if (v.z == 0) {
                    ++plains;
                    plainSlope += slope;
                    near(v.w, map[i].detail.w, 0, "Pure plains are protected from erosion");
                    if (!found[0]) {
                        locations[0] = i;
                        found[0] = true;
                    }
                }
                if (v.z > 0.2f && v.z < 0.8f && !found[1]) {
                    locations[1] = i;
                    found[1] = true;
                }
                if (v.z == 1) {
                    ++mountains;
                    mountainSlope += slope;
                    if (!found[2]) {
                        locations[2] = i;
                        found[2] = true;
                    }
                    if (v.w < lowestMountain) {
                        lowestMountain = v.w;
                        locations[4] = i;
                        found[4] = true;
                    }
                    if (v.w > highest) {
                        highest = v.w;
                        locations[3] = i;
                        found[3] = true;
                    }
                }
            }
            require(plains > map.size() / 4 && mountains > map.size() / 100,
                    "Owned seeds contain broad plains and mountains");
            require(high - low > 600, "Regional relief includes tall mountains");
            require(plainSlope / plains < 0.02 && mountainSlope / mountains > 20 * plainSlope / plains,
                    "Plains remain much gentler than mountains");
            require(std::all_of(found.begin(), found.end(), [](bool b) { return b; }),
                    "All gradient fixtures represented");
            for (size_t location : locations) {
                // 2 m square around a point selected on the 500 m map lattice.
                request.chunk = {int64_t(location % 129) * 250, int64_t(location / 129) * 250};
                const auto samples = r.sampleTerrainGeography({8, .25f}, request);
                for (uint32_t z = 1; z < 8; ++z)
                    for (uint32_t x = 1; x < 8; ++x) {
                        size_t i = z * 9 + x;
                        const float dx = (samples[i + 1].base.w - samples[i - 1].base.w) / .5f;
                        const float dz = (samples[i + 9].base.w - samples[i - 9].base.w) / .5f;
                        // Float height subtraction and rounded-crest curvature over
                        // the half-metre stencil, not cross-device bitwise agreement.
                        near(samples[i].base.x, dx, .002 + std::abs(dx) * .02, "Analytic world X gradient");
                        near(samples[i].base.y, dz, .002 + std::abs(dz) * .02, "Analytic world Z gradient");
                    }
            }
            // Owned 2 km patch near a mountain selected from this seed's map.
            // Compare shape statistics, not a stored device-specific image.
            const size_t peak = locations[3];
            const ChunkCoord patch{int64_t(peak % 129) * 500 / 2048,
                                   int64_t(peak / 129) * 500 / 2048};
            double curvature[5]{};
            const glm::vec2 shapes[] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {.4f, .6f}};
            for (size_t shape = 0; shape < 5; ++shape) {
                request.chunk = patch;
                request.settings.ridgeStrength = shapes[shape].x;
                request.settings.baseDetail = shapes[shape].y;
                const auto samples = r.sampleTerrainGeography({128, 16}, request);
                float low = 1e10f, high = -1e10f;
                for (size_t i = 0; i < samples.size(); ++i) {
                    const auto &s = samples[i];
                    require(std::isfinite(s.base.w) && std::isfinite(s.detail.w) &&
                                std::isfinite(s.detail.x) && std::isfinite(s.detail.y) &&
                                std::isfinite(s.detail.z), "Finite shaped terrain and erosion driver");
                    low = std::min(low, s.base.w);
                    high = std::max(high, s.base.w);
                    if (i % 129 > 0 && i % 129 < 128 && i / 129 > 0 && i / 129 < 128) {
                        const double dx = samples[i - 1].base.w - 2.0 * s.base.w + samples[i + 1].base.w;
                        const double dz = samples[i - 129].base.w - 2.0 * s.base.w + samples[i + 129].base.w;
                        curvature[shape] += dx * dx + dz * dz;
                    }
                }
                require(high - low > 20, "Smoothing retains broad mountain relief");
                request.chunk = {int64_t(peak % 129) * 250, int64_t(peak / 129) * 250};
                const auto fine = r.sampleTerrainGeography({8, .25f}, request);
                for (uint32_t z = 1; z < 8; ++z)
                    for (uint32_t x = 1; x < 8; ++x) {
                        const size_t i = z * 9 + x;
                        const float dx = (fine[i + 1].base.w - fine[i - 1].base.w) / .5f;
                        const float dz = (fine[i + 9].base.w - fine[i - 9].base.w) / .5f;
                        near(fine[i].base.x, dx, .002 + std::abs(dx) * .02, "Shaped X gradient");
                        near(fine[i].base.y, dz, .002 + std::abs(dz) * .02, "Shaped Z gradient");
                        if (shape == 0) {
                            near(fine[i].base.x, fine[i].detail.y, 1e-6, "Smooth driver follows base X slope");
                            near(fine[i].base.y, fine[i].detail.z, 1e-6, "Smooth driver follows base Z slope");
                        }
                    }
                request.settings.erosionStrength = 0;
                for (const auto &s : r.sampleTerrainGeography({8, .25f}, request))
                    require(s.base.w == s.detail.w, "All shapes preserve zero erosion identity");
                request.settings.erosionStrength = .22f;
                request.settings.heightScale = 0;
                for (const auto &s : r.sampleTerrainGeography({8, .25f}, request))
                    require(s.base.w == 0 && s.detail.w == 0 && s.base.x == 0 && s.base.y == 0,
                            "All shapes preserve zero height identity");
                request.settings.heightScale = 1200;
            }
            std::cout << "Shape curvature seed " << seed << " patch " << patch.x << "," << patch.z << ":";
            for (double value : curvature) std::cout << ' ' << value;
            std::cout << '\n';
            require(curvature[0] < curvature[3] * .25,
                    "Smooth base substantially reduces short-scale curvature");
            require(curvature[0] < curvature[1] && curvature[2] < curvature[3],
                    "Base detail independently adds smaller-scale features");
        }
        // Fixed, wholly plain 2 km areas: remove a fitted plane so a regional
        // tilt alone cannot satisfy the local-relief requirement.
        const uint32_t plainSeeds[] = {1, 7, 42, 34434};
        const ChunkCoord plainChunks[] = {{8, 0}, {10, 0}, {0, 0}, {0, 0}};
        for (size_t fixture = 0; fixture < 4; ++fixture) {
            TerrainRequest plainRequest{plainChunks[fixture], {}};
            plainRequest.settings.ridgeStrength = plainRequest.settings.baseDetail = 1;
            plainRequest.settings.seed = plainSeeds[fixture];
            const auto plain = r.sampleTerrainGeography({128, 16}, plainRequest);
            double mean = 0, xh = 0, zh = 0, xx = 0, zz = 0, slope = 0;
            for (size_t i = 0; i < plain.size(); ++i) {
                const auto &v = plain[i].base;
                const double x = (int(i % 129) - 64) * 16, z = (int(i / 129) - 64) * 16;
                require(v.z == 0 && v.w == plain[i].detail.w, "Plains fixture stays un-eroded");
                mean += v.w;
                xh += x * v.w;
                zh += z * v.w;
                xx += x * x;
                zz += z * z;
                slope += glm::length(glm::vec2(v));
            }
            mean /= plain.size();
            double low = 1e10, high = -1e10;
            for (size_t i = 0; i < plain.size(); ++i) {
                const double x = (int(i % 129) - 64) * 16, z = (int(i / 129) - 64) * 16;
                const double residual = plain[i].base.w - mean - xh / xx * x - zh / zz * z;
                low = std::min(low, residual);
                high = std::max(high, residual);
            }
            require(high - low > 10 && high - low < 40 && slope / plain.size() < .04,
                    "Plains have gentle local relief independent of regional tilt");
            std::cout << "Plain seed " << plainSeeds[fixture] << " detrended relief=" << high - low << " m\n";
        }
        TerrainRequest request{{1320, 1320}, {}}; // mountain at (2640,2640)
        const auto mountain = r.sampleTerrainGeography({8, .25f}, request);
        request.settings.erosionWavelength = 333;
        const auto changed = r.sampleTerrainGeography({8, .25f}, request);
        double difference = 0;
        for (size_t i = 0; i < mountain.size(); ++i) {
            require(mountain[i].base == changed[i].base, "Erosion wavelength never changes base geography");
            difference += std::abs(mountain[i].detail.w - changed[i].detail.w);
        }
        require(difference > 1, "Independent erosion wavelength affects mountain displacement");
        request.settings.erosionStrength = 0;
        for (const auto &s : r.sampleTerrainGeography({8, .25f}, request))
            near(s.base.w, s.detail.w, 0, "Zero erosion gives exact identity");
        request.settings.heightScale = 0;
        request.settings.erosionStrength = 1;
        for (const auto &s : r.sampleTerrainGeography({8, .25f}, request)) {
            require(s.base.x == 0 && s.base.y == 0 && s.base.w == 0 && s.detail.w == 0,
                    "Zero height remains flat under erosion");
        }
        request.settings = {};
        for (glm::vec2 shape : {glm::vec2(0, 0), glm::vec2(.4f, .6f), glm::vec2(1, 1)}) {
            request.settings.ridgeStrength = shape.x;
            request.settings.baseDetail = shape.y;
            for (auto coord : {ChunkCoord{-8, -4}, ChunkCoord{1000000, -1000000}}) {
                request.chunk = coord;
                const auto left = r.sampleTerrainGeography({8, .25f}, request);
                ++request.chunk.x;
                const auto right = r.sampleTerrainGeography({8, .25f}, request);
                request.chunk = coord;
                const auto repeat = r.sampleTerrainGeography({8, .25f}, request);
                require(std::memcmp(left.data(), repeat.data(), left.size() * sizeof(left[0])) == 0,
                        "Geography probes are generation-order independent");
                for (size_t z = 0; z < 9; ++z) {
                    require(left[z * 9 + 8].base == right[z * 9].base,
                            "Shared gradients, weights and heights agree exactly");
                    require(left[z * 9 + 8].detail == right[z * 9].detail, "Shared eroded heights agree exactly");
                }
            }
        }
        rejects([&] { r.sampleTerrainGeography({129, .25f}, request); }, "Probe capacity enforced");
        rejects([&] { r.sampleTerrainGeography({8, .125f}, request); },
                "Canonical probes reject sub-quarter spacing");
        r.render(camera, {});
        const auto restored = r.readTerrainSnapshot();
        require(std::memcmp(original.data(), restored.data(), original.size() * sizeof(original[0])) == 0,
                "Ordinary rendering restores terrain overwritten by diagnostics");
        require(r.validationErrors() == 0, "Geography diagnostics Vulkan synchronization validation");
        std::cout << "Regional geography GPU tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

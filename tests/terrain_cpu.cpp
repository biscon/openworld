#include "camera.hpp"
#include "test_support.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
namespace {
thread_local bool trackAllocations = false;
thread_local size_t allocations = 0;
} // namespace
void *operator new(std::size_t size) {
    if (trackAllocations)
        ++allocations;
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace ow;
void topology() {
    TerrainGrid grid{9, 4};
    const auto indices = terrainIndices(grid);
    require(indices.size() == 9 * 9 * 6, "Index count");
    auto position = [](uint32_t i) { return glm::vec3(float(i % 10), 0, float(i / 10)); };
    for (size_t i = 0; i < indices.size(); i += 3) {
        for (size_t j = 0; j < 3; ++j)
            require(indices[i + j] < 100, "Index bounds");
        const auto a = position(indices[i]), b = position(indices[i + 1]), c = position(indices[i + 2]);
        require(glm::cross(b - a, c - a).y > 0, "Triangle winding");
    }
    validateGrid({1024, 0.5f}); // Maximum grid without a large routine-test allocation.
    rejects([] { terrainIndices({7, 4}); }, "Reject small grid");
    rejects([] { terrainIndices({1025, 4}); }, "Reject excessive grid");
    rejects([] { terrainIndices({8, 0}); }, "Reject zero spacing");
}
void gridConfiguration() {
    for (float spacing : {4.0f, 1.0f, 0.5f}) {
        const auto grid = terrainGrid(512, spacing);
        near(grid.extent(), 512, 0, "Spacing preserves footprint");
        near(grid.spacing, spacing, 0, "Requested spacing");
    }
    for (uint32_t cells : {33u, 129u, 511u, 999u, 1023u})
        require(terrainGrid(512, 512.0f / float(cells)).cells == cells,
                "Rounded float spacing retains cell count");
    require(terrainGrid(512, 0.5f) == TerrainGrid{}, "Default half-metre patch");
    rejects([] { terrainGrid(513, 4); }, "Fractional cells rejected");
    rejects([] { terrainGrid(1024, 0.5f); }, "Capacity enforced");
    rejects([] { terrainGrid(2, 1); }, "Minimum grid enforced");
    rejects([] { terrainGrid(512, 0); }, "Zero spacing rejected");
    rejects([] { terrainGrid(-512, 1); }, "Negative extent rejected");
    rejects([] { terrainGrid(std::numeric_limits<double>::infinity(), 1); }, "Infinite extent rejected");
    rejects([] { terrainGrid(512, std::numeric_limits<float>::quiet_NaN()); }, "NaN spacing rejected");
}
void coordinates() {
    TerrainGrid g{32, 4};
    TerrainRequest r{{-2, 3}, {1, 777, 400, 0.22f}};
    auto p = terrainParameters(g, r);
    for (size_t i = 0; i < p.domains.size(); ++i)
        for (int axis = 0; axis < 2; ++axis) {
            const auto &d = p.domains[i];
            const double chunk = axis ? r.chunk.z : r.chunk.x;
            require(d.fractionFrequency[axis] >= 0 && d.fractionFrequency[axis] < 1, "Fraction normalized");
            near(double(d.lattice[axis]) + d.fractionFrequency[axis],
                 chunk * g.extent() * d.fractionFrequency.z, 1e-6, "Negative coordinate decomposition");
        }
    r.chunk = {1000000, -1000000};
    terrainParameters(g, r);
    r.chunk.x = std::numeric_limits<int64_t>::max();
    rejects([&] { terrainParameters(g, r); }, "Reject unrepresentable domain");
    r = {};
    r.settings.heightScale = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { terrainParameters(g, r); }, "Reject NaN");
    r = {};
    r.settings.wavelength = 0;
    rejects([&] { terrainParameters(g, r); }, "Reject zero wavelength");
    r = {};
    r.settings.creaseRounding = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { terrainParameters(g, r); }, "Reject invalid crease rounding");
    r = {};
    r.settings.regionWavelength = 4095;
    rejects([&] { terrainParameters(g, r); }, "Reject too-small region wavelength");
    r = {};
    r.settings.regionWavelength = std::numeric_limits<float>::infinity();
    rejects([&] { terrainParameters(g, r); }, "Reject infinite region wavelength");
    r = {};
    r.settings.erosionWavelength = 8193;
    rejects([&] { terrainParameters(g, r); }, "Reject excessive erosion wavelength");
    r = {};
    r.settings.erosionWavelength = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { terrainParameters(g, r); }, "Reject nonfinite erosion wavelength");
    r = {};
    r.settings.erosionStrength = -1;
    rejects([&] { terrainParameters(g, r); }, "Reject negative erosion");
}
void requests() {
    TerrainUpdates q({32, 4});
    require(q.pending(), "Initial request pending");
    TerrainRequest r;
    q.request(r);
    require(q.pending(), "Skipped frames preserve request");
    q.submitted();
    require(!q.pending() && q.count() == 1, "Submission commits request");
    q.request(r);
    require(!q.pending(), "Equal request cached");
    require(r.settings.ridgeStrength == .50f && r.settings.baseDetail == .65f, "Requested startup terrain");
    for (auto field : {&TerrainSettings::ridgeStrength, &TerrainSettings::baseDetail}) {
        auto changed = r;
        for (float value : {0.f, .5f, 1.f}) {
            changed.settings.*field = value;
            const auto p = terrainParameters({32, 4}, changed);
            require(p.geography.y == changed.settings.ridgeStrength &&
                        p.geography.z == changed.settings.baseDetail, "Smoothness GPU parameters");
            q.request(changed);
            require(q.pending() == (value != r.settings.*field), "Smoothness edits invalidate terrain");
        }
        q.request(r);
        require(!q.pending(), "Reverting smoothness coalesces without regeneration");
        for (float value : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
            changed.settings.*field = value;
            rejects([&] { q.request(changed); }, "Reject invalid smoothness transactionally");
            require(q.requested() == r && !q.pending(), "Invalid smoothness preserves request");
        }
    }
    for (auto field : {&TerrainSettings::regionWavelength, &TerrainSettings::erosionWavelength}) {
        auto changed = r;
        changed.settings.*field *= 1.5f;
        require(!(r == changed), "Each independent wavelength changes world identity");
        q.request(changed);
        require(q.pending(), "New wavelength requests regeneration");
        q.request(r);
        require(!q.pending(), "Reverting to applied settings coalesces without regeneration");
        changed.settings.*field = 0;
        rejects([&] { q.request(changed); }, "Reject invalid new wavelength transactionally");
        require(q.requested() == r && !q.pending(), "Invalid wavelength preserves applied request");
    }
    r.settings.creaseRounding = 0;
    q.request(r);
    require(q.pending(), "Crease rounding invalidates cached terrain");
    r.settings.seed = 7;
    q.request(r);
    r.settings.seed = 9;
    q.request(r);
    require(q.pending() && q.requested().settings.seed == 9, "Requests coalesce");
    auto invalid = r;
    invalid.settings.wavelength = 0;
    rejects([&] { q.request(invalid); }, "Reject bad request");
    require(q.requested() == r && q.pending(), "Bad request preserves pending work");
    q.submitted();
    require(q.count() == 2 && !q.pending(), "Latest request commits once");
    q.setGrid({32, 4});
    require(!q.pending(), "Equal grid cached");
    q.setGrid({64, 2});
    require(q.pending() && q.count() == 2, "Resolution change stays pending until submission");
    rejects([&] { q.setGrid({1025, 1}); }, "Invalid grid rejected");
    require(q.pending(), "Invalid grid preserves pending work");
    q.submitted();
    require(!q.pending() && q.count() == 3, "Resolution committed");
    q.setGrid({64, 1});
    require(q.pending(), "Spacing-only change regenerates");
    q.setGrid({64, 2});
    require(!q.pending(), "Returning to submitted grid cancels unnecessary generation");
}
void cameras() {
    TerrainGrid g{512, 4};
    Camera c;
    c.reset(g);
    auto start = c.local;
    CameraInput i;
    i.movement = {1, 0, 1};
    c.update(i, 0.1f, g);
    near(glm::length(c.local - start), 10, 1e-5, "Diagonal normalized");
    Camera a, b;
    a.reset(g);
    b = a;
    i.movement = {0, 0, 1};
    a.update(i, 0.1f, g);
    for (int j = 0; j < 10; ++j)
        b.update(i, 0.01f, g);
    near(glm::length(a.local - b.local), 0, 1e-5, "Timestep independent movement");
    c.reset(g);
    start = c.local;
    i.boost = true;
    c.update(i, 0.1f, g);
    near(glm::length(c.local - start), 40, 1e-5, "Boost");
    i = {};
    i.look = {100000, 100000};
    c.update(i, 0, g);
    require(std::abs(c.pitch) < 1.57f, "Pitch clamp");
    c.reset(g);
    c.chunk = {-3, 4};
    c.local = {-1, 10, g.extent() + 2};
    c.update({}, 0, g);
    require(c.chunk.x == -4 && c.chunk.z == 5, "Chunk carry");
    near(c.local.x, g.extent() - 1, 0, "Negative local normalized");
    near(c.local.z, 2, 0, "Positive carry");
    auto offset = c.terrainOffset({-4, 5}, g);
    near(offset.x, -c.local.x, 0, "Relative origin");
    const auto beforeRebase = c.terrainOffset({}, g);
    const auto oldPitch = c.pitch;
    c.rebaseGrid(g, {1024, 0.5f});
    near(glm::length(c.terrainOffset({}, {1024, 0.5f}) - beforeRebase), 0, 0,
         "Footprint rebase preserves world position");
    near(c.pitch, oldPitch, 0, "Rebase preserves look direction");
    c.rebaseGrid({1024, 0.5f}, g);
    near(glm::length(c.terrainOffset({}, g) - beforeRebase), 0, 0, "Rebase round trip");
    c.reset(g);
    Camera reset;
    reset.reset(g);
    near(glm::length(c.local - reset.local), 0, 0, "Camera reset");
}
void batches() {
    TerrainGrid grid{16, 0.5f};
    TerrainRequest centre{{-3, 5}, {}};
    auto request = terrainNeighbourhood(centre);
    require(request.count == 9, "Nine neighbourhood chunks");
    for (uint32_t i = 0; i < 9; ++i)
        require(request.chunks[i].x == -4 + int64_t(i % 3) &&
                request.chunks[i].z == 4 + int64_t(i / 3), "Row-major signed neighbourhood");
    rejects([&] { validateBatch(grid, request, 1); }, "Reject batch larger than configured capacity");
    TerrainBatchUpdates updates(grid, 9);
    updates.request(request);
    updates.submitted(0x1ff);
    require(updates.count() == 9, "Count submitted chunks");
    for (uint32_t i = 0; i < 9; ++i) require(!updates.pending(i), "Batch cached");
    auto bad = request;
    bad.chunks[8] = bad.chunks[0];
    rejects([&] { updates.request(bad); }, "Reject duplicate batch");
    bad = request;
    bad.chunks[8].x = INT64_MAX;
    rejects([&] { updates.request(bad); }, "Validate final slot before committing any slot");
    require(updates.requested().chunks[8].x == request.chunks[8].x && !updates.pending(0),
            "Invalid batch leaves previous request intact");
    bad = request; bad.count = 10;
    rejects([&] { updates.request(bad); }, "Reject excess slots");
    bad.count = 0;
    rejects([&] { updates.request(bad); }, "Reject empty batch");
    rejects([&] { TerrainBatchUpdates invalid(grid, 0); }, "Reject zero capacity");
    rejects([&] { terrainNeighbourhood({{INT64_MAX, 0}, {}}); }, "Positive overflow");
    rejects([&] { terrainNeighbourhood({{0, INT64_MIN}, {}}); }, "Negative overflow");
    request.settings.seed = 7;
    updates.request(request);
    request.settings.seed = 8;
    updates.request(request);
    updates.submitted(1);
    require(!updates.pending(0) && updates.pending(8) && updates.count() == 10,
            "Only submitted slots commit latest request");
    updates.request(terrainNeighbourhood(centre, true));
    updates.setGrid({8, 0.5f});
    updates.request(request);
    for (uint32_t i = 0; i < 9; ++i) require(updates.pending(i), "Reactivated slots use current grid");
    Camera camera;
    camera.chunk = centre.chunk;
    camera.local = {7.9, 12, 4};
    const auto before = camera.terrainOffset(request.chunks[4], grid);
    camera.local.x += 0.2;
    camera.update({}, 0, grid);
    const auto after = camera.terrainOffset(request.chunks[4], grid);
    near(after.x - before.x, -0.2, 1e-6, "Neighbourhood offset continuous across camera chunk boundary");
}
void allocationBudget() {
    TerrainGrid grid;
    TerrainUpdates updates(grid);
    TerrainBatchUpdates batchUpdates(grid, 9);
    TerrainRequest request;
    Camera camera;
    camera.reset(grid);
    CameraInput input;
    input.movement = {1, 0, 1};
    allocations = 0;
    trackAllocations = true;
    for (uint32_t i = 0; i < 1000; ++i) {
        request.settings.seed = i;
        updates.setGrid(i % 2 ? TerrainGrid{128, 4} : TerrainGrid{1024, 0.5f});
        updates.request(request);
        batchUpdates.request(terrainNeighbourhood(request));
        batchUpdates.submitted(0x1ff);
        const auto parameters = terrainParameters(grid, updates.requested());
        if (parameters.dimensions.y != i)
            std::abort();
        updates.submitted();
        camera.update(input, 0.016f, grid);
        const auto view = camera.viewProjection(1.6f);
        if (!std::isfinite(view[0][0]))
            std::abort();
    }
    trackAllocations = false;
    require(allocations == 0, "Core updates allocate no C++ heap memory");
    std::cout << "1000 terrain/camera updates: " << allocations << " C++ heap allocations\n";
}
int main() {
    try {
        gridConfiguration();
        topology();
        coordinates();
        requests();
        cameras();
        batches();
        allocationBudget();
        std::cout << "CPU terrain/camera tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

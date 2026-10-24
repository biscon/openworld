#pragma once
#include "renderer.hpp"
namespace ow {
// Explicit diagnostic bookmark; no GPU handles, assets or streaming residency.
struct ReviewView {
    uint32_t version = 5;
    Camera camera;
    TerrainSettings terrain;
    ChunkCoord terrainOrigin; // Fixed inspection terrain may differ from the
                              // camera chunk.
    SceneSettings scene;
    TerrainLodReviewState lod;
    RendererOptions startup;
};
ReviewView loadReviewView(const std::filesystem::path &);
void saveReviewView(const std::filesystem::path &, const ReviewView &);
void validateReviewView(const ReviewView &);
// Copy only persisted startup fields, preserving window/validation/shader/cache
// paths.
void applyReviewStartup(RendererOptions &, const ReviewView &);
bool compatibleReviewStartup(const RendererOptions &, const ReviewView &);
} // namespace ow

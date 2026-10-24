#pragma once

#include "atmosphere.hpp"
#include "gtao.hpp"
#include "lighting.hpp"
#include "material_assets.hpp"
#include "scene.hpp"
#include "shadows.hpp"
#include "temporal.hpp"
#include "terrain_lod.hpp"
#include "terrain_stream.hpp"
#include "visibility.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct SDL_Window;

namespace ow {

struct RendererOptions {
    SDL_Window *window = nullptr; // Null selects offscreen rendering.
    bool validation = true;
    bool requireValidation = false;
    std::filesystem::path shaderDirectory;
    std::filesystem::path materialDirectory; // Empty uses owned fallback textures.
    bool forceFixedMaterials = false, forceRgbaMaterials = false, forceTrilinear = false;
    uint64_t materialBudgetBytes = 320ull * 1024 * 1024;
    SceneCapacity sceneCapacity;
    ShadowCapacity shadowCapacity;
    bool forceDirectShadows = false, forceManualShadowFiltering = false, forceNoShadowDepth = false;
    uint32_t visibilityCapacity = 65536;
    uint64_t atmosphereBudgetBytes = 64ull << 20;
    bool forceNoAtmosphere = false;
    bool curvedHorizon = false;
    uint64_t taaBudgetBytes = 512ull << 20, bloomBudgetBytes = 128ull << 20;
    bool forceNoTaa = false, forceNoBloom = false;
    uint64_t gtaoBudgetBytes = 512ull * 1024 * 1024;
    bool forceNoGtao = false, forceWideGtao = false;
    bool forceFixedIndirect = false, forceSingleIndirect = false, forceDirectVisibility = false,
         forceNoHiZ = false;
    TerrainGrid terrainGrid;
    uint32_t normalSubdivisions = 2;   // 2 gives 0.25 m normals over the 0.5 m demo mesh.
    uint32_t terrainCapacity = 1024;   // Maximum cells/side; storage allocated once.
    uint32_t terrainChunkCapacity = 1; // Fixed slots; demo selects nine.
    TerrainLodConfig lod;
    TerrainStreamingConfig streaming;
    bool ui = true;
    uint32_t width = 1280, height = 800; // Offscreen extent.
    uint32_t renderHeight = 0;           // Scene height; zero follows output. Width follows output aspect.
};

struct SceneSettings {
    ShadingMode shading = ShadingMode::Pbr;
    AtmosphereSettings atmosphere;
    TaaSettings taa;
    BloomSettings bloom;
    GtaoSettings gtao;
    ShadowSettings shadows;
    bool terrainVisible = true;
    LocalLightMode localLights = LocalLightMode::Clustered;
    uint32_t clusterDiagnostic = 0; // 0 shaded, 1 occupancy, 2 overflow.
    MaterialSettings material;
    TerrainMaterialSettings terrainMaterial;
    LightingSettings lighting;
    OutputSettings output;
    int viewRadius = 2;
    bool lod = true;
    bool lodColours = false;
    bool freezeLod = false;
    VisibilityMode visibility = VisibilityMode::Indirect;
    bool opaqueDepthPrepass = false; // Optional; HiZ or effective GTAO forces it.
    bool terrainLodQuality = true, terrainLodErrorView = false;
    float terrainLodPixelError = 2;
    bool frustumCulling = true;
    bool eroded = true;
    bool normalView = false;
    bool wireframe = false;
    bool chunkBorders = false;
    bool filteredNormals = true; // Draw-only comparison with vertex interpolation.
    float background[3] = {0.018f, 0.027f, 0.045f};
};

// Engine-facing boundary. Vulkan resource and synchronization details stay in
// the implementation.
class Renderer {
  public:
    explicit Renderer(const RendererOptions &options);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    LightHandle createLight(const LocalLight &);
    bool updateLight(LightHandle, const LocalLight &);
    bool removeLight(LightHandle);
    MeshHandle uploadMesh(const MeshData &);
    bool releaseMesh(MeshHandle);
    InstanceHandle createInstance(const MeshInstance &);
    bool updateInstance(InstanceHandle, const MeshInstance &);
    bool removeInstance(InstanceHandle);
    TextureHandle uploadTexture(const TextureData &);
    bool releaseTexture(TextureHandle);
    MaterialHandle createMaterial(const MeshMaterial &);
    bool releaseMaterial(MaterialHandle);
    const SceneStats &sceneStats() const;
    const MaterialStats &materialStats() const;
    bool
    reloadMaterials(const std::filesystem::path &); // Explicit asset load; published on subsequent frames.
    void resize();
    void resizeOffscreen(uint32_t width, uint32_t height);
    // Queued for next frame; 0 selects native, otherwise 1..16384.
    void setRenderHeight(uint32_t height);
    void resetTemporalHistory();
    bool invalidateInstanceHistory(InstanceHandle);
    const AtmosphereStats &atmosphereStats() const;
    AtmosphereSnapshot readAtmosphereSnapshot();
    const PostStats &postStats() const;
    PostSnapshot readPostSnapshot();
    HdrSnapshot readHdrSnapshot(); // Blocking diagnostic, pre-exposed pixels.
    const RenderingStats &renderingStats() const;
    const VisibilityStats &visibilityStats() const;
    const GtaoStats &gtaoStats() const;
    const ShadowStats &shadowStats() const;
    ShadowSnapshot readShadowSnapshot(); // Blocking diagnostic, never a rendering input.
    GtaoSnapshot readGtaoSnapshot();     // Blocking diagnostic; last frame must have active GTAO.
    VisibilityPyramidSnapshot readVisibilityPyramid(); // Blocking diagnostic; last frame must use HiZ.
    bool render(const Camera &camera, const SceneSettings &settings);
    void setTerrainGrid(TerrainGrid grid);
    void setTerrainSettings(const TerrainSettings &);
    const TerrainStreamStats &terrainStreamingStats() const;
    std::vector<glm::vec2> readStreamedHeights(ChunkCoord);
    std::vector<glm::vec2> readStreamedTileHeights(TerrainTileKey, uint32_t geometryLevel = 0);
    std::vector<glm::vec4> readStreamedTileNormals(TerrainTileKey, uint32_t normalLevel);
    std::vector<TerrainTileSnapshot> readTerrainResidency() const;
    TerrainDetailJobSnapshot readTerrainDetailJob() const;
    std::vector<glm::vec4> readTerrainVertexPositions();
    std::vector<glm::vec4> readStreamedNormals(ChunkCoord, uint32_t level = 0);
    // Legacy dense indices, or concatenated shared quadrant masks when LOD is
    // configured.
    std::vector<uint32_t> readTerrainIndices(); // Diagnostics.
    void requestTerrain(const TerrainRequest &request);
    void requestTerrainBatch(const TerrainBatchRequest &request);
    uint32_t terrainChunkCount() const;
    uint64_t terrainAllocatedBytes() const;
    const TerrainRenderStats &terrainRenderStats() const;
    std::vector<TerrainPatchDraw> readTerrainPatches() const; // Owned copy of the last selection.
    uint64_t terrainGenerationCount() const;
    std::vector<TerrainSample> readTerrainSnapshot(uint32_t slot = 0); // Blocking diagnostics.
    std::vector<glm::vec4> readTerrainNormals(uint32_t slot = 0, uint32_t level = 0);
    // Blocking diagnostics; reuses scratch and invalidates slot zero.
    // Non-streamed, non-LOD only.
    std::vector<TerrainGeographySample> sampleTerrainGeography(TerrainGrid, const TerrainRequest &,
                                                               bool canonical = true);
    // Actual filtered generation kernel; optional owned analytic ridge fixture.
    std::vector<glm::vec2> sampleTerrainHeightLevel(TerrainGrid, const TerrainRequest &, uint32_t level,
                                                    bool ridgeFixture = false);
    // Owned analytic surface through the production hierarchical normal kernel.
    // Blocking diagnostic.
    std::vector<TerrainSample> sampleTerrainNormalFixture(TerrainGrid, uint32_t level,
                                                          uint32_t tileLevel = 0);
    std::vector<TerrainSample> normalsForFixture(const std::vector<glm::vec2> &heights);
    // Blocking numerical probe; non-streamed/non-LOD scratch only.
    std::vector<TerrainSample> sampleCurvatureFixture(const std::vector<TerrainSample> &,
                                                     glm::vec4 curvature, bool reducedTrigPrecision);
    // Diagnostic observer override; subsequent render must use freezeLod=true.
    void setTerrainLodObserver(const Camera &);
    TerrainLodReviewState terrainLodReviewState() const;
    void setTerrainLodReviewState(const TerrainLodReviewState &);
    void capture(const std::filesystem::path &path); // Offscreen PPM capture.
    void waitIdle();
    const std::string &deviceName() const;
    bool validationEnabled() const;
    uint32_t validationErrors() const;
    uint32_t width() const;
    uint32_t height() const;
    uint32_t renderWidth() const;
    uint32_t renderHeight() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ow

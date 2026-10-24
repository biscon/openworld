#include "review_view.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
namespace nlohmann {
template <> struct adl_serializer<std::filesystem::path> {
    static void to_json(json &j, const std::filesystem::path &p) { j = p.string(); }
    static void from_json(const json &j, std::filesystem::path &p) { p = j.get<std::string>(); }
};
} // namespace nlohmann
namespace glm {
template <length_t N, class T, qualifier Q> void to_json(nlohmann::json &j, const vec<N, T, Q> &v) {
    j = nlohmann::json::array();
    for (length_t i = 0; i < N; ++i)
        j.push_back(v[i]);
}
template <length_t N, class T, qualifier Q> void from_json(const nlohmann::json &j, vec<N, T, Q> &v) {
    if (!j.is_array() || j.size() != N)
        throw std::invalid_argument("Invalid review vector");
    for (length_t i = 0; i < N; ++i)
        v[i] = j.at(i).get<T>();
}
} // namespace glm
namespace ow {
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChunkCoord, x, z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Camera, chunk, local, yaw, pitch, speed, renderFar, curvature)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TerrainGrid, cells, spacing)
void to_json(json &j, const TerrainLodConfig &v) {
    j = {{"enabled", v.enabled},
         {"patchCells", v.patchCells},
         {"firstRange", v.firstRange},
         {"triangleBudget", v.triangleBudget}};
}
void from_json(const json &j, TerrainLodConfig &v) {
    j.at("enabled").get_to(v.enabled);
    j.at("patchCells").get_to(v.patchCells);
    j.at("firstRange").get_to(v.firstRange);
    v.triangleBudget = j.value("triangleBudget", 2097152ull);
}
void to_json(json &j, const TerrainPatchKey &v) { j = json::array({v.x, v.z, v.level}); }
void from_json(const json &j, TerrainPatchKey &v) {
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("Invalid review patch key");
    j.at(0).get_to(v.x);
    j.at(1).get_to(v.z);
    j.at(2).get_to(v.level);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TerrainSettings, seed, wavelength, heightScale, erosionStrength,
                                   creaseRounding, regionWavelength, erosionWavelength, ridgeStrength,
                                   baseDetail)
void to_json(json &j, const TerrainLodReviewState &v) {
    j = {{"observer", v.observer},
         {"qualityScale", v.qualityScale},
         {"focalPixels", v.focalPixels},
         {"initialized", v.initialized},
         {"patches", v.patches}};
}
void from_json(const json &j, TerrainLodReviewState &v) {
    j.at("observer").get_to(v.observer);
    j.at("qualityScale").get_to(v.qualityScale);
    j.at("focalPixels").get_to(v.focalPixels);
    j.at("initialized").get_to(v.initialized);
    if (j.contains("patches"))
        j.at("patches").get_to(v.patches);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TerrainStreamingConfig, enabled, hierarchical, unfilteredHeightReference,
                                   shadowCasterBytes, horizonBytes, horizonMaximumDistance, maximumDistance,
                                   transferBytesPerFrame, gpuBytes, hostBytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ShadowCapacity, resolution, casters, bytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MaterialSettings, baseColour, roughness, metallic, ao, emissive)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LightingSettings, sunEnabled, environmentEnabled, sunDirection, sunColour,
                                   sunLux, environmentNits, environmentRotation)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OutputSettings, ev100, compensation, toneMapper)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TerrainMaterialSettings, enabled, weights, isolate, filtering,
                                   repeatMetres, normalStrength, roughness, tint, heightBlend, cliffs, macro,
                                   twoAxis, heightInfluence, blendWidth, macroRepeatMetres, macroStrength,
                                   diagnostic)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AtmosphereSettings, mode, quality, aerialPerspective, multipleScattering,
                                   datum, planetRadius, atmosphereHeight, rayleigh, rayleighHeight,
                                   mieScattering, mieAbsorption, mieHeight, mieG, ozone, ozoneCentre,
                                   ozoneWidth, aerosolDensity, groundAlbedo, sunRadius, environmentMultiplier,
                                   boundaryHaze, diagnostic)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GtaoSettings, mode, radius, diagnostic)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ShadowSettings, mode, nearDistance, farDistance, biasTexels,
                                   normalBiasTexels, diagnostic)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TaaSettings, enabled, historyWeight)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BloomSettings, intensity, threshold, knee, radius)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SceneSettings, shading, atmosphere, taa, bloom, gtao, shadows,
                                   terrainVisible, localLights, clusterDiagnostic, material, terrainMaterial,
                                   lighting, output, viewRadius, lod, lodColours, freezeLod, visibility,
                                   opaqueDepthPrepass, terrainLodQuality, terrainLodErrorView,
                                   terrainLodPixelError, frustumCulling, eroded, normalView, wireframe,
                                   chunkBorders, filteredNormals, background)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RendererOptions, materialDirectory, terrainGrid, normalSubdivisions,
                                   terrainCapacity, terrainChunkCapacity, lod, streaming, curvedHorizon,
                                   shadowCapacity, width, height, renderHeight, forceFixedMaterials,
                                   forceRgbaMaterials, forceTrilinear, materialBudgetBytes,
                                   forceDirectShadows, forceManualShadowFiltering, forceNoShadowDepth,
                                   visibilityCapacity, atmosphereBudgetBytes, forceNoAtmosphere,
                                   taaBudgetBytes, bloomBudgetBytes, forceNoTaa, forceNoBloom,
                                   gtaoBudgetBytes, forceNoGtao, forceWideGtao, forceFixedIndirect,
                                   forceSingleIndirect, forceDirectVisibility, forceNoHiZ)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ReviewView, version, camera, terrain, terrainOrigin, scene, lod, startup)
namespace {
void check(bool valid, const char *message) {
    if (!valid)
        throw std::invalid_argument(message);
}
void finite(const json &j) {
    if (j.is_number_float())
        check(std::isfinite(j.get<double>()), "Nonfinite review value");
    if (j.is_null())
        throw std::invalid_argument("Missing/nonfinite review value");
    if (j.is_structured())
        for (const auto &v : j)
            finite(v);
}
void cameraValid(const Camera &c) {
    check(std::abs(double(c.chunk.x)) < 1e12 && std::abs(double(c.chunk.z)) < 1e12,
          "Review camera chunk exceeds supported diagnostic range");
    check(glm::all(glm::lessThan(glm::abs(c.local), glm::dvec3(1e9))) && std::abs(c.pitch) <= 1.571 &&
              std::abs(c.yaw) < 1e7 && c.speed >= 0 && c.speed <= 1e6 && c.renderFar > Camera::nearPlane &&
              c.renderFar <= 1e8 && (c.curvature.z == 0 || (c.curvature.z == 1 && c.curvature.x >= 1e6f)),
          "Invalid review camera");
}
} // namespace
void validateReviewView(const ReviewView &v) {
    check(v.version == 1 || v.version == 2, "Unsupported review view version");
    finite(json(v));
    cameraValid(v.camera);
    cameraValid(v.lod.observer);
    const auto &o = v.startup;
    const auto &s = v.scene;
    validateGrid(o.terrainGrid);
    if (o.lod.enabled)
        validateLodGrid(o.terrainGrid, o.lod.patchCells);
    check(o.terrainCapacity >= o.terrainGrid.cells && o.terrainCapacity <= 1024 &&
              o.terrainChunkCapacity > 0 && o.terrainChunkCapacity <= maxTerrainChunks &&
              o.normalSubdivisions > 0 && o.normalSubdivisions <= 2 && o.lod.firstRange > 0 &&
              o.lod.triangleBudget >= uint64_t(o.lod.patchCells) * o.lod.patchCells * 8 &&
              o.lod.triangleBudget <= 33554432,
          "Invalid review terrain startup");
    validateBatch(o.terrainGrid, terrainNeighbourhood({v.terrainOrigin, v.terrain}, true),
                  o.terrainChunkCapacity);
    check(o.width > 0 && o.width <= 16384 && o.height > 0 && o.height <= 16384 && o.renderHeight <= 16384,
          "Invalid review render extent");
    check(o.streaming.maximumDistance > 0 && o.streaming.maximumDistance <= 16384 &&
              o.streaming.horizonMaximumDistance > 0 && o.streaming.horizonMaximumDistance <= 1048576 &&
              o.streaming.gpuBytes <= (4ull << 30) && o.streaming.hostBytes <= (4ull << 30) &&
              o.streaming.horizonBytes <= (1ull << 30) && o.streaming.shadowCasterBytes <= (1ull << 30) &&
              o.shadowCapacity.resolution >= 16 && o.shadowCapacity.resolution <= 8192 &&
              o.shadowCapacity.casters > 0 && o.shadowCapacity.casters <= 65536 && o.visibilityCapacity > 0 &&
              o.visibilityCapacity <= 1048576,
          "Invalid review resource limits");
    check(v.lod.qualityScale >= 1 && v.lod.qualityScale <= terrainLodMaximumScale && v.lod.focalPixels >= 0 &&
              v.lod.focalPixels <= 32768,
          "Invalid frozen LOD state");
    check(v.lod.patches.size() <= o.visibilityCapacity, "Review partition exceeds draw capacity");
    std::set<std::tuple<int64_t, int64_t, uint32_t>> keys;
    for (auto k : v.lod.patches) {
        check(k.level < 32 && std::abs(double(k.x)) < 1e15 && std::abs(double(k.z)) < 1e15,
              "Invalid review patch coordinates");
        const int64_t side = int64_t(o.lod.patchCells / 2) << k.level;
        check(side > 0 && k.x % side == 0 && k.z % side == 0, "Unaligned review patch");
        check(keys.emplace(k.x, k.z, k.level).second, "Duplicate review patch");
    }
    for (auto k : v.lod.patches)
        for (uint32_t l = k.level + 1; l < 32; ++l) {
            const int64_t side = int64_t(o.lod.patchCells / 2) << l;
            check(
                !keys.count({terrainFloorDivide(k.x, side) * side, terrainFloorDivide(k.z, side) * side, l}),
                "Overlapping review patches");
        }
    validateAtmosphere(s.atmosphere);
    validateGtao(s.gtao);
    validatePost(s.taa, s.bloom);
    validate(s.shadows);
    check(uint32_t(s.shading) <= 1 && uint32_t(s.visibility) <= 2 && uint32_t(s.localLights) <= 2 &&
              uint32_t(s.output.toneMapper) <= 1 && s.clusterDiagnostic <= 2 && s.viewRadius >= 0 &&
              s.viewRadius <= terrainMaximumRadius(o.terrainGrid) && s.terrainLodPixelError >= .5f &&
              s.terrainLodPixelError <= 8,
          "Invalid review scene settings");
    const auto &m = s.terrainMaterial;
    check(m.repeatMetres > 0 && m.macroRepeatMetres > 0 && m.blendWidth > 0 && m.isolate >= -1 &&
              m.isolate <= 3 && m.filtering >= 0 && m.filtering <= 2 && m.diagnostic >= 0 &&
              m.diagnostic <= 4,
          "Invalid review material settings");
    check(glm::length(s.lighting.sunDirection) > 1e-6f && s.lighting.sunLux >= 0 &&
              s.lighting.environmentNits >= 0,
          "Invalid review lighting");
}
ReviewView loadReviewView(const std::filesystem::path &path) {
    check(std::filesystem::file_size(path) <= 1024 * 1024, "Review file exceeds 1 MiB");
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open review view: " + path.string());
    auto j = json::parse(f);
    finite(j);
    auto v = j.get<ReviewView>();
    validateReviewView(v);
    return v;
}
void saveReviewView(const std::filesystem::path &path, const ReviewView &v) {
    validateReviewView(v);
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        std::ofstream f(temp, std::ios::trunc);
        f.exceptions(std::ios::failbit | std::ios::badbit);
        auto document = json(v);
        document["version"] = 2; // Loading v1 remains supported; every new save uses the current format.
        const auto bytes = document.dump(2);
        check(bytes.size() + 1 <= 1024 * 1024, "Review file exceeds 1 MiB");
        f << bytes << '\n';
        f.close();
        std::filesystem::rename(temp, path);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw;
    }
}
void applyReviewStartup(RendererOptions &o, const ReviewView &v) {
    // from_json changes only named persisted fields; process-local settings
    // survive.
    from_json(json(v.startup), o);
}
bool compatibleReviewStartup(const RendererOptions &o, const ReviewView &v) {
    auto current = json(o), saved = json(v.startup);
    // Extent and render height can be changed live; all other fields are
    // startup-only.
    for (const char *key : {"width", "height", "renderHeight"}) {
        current.erase(key);
        saved.erase(key);
    }
    return current == saved;
}
} // namespace ow

#include "scene.hpp"
#include <cmath>
#include <limits>
namespace ow {
namespace {
template <class V> bool finite(V v) {
    for (glm::length_t i = 0; i < v.length(); ++i)
        if (!std::isfinite(v[i]))
            return false;
    return true;
}
void require(bool b, const char *s) {
    if (!b)
        throw std::invalid_argument(s);
}
} // namespace
void validate(const WorldPosition &p) {
    require(p.x >= -1000000000000ll && p.x <= 1000000000000ll && p.z >= -1000000000000ll &&
                p.z <= 1000000000000ll && finite(p.local) &&
                glm::all(glm::lessThanEqual(glm::abs(p.local), glm::dvec3(1e15))),
            "Invalid world position");
}
WorldPosition translated(WorldPosition p, glm::dvec3 delta) {
    validate(p);
    require(finite(delta), "Invalid world translation");
    p.local += delta;
    for (int axis : {0, 2}) {
        const double c = std::floor(p.local[axis] / 1024.);
        require(std::abs(c) <= 2e12, "World translation too large");
        auto &cell = axis == 0 ? p.x : p.z;
        cell += int64_t(c);
        p.local[axis] -= c * 1024.;
    }
    validate(p);
    return p;
}
WorldPosition worldPosition(const Camera &c, const TerrainGrid &g) {
    WorldPosition p;
    p.local = c.local;
    for (int axis : {0, 2}) {
        const long double v = static_cast<long double>(axis == 0 ? c.chunk.x : c.chunk.z) * g.extent();
        const auto cell = std::floor(v / 1024.L);
        require(std::abs(cell) <= 1e12L, "Camera out of scene range");
        (axis == 0 ? p.x : p.z) = int64_t(cell);
        p.local[axis] += double(v - cell * 1024.L);
    }
    return translated(p, {});
}
glm::dvec3 relativePosition(const WorldPosition &p, const WorldPosition &origin) {
    return glm::dvec3(double(p.x - origin.x) * 1024., 0, double(p.z - origin.z) * 1024.) + p.local -
           origin.local;
}
void validate(const LocalLight &l) {
    validate(l.position);
    require(std::isfinite(l.volumetricScattering) && l.volumetricScattering >= 0 &&
                l.volumetricScattering <= 10,
            "Invalid volumetric light multiplier");
    require(finite(l.colour) && glm::all(glm::greaterThanEqual(l.colour, glm::vec3(0))) &&
                glm::all(glm::lessThanEqual(l.colour, glm::vec3(1))),
            "Light colour must be finite and in [0,1]");
    require(std::isfinite(l.intensity) && l.intensity >= 0 && l.intensity <= 1e9f && std::isfinite(l.range) &&
                l.range > 0 && l.range <= 128000 && std::isfinite(l.sourceRadius) &&
                l.sourceRadius >= .001f && l.sourceRadius <= l.range,
            "Invalid light intensity/range/radius");
    require(l.type == LightType::Point || l.type == LightType::Spot, "Invalid light type");
    require(finite(l.direction) && std::isfinite(glm::dot(l.direction, l.direction)) &&
                glm::dot(l.direction, l.direction) > 1e-12f && std::isfinite(l.innerAngle) &&
                std::isfinite(l.outerAngle) && l.innerAngle >= 0 && l.outerAngle > l.innerAngle &&
                l.outerAngle < 1.5707963f,
            "Invalid spot cone");
}
float lightAttenuation(const LocalLight &l, glm::vec3 d) {
    const float d2 = glm::dot(d, d), q = d2 / (l.range * l.range), a = std::max(0.f, 1 - q * q);
    float cone = 1;
    if (l.type == LightType::Spot && d2 > 1e-12f)
        cone =
            std::clamp((glm::dot(d / std::sqrt(d2), glm::normalize(l.direction)) - std::cos(l.outerAngle)) /
                           (std::cos(l.innerAngle) - std::cos(l.outerAngle)),
                       0.f, 1.f);
    return a * a * cone * cone / std::max(d2, l.sourceRadius * l.sourceRadius);
}
void validate(const MeshMaterial &m) {
    require(finite(m.baseColour) && glm::all(glm::greaterThanEqual(m.baseColour, glm::vec4(0))) &&
                glm::all(glm::lessThanEqual(m.baseColour, glm::vec4(1))) && finite(m.emissive) &&
                glm::all(glm::greaterThanEqual(m.emissive, glm::vec3(0))) &&
                glm::all(glm::lessThanEqual(m.emissive, glm::vec3(1e9))),
            "Invalid mesh colour");
    require(std::isfinite(m.roughness) && m.roughness >= 0 && m.roughness <= 1 && std::isfinite(m.metallic) &&
                m.metallic >= 0 && m.metallic <= 1 && std::isfinite(m.ao) && m.ao >= 0 && m.ao <= 1 &&
                std::isfinite(m.normalStrength) && m.normalStrength >= 0 && m.normalStrength <= 10 &&
                std::isfinite(m.cutoff) && m.cutoff >= 0 && m.cutoff <= 1 && uint32_t(m.alpha) <= 2,
            "Invalid mesh material");
}
void validate(const MeshInstance &i) {
    validate(i.position);
    require(finite(i.scale) && std::abs(i.scale.x) >= 1e-6 && std::abs(i.scale.y) >= 1e-6 &&
                std::abs(i.scale.z) >= 1e-6 &&
                glm::all(glm::lessThanEqual(glm::abs(i.scale), glm::vec3(1e6))),
            "Singular or invalid instance scale");
    const glm::vec4 q(i.rotation.x, i.rotation.y, i.rotation.z, i.rotation.w);
    require(finite(q) && std::isfinite(glm::dot(q, q)) && glm::dot(q, q) > 1e-12,
            "Invalid instance rotation");
}
ClusterGrid clusterGrid(uint32_t w, uint32_t h, const SceneCapacity &c) {
    require(w && h && w <= 16384 && h <= 16384 && c.clusterTilePixels >= 8 && c.clusterTilePixels <= 16384 &&
                c.clusterSlices && c.clusterSlices <= 128 && c.clusterIndices && c.clusterIndices <= 4096,
            "Invalid cluster configuration");
    ClusterGrid g;
    g.tile = c.clusterTilePixels;
    g.z = c.clusterSlices;
    g.indices = c.clusterIndices;
    do {
        g.x = (w + g.tile - 1) / g.tile;
        g.y = (h + g.tile - 1) / g.tile;
        if (g.bytes() + uint64_t(c.lights) * 64 + 4096 <= c.clusterBytesPerFrame)
            return g;
        require(g.tile < 32768, "Cluster budget too small");
        g.tile *= 2;
    } while (true);
}
uint32_t depthSlice(float d, uint32_t slices) {
    return uint32_t(std::clamp(std::floor(std::log(std::max(d, Camera::nearPlane) / Camera::nearPlane) /
                                          std::log(Camera::farPlane / Camera::nearPlane) * slices),
                               0.f, float(slices - 1)));
}
} // namespace ow

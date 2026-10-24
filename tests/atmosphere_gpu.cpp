#include "render_fixtures.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
RendererOptions options() {
    RendererOptions o;
    o.shaderDirectory = OPENWORLD_SHADER_DIR;
    o.requireValidation = true;
    o.ui = false;
    o.width = 65;
    o.height = 49;
    o.terrainGrid = {8, .5f};
    o.terrainCapacity = 8;
    o.visibilityCapacity = 128;
    return o;
}
void frame(Renderer &r, const Camera &c, const SceneSettings &s) {
    require(r.render(c, s), "Atmosphere frame submitted");
    require(r.validationErrors() == 0, "Atmosphere Vulkan validation");
}
void finite(const std::vector<glm::vec4> &v) {
    for (auto p : v)
        for (int k = 0; k < 4; ++k)
            require(std::isfinite(p[k]) && p[k] >= 0, "Finite atmosphere product");
}
void referenceChecks(Renderer &r, Camera c, SceneSettings s) {
    s.taa.enabled = false;
    s.bloom.intensity = 0;
    s.atmosphere.multipleScattering = false;
    s.atmosphere.groundAlbedo = 0;
    c.local.y = 100;
    frame(r, c, s);
    auto data = r.readAtmosphereSnapshot();
    const double pi = 3.141592653589793, radius = s.atmosphere.planetRadius,
                 top = radius + s.atmosphere.atmosphereHeight;
    double maxTransmissionError = 0, maxRadianceError = 0;
    for (auto xy : {glm::uvec2(30, 4), glm::uvec2(110, 12), glm::uvec2(200, 32)}) {
        double H = std::sqrt((top - radius) * (top + radius)), rho = double(xy.y) / 63 * H;
        double r0 = std::sqrt(radius * radius + rho * rho),
               d = (top - r0) + (rho + H - (top - r0)) * double(xy.x) / 255;
        double mu = ((top - r0) * (top + r0) - d * d) / (2 * r0 * d);
        auto reference =
            atmosphereReference(s.atmosphere, r0 - radius, {std::sqrt(1 - mu * mu), mu, 0},
                                glm::dvec3(s.lighting.sunDirection), d, 2048, 1);
        auto actual = glm::dvec3(data.transmittance[xy.y * 256 + xy.x]);
        maxTransmissionError =
            std::max(maxTransmissionError, glm::length(actual - reference.transmittance));
    }
    double horizon = -std::acos(radius / (radius + 100));
    for (auto xy : {glm::uvec2(20, 75), glm::uvec2(100, 95), glm::uvec2(150, 65)}) {
        double v = double(xy.y) / (data.skyExtent.y - 1) * 2 - 1;
        double e = horizon + v * v * (pi * .5 - horizon),
               a = (xy.x + .5) / data.skyExtent.x * 2 * pi;
        glm::dvec3 ray(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a));
        auto reference = atmosphereReference(s.atmosphere, 100, ray,
                                             glm::dvec3(s.lighting.sunDirection), 1e7, 2048, 256);
        auto actual = glm::dvec3(data.sky[xy.y * data.skyExtent.x + xy.x]);
        maxRadianceError =
            std::max(maxRadianceError, glm::length(actual - reference.scattering) /
                                           std::max(.0001, glm::length(reference.scattering)));
    }
    std::cout << "Reference transmission absolute error " << maxTransmissionError
              << " radiance relative error " << maxRadianceError << '\n';
    require(maxTransmissionError < .01, "LUT transmittance matches independent integration");
    require(maxRadianceError < .08, "Sky single scattering matches independent integration");
    for (auto xy : {glm::uvec2(20, 0), glm::uvec2(28, 2)}) {
        auto reference = atmosphereMultipleReference(
            s.atmosphere, double(xy.y) / 31 * s.atmosphere.atmosphereHeight,
            double(xy.x) / 31 * 2 - 1);
        auto actual = glm::dvec3(data.multipleScattering[xy.y * 32 + xy.x]);
        auto error = glm::length(actual - reference) / std::max(.00001, glm::length(reference));
        std::cout << "Multiple scattering closure relative error " << error << '\n';
        require(error < .15, "Multiple-scattering LUT matches higher-sample quadrature");
    }
    {
        auto mixed = s;
        mixed.atmosphere.boundaryHaze = .5f;
        mixed.viewRadius = 1000;
        Camera camera;
        camera.local.y = 2000;
        frame(r, camera, mixed);
        auto volume = r.readAtmosphereSnapshot();
        const uint32_t n = volume.volumeSize, x = n / 2, y = n / 2, z = 10;
        const double tangent = std::tan(pi / 6), nx = (x + .5) / n * 2 - 1,
                     ny = (y + .5) / n * 2 - 1;
        auto ray = glm::normalize(glm::dvec3(nx * tangent * 65 / 49, -ny * tangent, -1));
        double distance = std::pow(double(z) / (n - 1), 2) * Camera::farPlane / (-ray.z);
        auto reference =
            atmosphereReference(mixed.atmosphere, 2000, ray,
                                glm::dvec3(mixed.lighting.sunDirection), distance, 2048, 256, 4000);
        uint32_t at = (z * n + y) * n + x;
        near(glm::length(glm::dvec3(volume.aerialTransmittance[at]) - reference.transmittance), 0,
             .015, "Mixed haze transmission reference");
        near(glm::length(glm::dvec3(volume.aerialScattering[at]) - reference.scattering), 0, .001,
             "Mixed haze source reference");
        frame(r, c, s);
    }
    // Full transport must agree between opaque and alpha-one transparent
    // surfaces.
    c.pitch = 0;
    c.yaw = 0;
    c.chunk = {};
    s.atmosphere.multipleScattering = false;
    c.local.y = 2000;
    auto mesh = r.uploadMesh(makeQuad());
    MeshMaterial m;
    m.unlit = true;
    m.doubleSided = true;
    m.baseColour = {.3, .2, .1, 1};
    m.emissive = {1000, 2000, 3000};
    auto opaque = r.createMaterial(m);
    m.alpha = AlphaMode::Blend;
    auto solidBlend = r.createMaterial(m);
    m.baseColour.a = .5f;
    auto halfBlend = r.createMaterial(m);
    for (double distance : {1000., 8000.}) {
        frame(r, c, s);
        auto bg = r.readHdrSnapshot().pixels[24 * 65 + 32];
        MeshInstance instance;
        instance.mesh = mesh;
        instance.material = opaque;
        instance.position.local = {0, 2000, -distance};
        instance.scale = glm::vec3(float(distance));
        auto handle = r.createInstance(instance);
        frame(r, c, s);
        auto fogged = r.readHdrSnapshot().pixels[24 * 65 + 32];
        const auto reference =
            atmosphereReference(s.atmosphere, 2000, {0, 0, -1}, glm::dvec3(s.lighting.sunDirection),
                                distance, 2048, 256);
        const auto emitted = glm::dvec3(m.emissive) + glm::dvec3(m.baseColour);
        const auto expected =
            (reference.transmittance * emitted +
             reference.scattering * glm::dvec3(s.lighting.sunColour) * double(s.lighting.sunLux)) *
            double(exposure(s.output));
        require(glm::length(glm::dvec3(fogged) - expected) < .02 * glm::length(expected) + .0001,
                "Opaque composition agrees with independent ray integration");
        if (distance == 1000) {
            auto vacuum = s;
            vacuum.atmosphere.rayleigh = vacuum.atmosphere.ozone = glm::vec3(0);
            vacuum.atmosphere.mieScattering = vacuum.atmosphere.mieAbsorption = 0;
            frame(r, c, vacuum);
            auto unchanged = glm::dvec3(r.readHdrSnapshot().pixels[24 * 65 + 32]);
            near(glm::length(unchanged - emitted * double(exposure(s.output))), 0, .0001,
                 "GPU vacuum preserves pre-exposed surface radiance");
            frame(r, c, s);
        }
        instance.material = solidBlend;
        r.updateInstance(handle, instance);
        frame(r, c, s);
        auto transparent = r.readHdrSnapshot().pixels[24 * 65 + 32];
        near(glm::length(glm::vec3(transparent - fogged)), 0, .001,
             "Opaque and transparent own-depth transport agree");
        instance.material = halfBlend;
        r.updateInstance(handle, instance);
        frame(r, c, s);
        auto half = r.readHdrSnapshot().pixels[24 * 65 + 32];
        near(glm::length(glm::vec3(half - (fogged + bg) * .5f)), 0, .002,
             "Atmosphere composes once before premultiplied blending");
        r.removeInstance(handle);
    }
    r.releaseMaterial(opaque);
    r.releaseMaterial(solidBlend);
    r.releaseMaterial(halfBlend);
    r.releaseMesh(mesh);
}
void extendedHorizonChecks() {
    auto o = options();
    o.curvedHorizon = true;
    o.shadowCapacity.resolution = 128;
    o.shadowCapacity.casters = 128;
    Renderer r(o);
    Camera c;
    c.local = {0, 2000, 0};
    SceneSettings s;
    s.terrainVisible = false;
    s.atmosphere.mode = AtmosphereMode::Physical;
    s.atmosphere.multipleScattering = false;
    s.atmosphere.groundAlbedo = 0;
    s.lighting.sunDirection = glm::normalize(glm::vec3(-.6, .8, .35));
    frame(r, c, s);
    auto a = r.readAtmosphereSnapshot();
    require(a.volumeSize == 32 && a.volumeDepth == 64,
            "Normal distant volume preserves 32 near samples and adds 32 far samples");
    Camera projection = c;
    projection.renderFar = 1572864;
    const auto inverse = glm::inverse(projection.viewProjection(float(o.width) / o.height));
    const uint32_t x = 16, y = 15, n = a.volumeSize;
    auto point = inverse * glm::vec4((x + .5f) / n * 2 - 1, (y + .5f) / n * 2 - 1, 0, 1);
    auto ray = glm::normalize(glm::dvec3(point) / double(point.w));
    double maxT = 0, maxS = 0;
    for (uint32_t z : {1u, 15u, 30u, 31u, 32u, 40u, 50u, 63u}) {
        double depth = z <= 31 ? std::pow(double(z) / 31, 2) * 64000
                               : 64000 * std::pow(1572864. / 64000, double(z - 31) / 32);
        auto ref = atmosphereReference(s.atmosphere, 2000, ray, glm::dvec3(s.lighting.sunDirection),
                                       depth / -ray.z, 4096, 256);
        const auto index = (z * n + y) * n + x;
        maxT = std::max(maxT,
                        glm::length(glm::dvec3(a.aerialTransmittance[index]) - ref.transmittance));
        maxS = std::max(maxS, glm::length(glm::dvec3(a.aerialScattering[index]) - ref.scattering) /
                                  std::max(.0001, glm::length(ref.scattering)));
    }
    std::cout << "Extended aerial reference errors T " << maxT << " S " << maxS << '\n';
    require(maxT < .015 && maxS < .08,
            "Near and long-range transport match independent integration");
    auto mesh = r.uploadMesh(makeQuad());
    MeshMaterial material;
    material.unlit = true;
    material.doubleSided = true;
    material.baseColour = {1, 1, 1, 1};
    auto m = r.createMaterial(material);
    MeshInstance instance;
    instance.mesh = mesh;
    instance.material = m;
    instance.position.local = {0, 2000, -100000};
    instance.scale = {10000, 10000, 1};
    auto handle = r.createInstance(instance);
    s.atmosphere.mode = AtmosphereMode::Off;
    s.taa.enabled = true;
    for (auto mode : {VisibilityMode::Direct, VisibilityMode::Indirect, VisibilityMode::HiZ}) {
        s.visibility = mode;
        s.opaqueDepthPrepass = true;
        for (int i = 0; i < 5; ++i)
            frame(r, c, s);
        auto post = r.readPostSnapshot();
        const auto index = (post.height / 2) * post.width + post.width / 2;
        near(glm::length(glm::vec2(post.motion[index])), 0, 1e-5,
             "Stationary curved triangles have no phantom motion");
        require(post.motion[index].z > 90000 && post.motion[index].z < 110000,
                "Public motion snapshots retain metres");
        require(post.depth[index] > 90000 && post.depth[index] < 110000,
                "Curved far mesh retains finite, accepted temporal depth beyond half-float range");
    }
    c.local.x += 10;
    c.update({}, 0, o.terrainGrid);
    frame(r, c, s);
    require(r.postStats().historyValid, "Curved camera rebase retains temporal history");
    s.atmosphere.mode = AtmosphereMode::Physical;
    s.taa.enabled = false;
    frame(r, c, s);
    auto opaque = r.readHdrSnapshot();
    material.alpha = AlphaMode::Blend;
    auto transparent = r.createMaterial(material);
    instance.material = transparent;
    r.updateInstance(handle, instance);
    frame(r, c, s);
    auto blend = r.readHdrSnapshot();
    const auto centre = (o.height / 2) * o.width + o.width / 2;
    near(glm::length(glm::vec3(opaque.pixels[centre] - blend.pixels[centre])), 0, .001,
         "Opaque and own-depth transparent transport agree at 100 km");
    s.gtao.mode = GtaoMode::Half;
    s.shadows.mode = ShadowMode::CSM;
    frame(r, c, s);
    s.atmosphere.quality = AtmosphereQuality::High;
    frame(r, c, s);
    a = r.readAtmosphereSnapshot();
    require(a.volumeSize == 64 && a.volumeDepth == 96, "High long-range volume dimensions");
    r.removeInstance(handle);
    r.releaseMaterial(transparent);
    r.releaseMaterial(m);
    r.releaseMesh(mesh);
    r.waitIdle();
    require(r.validationErrors() == 0, "Extended atmosphere and curved mesh validation");
}
int main() {
    try {
        extendedHorizonChecks();
        auto o = options();
        Renderer r(o);
        Camera c;
        c.local.y = 100;
        c.pitch = .1f;
        SceneSettings s;
        s.terrainVisible = false;
        s.shadows.mode = ShadowMode::Off;
        s.atmosphere.mode = AtmosphereMode::Physical;
        frame(r, c, s);
        require(r.atmosphereStats().effective == AtmosphereMode::Physical,
                "Physical atmosphere supported");
        auto snap = r.readAtmosphereSnapshot();
        finite(snap.transmittance);
        finite(snap.multipleScattering);
        finite(snap.sky);
        finite(snap.aerialScattering);
        finite(snap.aerialTransmittance);
        for (auto p : snap.transmittance)
            require(p.x <= 1 && p.y <= 1 && p.z <= 1, "Bounded transmission");
        for (uint32_t i = 0; i < snap.volumeSize * snap.volumeSize; ++i)
            near(glm::length(glm::vec3(snap.aerialTransmittance[i]) - glm::vec3(1)), 0, 1e-5,
                 "Aerial zero endpoint");
        referenceChecks(r, c, s);
        frame(r, c, s);
        auto hdr = r.readHdrSnapshot();
        finite(hdr.pixels);
        double energy = 0;
        for (auto p : hdr.pixels)
            energy += p.r + p.g + p.b;
        require(energy > 0, "Sky draws behind empty depth");
        frame(r, c, s);
        frame(r, c, s);
        auto updates = r.atmosphereStats().profileUpdates;
        frame(r, c, s);
        require(r.atmosphereStats().profileUpdates == updates, "Static atmosphere reuses profiles");
        s.taa.enabled = true;
        s.bloom.intensity = .1f;
        for (int i = 0; i < 4; ++i)
            frame(r, c, s);
        auto post = r.readPostSnapshot();
        finite(post.resolved);
        require(post.motion[post.motion.size() / 2].w == 2, "Sky rotation motion classification");
        c.yaw += .01f;
        frame(r, c, s);
        require(r.postStats().historyValid, "Small sky rotation preserves history");
        c.chunk = {1000000, -1000000};
        frame(r, c, s);
        finite(r.readHdrSnapshot().pixels);
        s.output.ev100 = 14;
        frame(r, c, s);
        require(r.atmosphereStats().profileUpdates == updates,
                "Exposure does not rebuild profiles");
        s.atmosphere.aerosolDensity = 2;
        frame(r, c, s);
        require(!r.postStats().historyValid, "Profile changes reset history");
        frame(r, c, s);
        frame(r, c, s);
        auto opticalUpdates = r.atmosphereStats().transmittanceUpdates;
        auto multipleUpdates = r.atmosphereStats().multipleScatteringUpdates;
        s.atmosphere.mieG = .7f;
        frame(r, c, s);
        require(r.atmosphereStats().transmittanceUpdates == opticalUpdates &&
                    r.atmosphereStats().multipleScatteringUpdates == multipleUpdates,
                "Phase changes only rebuild dependent sky and lighting products");
        s.atmosphere.quality = AtmosphereQuality::High;
        s.atmosphere.boundaryHaze = .5f;
        c.local.y = 2000;
        s.lighting.sunDirection = {.5f, .02f, -1};
        frame(r, c, s);
        require(r.readAtmosphereSnapshot().volumeSize == 64, "High-quality volume allocation");
        require(r.atmosphereStats().bytes <= o.atmosphereBudgetBytes, "High quality fits budget");
        s.shading = ShadingMode::Legacy;
        frame(r, c, s);
        require(r.atmosphereStats().fallback == AtmosphereFallback::Diagnostic,
                "Diagnostic bypass");
        s.shading = ShadingMode::Pbr;
        s.atmosphere.quality = AtmosphereQuality::Normal;
        s.atmosphere.aerialPerspective = false;
        frame(r, c, s);
        require(r.validationErrors() == 0, "Physical sky without aerial pass");
        s.atmosphere.aerialPerspective = true;
        s.lighting.sunDirection = {0, -1, 0};
        frame(r, c, s);
        finite(r.readHdrSnapshot().pixels);
        r.resizeOffscreen(1, 3);
        frame(r, c, s);
        finite(r.readHdrSnapshot().pixels);
        s.atmosphere.mode = AtmosphereMode::Analytic;
        frame(r, c, s);
        require(r.atmosphereStats().effective == AtmosphereMode::Analytic, "Analytic comparison");
        s.atmosphere.mode = AtmosphereMode::Off;
        frame(r, c, s);
        require(r.atmosphereStats().effective == AtmosphereMode::Off, "Atmosphere off");
        r.resizeOffscreen(3, 1);
        frame(r, c, s);
        r.waitIdle();
        require(r.validationErrors() == 0, "No validation errors after resize");
        o.atmosphereBudgetBytes = 1;
        Renderer limited(o);
        s.atmosphere.mode = AtmosphereMode::Physical;
        frame(limited, c, s);
        require(limited.atmosphereStats().fallback == AtmosphereFallback::Budget,
                "Budget selects analytic fallback");
        o = options();
        o.forceNoAtmosphere = true;
        Renderer unsupported(o);
        frame(unsupported, c, s);
        require(unsupported.atmosphereStats().fallback == AtmosphereFallback::Unsupported,
                "Capability fallback");
        std::cout << "Atmosphere GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

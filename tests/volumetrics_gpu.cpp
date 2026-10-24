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
    o.shadowCapacity.resolution = 128;
    return o;
}
void frame(Renderer &r, const Camera &c, const SceneSettings &s) {
    require(r.render(c, s), "Fog frame submitted");
    require(r.validationErrors() == 0, "Fog Vulkan validation");
}
void finite(const VolumetricSnapshot &v) {
    for (auto p : {&v.extinction, &v.source, &v.transmittance, &v.scattering, &v.sky, &v.skyTransmittance})
        for (auto a : *p)
            for (int k = 0; k < 4; ++k)
                require(std::isfinite(a[k]) && a[k] >= 0, "Finite nonnegative fog product");
}
size_t at(glm::uvec3 g, uint32_t x, uint32_t y, uint32_t z) { return (size_t(z) * g.y + y) * g.x + x; }
glm::dvec3 rayAt(glm::uvec3 g, uint32_t x, uint32_t y) {
    double t = std::tan(glm::radians(30.));
    return glm::normalize(
        glm::dvec3(((x + .5) / g.x * 2 - 1) * t * 65 / 49, -((y + .5) / g.y * 2 - 1) * t, -1));
}
// Sharp periodic occluders must be averaged over a fog cell, not sampled at a
// few points that can lock into a false pattern. Reference uses geometric ray /
// plane intersections, independent of CSM rasterization and the shader filter.
void slatFiltering(bool manual) {
    auto o = options();
    o.width = 192;
    o.height = 144;
    o.shadowCapacity.resolution = 1024;
    o.forceManualShadowFiltering = manual;
    Renderer r(o);
    SceneSettings s;
    s.terrainVisible = false;
    s.lighting.sunDirection = glm::normalize(glm::vec3(-.6f, .8f, -.35f));
    s.lighting.environmentEnabled = false;
    s.volumetrics.enabled = true;
    s.volumetrics.temporal = false;
    s.volumetrics.extinction = .01f;
    s.volumetrics.scaleHeight = 100000;
    s.volumetrics.albedo = glm::vec3(1);
    s.volumetrics.anisotropy = 0;
    auto quad = r.uploadMesh(makeQuad());
    MeshMaterial material;
    material.doubleSided = true;
    auto mh = r.createMaterial(material);
    std::vector<InstanceHandle> slats;
    for (int k = -4; k <= 4; ++k) {
        MeshInstance slat;
        slat.mesh = quad;
        slat.material = mh;
        slat.position.local = {k * 5., 112, -70};
        slat.scale = {1.5, 50, 1};
        slat.rotation = glm::angleAxis(glm::radians(-90.f), glm::vec3(1, 0, 0));
        slats.push_back(r.createInstance(slat));
    }
    for (double shift : {0., .73}) {
        Camera c;
        c.local = {shift, 100, 0};
        s.shadows.mode = ShadowMode::Off;
        frame(r, c, s);
        auto lit = r.readVolumetricSnapshot();
        s.shadows.mode = ShadowMode::CSM;
        frame(r, c, s);
        auto fog = r.readVolumetricSnapshot();
        std::vector<double> errors;
        double total = 0;
        const double fov = std::tan(glm::radians(30.));
        auto sun = glm::dvec3(s.lighting.sunDirection);
        for (uint32_t z = 0; z < fog.grid.z; ++z) {
            double z0 = volumetricDepth(z, fog.grid.z, 1000);
            double z1 = volumetricDepth(z + 1, fog.grid.z, 1000);
            if (z0 < 25 || z1 > 110)
                continue;
            for (uint32_t y = 0; y < fog.grid.y; ++y)
                for (uint32_t x = 0; x < fog.grid.x; ++x) {
                    double reference = 0;
                    constexpr int xySamples = 8, depthSamples = 64;
                    for (int iz = 0; iz < depthSamples; ++iz)
                        for (int iy = 0; iy < xySamples; ++iy)
                            for (int ix = 0; ix < xySamples; ++ix) {
                                double depth = glm::mix(z0, z1, (iz + .5) / depthSamples);
                                glm::dvec3 p((2 * (x + (ix + .5) / xySamples) / fog.grid.x - 1) * fov *
                                                     o.width / o.height * depth +
                                                 shift,
                                             (1 - 2 * (y + (iy + .5) / xySamples) / fog.grid.y) * fov * depth,
                                             -depth);
                                double travel = (12 - p.y) / sun.y;
                                auto hit = p + sun * travel;
                                bool blocked = false;
                                if (travel > 0 && hit.z > -120 && hit.z < -20)
                                    for (int k = -4; k <= 4; ++k)
                                        blocked |= std::abs(hit.x - k * 5) < 1.5;
                                reference += !blocked;
                            }
                    reference /= xySamples * xySamples * depthSamples;
                    if (reference < .05 || reference > .95)
                        continue; // Measure partial cells; existing tests cover solid shadows.
                    size_t index = at(fog.grid, x, y, z);
                    double actual = fog.source[index].x / lit.source[index].x;
                    double error = std::abs(actual - reference);
                    errors.push_back(error);
                    total += error;
                }
        }
        require(errors.size() > 100, "Slat fixture contains enough partially shadowed cells");
        std::sort(errors.begin(), errors.end());
        double mean = total / errors.size(), p95 = errors[size_t(.95 * (errors.size() - 1))];
        std::cout << "Slat visibility error (manual " << manual << ", shift " << shift << ", "
                  << errors.size() << " cells): mean " << mean << " p95 " << p95 << '\n';
        require(mean < .06 && p95 < .15, "Periodic shadows alias against the fog grid");
    }
    for (auto h : slats)
        r.removeInstance(h);
    r.releaseMaterial(mh);
    r.releaseMesh(quad);
}
int main() {
    try {
        auto o = options();
        Renderer r(o);
        Camera c;
        c.local.y = 100;
        SceneSettings s;
        s.terrainVisible = false;
        s.shadows.mode = ShadowMode::Off;
        s.lighting.sunEnabled = false;
        s.lighting.environmentEnabled = false;
        s.volumetrics.enabled = true;
        s.volumetrics.temporal = false;
        s.volumetrics.distance = 100;
        s.volumetrics.extinction = .01f;
        s.volumetrics.height = 100;
        s.volumetrics.scaleHeight = 100000;
        s.volumetrics.albedo = glm::vec3(0);
        frame(r, c, s);
        require(r.volumetricStats().active, "Fog enabled");
        auto v = r.readVolumetricSnapshot();
        finite(v);
        for (auto cell : v.source)
            near(cell.w, 0, 0, "Spatial fog does not globally reject image TAA");
        auto g = v.grid;
        const uint32_t x = g.x / 2, y = g.y / 2, z = 40;
        auto ray = rayAt(g, x, y);
        double distance = volumetricDepth(z, g.z, s.volumetrics.distance) / (-ray.z);
        double expected = std::exp(-.01 * distance);
        auto t = glm::dvec3(v.transmittance[at(g, x, y, z)]);
        near(t.x, expected, .002, "Homogeneous Beer Lambert");
        auto zero = s;
        zero.volumetrics.extinction = 0;
        frame(r, c, zero);
        auto noFog = r.readHdrSnapshot();
        zero.volumetrics.enabled = false;
        frame(r, c, zero);
        auto disabled = r.readHdrSnapshot();
        for (size_t k = 0; k < noFog.pixels.size(); ++k)
            near(glm::length(noFog.pixels[k] - disabled.pixels[k]), 0, 0, "Zero-density exact bypass");
        // A stationary medium must not blink when its history or image TAA
        // advances. Steep height density and a local edge reproduce the review
        // presets.
        {
            auto stable = s;
            stable.volumetrics.temporal = true;
            stable.volumetrics.distance = 1000;
            stable.volumetrics.extinction = .0008f;
            stable.volumetrics.scaleHeight = 40;
            FogVolume mist;
            mist.position.local = {0, 100, -45};
            mist.shape = FogShape::Box;
            mist.extent = {25, 16, 25};
            mist.extinction = .035f;
            mist.falloff = .3f;
            mist.albedo = glm::vec3(0);
            auto handle = r.createFogVolume(mist);
            for (uint32_t samples : {1u, 2u})
                for (bool taa : {false, true}) {
                    stable.volumetrics.samples = samples;
                    stable.taa.enabled = taa;
                    frame(r, c, stable);
                    auto previous = r.readVolumetricSnapshot();
                    double maximum = 0;
                    for (int k = 0; k < 16; ++k) {
                        frame(r, c, stable);
                        auto current = r.readVolumetricSnapshot();
                        for (size_t j = 0; j < current.transmittance.size(); ++j)
                            maximum = std::max(maximum, double(std::abs(current.transmittance[j].x -
                                                                        previous.transmittance[j].x)));
                        previous = std::move(current);
                    }
                    std::cout << "Stationary mist maximum transmission change (samples " << samples
                              << ", TAA " << taa << "): " << maximum << '\n';
                    require(maximum < .001, "Stationary fog opacity flickers");
                }
            r.removeFogVolume(handle);
            // Independent fine quadrature through steep height fog, including the
            // far fade. Check every boundary, not just the terminal transmission.
            stable.taa.enabled = false;
            frame(r, c, stable);
            auto height = r.readVolumetricSnapshot();
            double maxError = 0;
            for (uint32_t yy = 0; yy < height.grid.y; ++yy)
                for (uint32_t xx = 0; xx < height.grid.x; ++xx) {
                    auto direction = rayAt(height.grid, xx, yy);
                    double opticalDepth = 0;
                    for (uint32_t zz = 0; zz < height.grid.z; ++zz) {
                        double begin = volumetricDepth(zz, height.grid.z, 1000);
                        double step = (volumetricDepth(zz + 1, height.grid.z, 1000) - begin) / 128;
                        for (int k = 0; k < 128; ++k) {
                            double depth = begin + (k + .5) * step;
                            double altitude = direction.y * depth / -direction.z;
                            double f = std::clamp((depth - 900) / 100, 0., 1.);
                            double density = .0008 * std::exp(-altitude / 40) * (1 - f * f * (3 - 2 * f));
                            opticalDepth += density * step / -direction.z;
                        }
                        double actual = height.transmittance[at(height.grid, xx, yy, zz + 1)].x;
                        maxError = std::max(maxError, std::abs(actual - std::exp(-opticalDepth)));
                    }
                }
            std::cout << "Height layer maximum transmission error: " << maxError << '\n';
            require(maxError < .002, "Height layers disagree with independent quadrature");
        }
        // Empty background is still a lighting consumer.
        s.volumetrics.albedo = glm::vec3(.9f);
        LocalLight light;
        light.position.local = {0, 100, -15};
        light.range = 30;
        light.intensity = 100000;
        auto lh = r.createLight(light);
        frame(r, c, s);
        v = r.readVolumetricSnapshot();
        float peak = 0;
        for (auto p : v.source)
            peak = std::max(peak, glm::length(glm::vec3(p)));
        require(peak > 1e-5 && r.volumetricStats().selectedLights == 1,
                "Light illuminates empty screen region");
        light.type = LightType::Spot;
        light.direction = {0, 0, -1};
        r.updateLight(lh, light);
        frame(r, c, s);
        finite(r.readVolumetricSnapshot());
        r.removeLight(lh);
        // Own-depth opaque and blended emission: no material lighting ambiguity.
        auto quad = r.uploadMesh(makeQuad());
        MeshMaterial m;
        m.unlit = true;
        m.baseColour = {0, 0, 0, 1};
        m.emissive = glm::vec3(10000);
        auto material = r.createMaterial(m);
        MeshInstance instance;
        instance.mesh = quad;
        instance.material = material;
        instance.position.local = {0, 100, -20};
        instance.scale = {50, 50, 1};
        auto ih = r.createInstance(instance);
        s.volumetrics.albedo = glm::vec3(0);
        frame(r, c, s);
        auto opaque = r.readHdrSnapshot();
        size_t centre = (o.height / 2) * o.width + o.width / 2;
        near(opaque.pixels[centre].x, 10000 * opaque.preExposure * std::exp(-.2), .002,
             "Opaque partial-cell depth transport");
        r.removeInstance(ih);
        r.releaseMaterial(material);
        m.alpha = AlphaMode::Blend;
        m.baseColour.a = .5;
        material = r.createMaterial(m);
        instance.material = material;
        ih = r.createInstance(instance);
        frame(r, c, s);
        auto transparent = r.readHdrSnapshot();
        // Existing background uses physical scene HDR clear directly.
        near(transparent.pixels[centre].x,
             .5 * 10000 * transparent.preExposure * std::exp(-.2) + .5 * s.background[0] * std::exp(-.95),
             .003, "Transparent own-depth transport");
        r.removeInstance(ih);
        r.releaseMaterial(material);
        // Full local + physical overlap, independently integrated extinction and
        // source.
        s.atmosphere.mode = AtmosphereMode::Physical;
        s.atmosphere.multipleScattering = false;
        s.atmosphere.groundAlbedo = 0;
        s.atmosphere.mieScattering = s.atmosphere.mieAbsorption = 0;
        s.atmosphere.ozone = glm::vec3(0);
        s.atmosphere.rayleigh = {.0001f, .00005f, .000025f};
        s.volumetrics.extinction = .003f;
        s.volumetrics.albedo = glm::vec3(.8f);
        s.volumetrics.anisotropy = 0;
        s.volumetrics.distance = 1000;
        s.lighting.sunEnabled = true;
        s.lighting.sunDirection = {0, 1, 0};
        frame(r, c, s);
        v = r.readVolumetricSnapshot();
        g = v.grid;
        ray = rayAt(g, x, y);
        const uint32_t slice = 55;
        distance = volumetricDepth(slice, g.z, s.volumetrics.distance) / (-ray.z);
        glm::dvec3 trans(1), scatter(0);
        const double dt = distance / 4096, pi = 3.141592653589793;
        for (int k = 0; k < 4096; ++k) {
            auto p = ray * ((k + .5) * dt);
            double height = glm::length(glm::dvec3(0, s.atmosphere.planetRadius + 100., 0) + p) -
                            s.atmosphere.planetRadius;
            auto re = glm::dvec3(s.atmosphere.rayleigh) * std::exp(-height / s.atmosphere.rayleighHeight);
            double fogDensity = .003 * std::exp(-p.y / 100000);
            auto ext = re + fogDensity;
            auto sunT = glm::exp(-re * double(s.atmosphere.rayleighHeight));
            auto source = (re * (3 * (1 + ray.y * ray.y) / (16 * pi)) + fogDensity * .8 / (4 * pi)) * sunT *
                          double(s.lighting.sunLux) * double(v.preExposure);
            scatter += trans * source * dt;
            trans *= glm::exp(-ext * dt);
        }
        auto observedT = glm::dvec3(v.transmittance[at(g, x, y, slice)]),
             observedS = glm::dvec3(v.scattering[at(g, x, y, slice)]);
        near(glm::length(observedT - trans), 0, .015, "Mixed atmospheric extinction");
        near(glm::length(observedS - scatter) / glm::length(scatter), 0, .08,
             "Mixed atmospheric source reference");
        // Opaque tail transport across the exact endpoint and well beyond it.
        auto tailSettings = s;
        Camera tailCamera = c;
        tailCamera.local.y = 1000;
        tailSettings.volumetrics.height = 1000;
        tailSettings.lighting.sunEnabled = false;
        tailSettings.volumetrics.albedo = glm::vec3(0);
        MeshMaterial emission;
        emission.unlit = true;
        emission.baseColour = {0, 0, 0, 1};
        emission.emissive = glm::vec3(10000);
        auto tailMaterial = r.createMaterial(emission);
        instance.material = tailMaterial;
        instance.scale = {10000, 10000, 1};
        auto tailInstance = r.createInstance(instance);
        for (double depth : {1., 20., 999., 1000., 1001., 4000., 12000.}) {
            instance.position.local = {0, 1000, -depth};
            r.updateInstance(tailInstance, instance);
            frame(r, tailCamera, tailSettings);
            auto hdr = r.readHdrSnapshot();
            auto atmosphere =
                atmosphereReference(tailSettings.atmosphere, 1000, {0, 0, -1}, {0, 1, 0}, depth, 2048, 1);
            // Integral of the cubic endpoint fade is half its interval.
            double local = 0;
            if (depth <= 900)
                local = depth;
            else if (depth >= 1000)
                local = 950;
            else {
                double q = (depth - 900) / 100;
                local = 900 + 100 * (q - q * q * q + .5 * q * q * q * q);
            }
            auto expected = atmosphere.transmittance *
                            std::exp(-double(tailSettings.volumetrics.extinction) * local) *
                            (10000. * hdr.preExposure);
            std::cout << "Tail " << depth << " actual " << hdr.pixels[centre].x << " expected " << expected.x
                      << " error " << glm::length(glm::dvec3(hdr.pixels[centre]) - expected) << std::endl;
            near(glm::length(glm::dvec3(hdr.pixels[centre]) - expected), 0, .002,
                 "Atmosphere tail and local endpoint composition");
        }
        r.removeInstance(tailInstance);
        r.releaseMaterial(tailMaterial);
        // Current-frame CSM suppresses scattering under an owned horizontal
        // occluder.
        auto shaft = s;
        shaft.atmosphere.mode = AtmosphereMode::Off;
        shaft.volumetrics.distance = 100;
        shaft.volumetrics.extinction = .01f;
        shaft.volumetrics.albedo = glm::vec3(1);
        shaft.volumetrics.temporal = false;
        shaft.shadows.mode = ShadowMode::CSM;
        shaft.shadows.nearDistance = 100;
        shaft.shadows.farDistance = 200;
        MeshMaterial roof;
        roof.doubleSided = true;
        auto roofMaterial = r.createMaterial(roof);
        MeshInstance roofInstance;
        roofInstance.mesh = quad;
        roofInstance.material = roofMaterial;
        roofInstance.position.local = {0, 105, -20};
        roofInstance.scale = {8, 12, 1};
        roofInstance.rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(1, 0, 0));
        auto roofHandle = r.createInstance(roofInstance);
        frame(r, c, shaft);
        auto shadowed = r.readVolumetricSnapshot();
        shaft.shadows.mode = ShadowMode::Off;
        frame(r, c, shaft);
        auto lit = r.readVolumetricSnapshot();
        uint32_t shadowZ = uint32_t(std::log1p(20. / volumetricDepthOffset) /
                                    std::log1p(100. / volumetricDepthOffset) * lit.grid.z);
        size_t shadowAt = at(lit.grid, lit.grid.x / 2, lit.grid.y / 2, shadowZ);
        require(glm::length(glm::vec3(shadowed.source[shadowAt])) <
                    glm::length(glm::vec3(lit.source[shadowAt])) * .2f,
                "CSM light shaft occlusion");
        require(shadowed.extinction[shadowAt].w > .9f, "CSM coverage observable");
        // Include shadow boundaries, where stochastic visibility previously
        // blinked.
        for (bool taa : {false, true}) {
            auto stable = shaft;
            stable.shadows.mode = ShadowMode::CSM;
            stable.volumetrics.temporal = true;
            stable.volumetrics.samples = 1;
            stable.taa.enabled = taa;
            frame(r, c, stable);
            auto previous = r.readVolumetricSnapshot();
            double maximum = 0, peak = 0;
            for (int k = 0; k < 16; ++k) {
                frame(r, c, stable);
                auto current = r.readVolumetricSnapshot();
                for (size_t j = 0; j < current.scattering.size(); ++j) {
                    peak = std::max(peak, double(glm::length(glm::vec3(current.scattering[j]))));
                    maximum = std::max(maximum, double(glm::length(glm::vec3(current.scattering[j] -
                                                                             previous.scattering[j]))));
                }
                previous = std::move(current);
            }
            std::cout << "Stationary shaft relative scattering change (TAA " << taa << "): " << maximum / peak
                      << '\n';
            require(peak > 0 && maximum / peak < .003, "Stationary shaft lighting flickers or drifts");
        }
        for (auto visibility : {VisibilityMode::Direct, VisibilityMode::Indirect, VisibilityMode::HiZ}) {
            auto combined = shaft;
            combined.visibility = visibility;
            combined.shadows.mode = ShadowMode::CSM;
            combined.gtao.mode = GtaoMode::Half;
            combined.taa.enabled = true;
            combined.bloom.intensity = .05f;
            frame(r, c, combined);
            frame(r, c, combined);
            finite(r.readVolumetricSnapshot());
        }
        r.removeInstance(roofHandle);
        r.releaseMaterial(roofMaterial);
        // Capped assignment never falls back to scanning every scene light.
        std::vector<LightHandle> crowded;
        for (int k = 0; k < 140; ++k) {
            LocalLight l;
            l.position.local = {0, 100, -20};
            l.range = 100;
            l.intensity = 10;
            crowded.push_back(r.createLight(l));
        }
        frame(r, c, shaft);
        frame(r, c, shaft);
        frame(r, c, shaft);
        require(r.volumetricStats().selectedLights == 128 && r.volumetricStats().excludedLights == 12,
                "Independent selected-light cap");
        require(r.volumetricStats().overflowClusters > 0, "Fog list overflow reported");
        for (auto h : crowded)
            r.removeLight(h);
        // Local volume support and smooth boundary, independent of height fog.
        auto local = shaft;
        local.volumetrics.extinction = 0;
        local.volumetrics.albedo = glm::vec3(0);
        FogVolume box;
        box.position.local = {0, 100, -30};
        box.extent = {20, 20, 10};
        box.shape = FogShape::Box;
        box.extinction = .03f;
        box.albedo = glm::vec3(0);
        auto boxHandle = r.createFogVolume(box);
        frame(r, c, local);
        auto boxSnapshot = r.readVolumetricSnapshot();
        auto boxRay = rayAt(boxSnapshot.grid, boxSnapshot.grid.x / 2, boxSnapshot.grid.y / 2);
        double tau = 0;
        double length = volumetricDepth(60, boxSnapshot.grid.z, 100) / (-boxRay.z), step = length / 4096;
        for (int k = 0; k < 4096; ++k)
            tau += fogDensity(box, boxRay * ((k + .5) * step) - glm::dvec3(0, 0, -30)) * step;
        near(boxSnapshot
                 .transmittance[at(boxSnapshot.grid, boxSnapshot.grid.x / 2, boxSnapshot.grid.y / 2, 60)]
                 .x,
             std::exp(-tau), .015, "Local box boundary quadrature");
        local.volumetrics.localDensity = .5f;
        frame(r, c, local);
        auto halfDensity = r.readVolumetricSnapshot();
        for (size_t k = 0; k < boxSnapshot.transmittance.size(); ++k)
            near(halfDensity.transmittance[k].x, std::sqrt(boxSnapshot.transmittance[k].x), .001,
                 "Local density multiplier controls optical thickness");
        local.volumetrics.localDensity = 0;
        frame(r, c, local);
        require(!r.volumetricStats().active, "Zero local density bypasses otherwise empty fog");
        r.removeFogVolume(boxHandle);
        // Fog history does not require image TAA.
        s.volumetrics.temporal = true;
        frame(r, c, s);
        frame(r, c, s);
        require(r.volumetricStats().historyValid, "Independent fog history");
        LocalLight transient;
        transient.position.local = {0, 100, -20};
        transient.range = 20;
        transient.intensity = 100000;
        transient.volumetricHistory = false;
        auto transientHandle = r.createLight(transient);
        frame(r, c, s);
        auto transientSnapshot = r.readVolumetricSnapshot();
        size_t rejected = 0;
        for (auto p : transientSnapshot.source)
            rejected += p.w > 0;
        require(rejected > 0, "History-disabled light rejects influenced cells");
        transient.position.local.x = 20;
        r.updateLight(transientHandle, transient);
        frame(r, c, s);
        r.removeLight(transientHandle);
        frame(r, c, s);
        auto removed = r.readVolumetricSnapshot();
        rejected = 0;
        for (auto p : removed.source)
            rejected += p.w > 0;
        require(rejected > 0, "Removed light rejects old bounds");

        auto original = c;
        c.chunk.x += 1;
        c.local.x -= o.terrainGrid.extent();
        frame(r, c, s);
        require(r.volumetricStats().historyValid, "Rebase preserves fog history");
        s.output.compensation += 1;
        frame(r, c, s);
        require(r.volumetricStats().historyValid, "Exposure retains fog history");
        finite(r.readVolumetricSnapshot());
        r.resetTemporalHistory();
        frame(r, c, s);
        require(!r.volumetricStats().historyValid, "Explicit fog reset");
        c.local.x += 300;
        frame(r, c, s);
        require(!r.volumetricStats().historyValid, "Camera cut rejects fog history");
        c = original;
        FogVolume volume;
        volume.position.local = {0, 100, -30};
        auto vh = r.createFogVolume(volume);
        frame(r, c, s);
        require(!r.volumetricStats().historyValid, "Volume edit rejects history");
        r.removeFogVolume(vh);
        frame(r, c, s);
        require(!r.volumetricStats().historyValid, "Volume removal rejects history");
        s.taa.enabled = true;
        s.bloom.intensity = .05f;
        s.volumetrics.quality = VolumetricQuality::High;
        s.volumetrics.samples = 2;
        s.atmosphere.aerialPerspective = false;
        frame(r, c, s);
        frame(r, c, s);
        finite(r.readVolumetricSnapshot());
        s.atmosphere.mode = AtmosphereMode::Analytic;
        s.volumetrics.diagnostic = 1;
        frame(r, c, s);
        s.volumetrics.diagnostic = 0;
        s.clusterDiagnostic = 1;
        frame(r, c, s);
        require(!r.volumetricStats().active, "Diagnostic bypass");
        s.clusterDiagnostic = 0;
        s.atmosphere.mode = AtmosphereMode::Physical;
        s.atmosphere.diagnostic = 1;
        frame(r, c, s);
        require(!r.volumetricStats().active, "Atmospheric transport diagnostic bypasses local fog");
        near(r.readAtmosphereSnapshot().aerialStartDistance, 0, 0,
             "Diagnostic retains full atmosphere transport");
        s.atmosphere.diagnostic = 0;
        s.atmosphere.mode = AtmosphereMode::Analytic;
        r.setRenderHeight(7);
        frame(r, c, s);
        require(!r.volumetricStats().historyValid, "Resize rejects history");
        for (int kind = 0; kind < 4; ++kind) {
            auto fallback = options();
            fallback.forceNoVolumetrics = kind == 0;
            fallback.volumetricCapacity.bytes = kind == 1 ? 1 : (384ull << 20);
            fallback.forceFixedMaterials = kind == 2;
            fallback.curvedHorizon = kind == 3;
            fallback.forceNoAtmosphere = kind == 3;
            Renderer f(fallback);
            Camera distant = c;
            if (kind == 3)
                distant.chunk = {100000000, -100000000};
            frame(f, distant, s);
            if (kind == 3) {
                frame(f, distant, s);
                require(f.volumetricStats().historyValid, "Curved large-world fog history");
                finite(f.readVolumetricSnapshot());
            }
            require(f.volumetricStats().active == (kind >= 2), "Capability/budget fallback");
        }
        rejects(
            [&] {
                auto invalid = options();
                invalid.volumetricCapacity.volumes = ~0u;
                Renderer rejected(invalid);
            },
            "Invalid fog capacity rejected before pool allocation");
        r.releaseMesh(quad);
        require(r.validationErrors() == 0, "Final validation");
        slatFiltering(false);
        slatFiltering(true);
        std::cout << "Volumetric GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

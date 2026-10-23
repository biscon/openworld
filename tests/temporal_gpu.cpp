#include "render_fixtures.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
namespace {
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
SceneSettings scene() {
    SceneSettings s;
    s.terrainVisible = false;
    s.shadows.mode = ShadowMode::Off;
    s.output.ev100 = 0;
    s.background[0] = s.background[1] = s.background[2] = 0;
    return s;
}
void finite(const PostSnapshot &s) {
    for (const auto *v : {&s.resolved, &s.motion})
        for (auto p : *v)
            for (int c = 0; c < 4; ++c)
                require(std::isfinite(p[c]), "Finite temporal output");
    for (const auto &v : s.bloom)
        for (auto p : v)
            for (int c = 0; c < 4; ++c)
                require(std::isfinite(p[c]) && p[c] >= 0, "Finite bloom");
}
void frame(Renderer &r, const Camera &c, const SceneSettings &s) {
    require(r.render(c, s), "Submitted fixture frame");
    require(r.validationErrors() == 0, "No Vulkan validation errors");
}
void convergence() {
    auto o = options();
    Renderer native(o);
    auto high = o;
    high.renderHeight = 49 * 8;
    Renderer reference(high);
    auto settings = scene();
    Camera camera;
    camera.local = {0, 0, 10};
    auto install = [](Renderer &r) {
        MeshData triangle;
        triangle.vertices = {{{-3, -2, 0}}, {{3, -1.7f, 0}}, {{-.7f, 3, 0}}};
        triangle.indices = {0, 1, 2};
        MeshMaterial m;
        m.unlit = m.doubleSided = true;
        m.baseColour = {.8, .8, .8, 1};
        MeshInstance i;
        i.mesh = r.uploadMesh(triangle);
        i.material = r.createMaterial(m);
        r.createInstance(i);
    };
    install(native);
    install(reference);
    frame(native, camera, settings);
    frame(reference, camera, settings);
    auto off = native.readHdrSnapshot(), super = reference.readHdrSnapshot();
    settings.taa.enabled = true;
    for (int i = 0; i < 24; ++i)
        frame(native, camera, settings);
    auto resolved = native.readPostSnapshot();
    double offError = 0, taaError = 0;
    for (uint32_t y = 0; y < 49; ++y)
        for (uint32_t x = 0; x < 65; ++x) {
            double expected = 0;
            for (uint32_t j = 0; j < 8; ++j)
                for (uint32_t i = 0; i < 8; ++i)
                    expected += super.pixels[(y * 8 + j) * super.width + x * 8 + i].r / 64.;
            auto at = y * 65 + x;
            offError += std::pow(off.pixels[at].r - expected, 2);
            taaError += std::pow(resolved.resolved[at].r - expected, 2);
        }
    std::cout << "Static edge squared error: off=" << offError << " TAA=" << taaError << '\n';
    require(taaError < offError * .9, "TAA improves owned slanted-edge error against 8x supersampling");
    // Output-only resize does not change temporal extent or reset history.
    native.setRenderHeight(49);
    frame(native, camera, settings);
    native.resizeOffscreen(130, 98);
    frame(native, camera, settings);
    require(native.postStats().historyValid, "Same-aspect output resize retains scene history");
    native.setRenderHeight(51);
    frame(native, camera, settings);
    require(!native.postStats().historyValid, "Scene resize resets history");
    settings.normalView = true;
    settings.terrainVisible = true;
    frame(native, camera, settings);
    require(!native.postStats().taa && native.postStats().taaFallback == PostFallback::Diagnostic,
            "Diagnostics bypass temporal processing");
}
void transparency() {
    auto o = options();
    o.forceFixedMaterials = true;
    Renderer r(o);
    auto s = scene();
    s.taa.enabled = true;
    s.output.ev100 = 15;
    s.bloom.intensity = .05;
    Camera c;
    c.local = {0, 0, 10};
    MeshInstance back;
    back.mesh = r.uploadMesh(makeQuad());
    back.scale = {4, 4, 4};
    MeshMaterial m;
    m.unlit = m.doubleSided = true;
    m.emissive = {1000, 1000, 1000};
    back.material = r.createMaterial(m);
    r.createInstance(back);
    MeshInstance front = back;
    front.position.local.z = 2;
    m.alpha = AlphaMode::Blend;
    m.baseColour.a = .4;
    m.emissive = {200000, 1000, 0};
    front.material = r.createMaterial(m);
    auto h = r.createInstance(front);
    for (int i = 0; i < 5; ++i)
        frame(r, c, s);
    auto before = r.readPostSnapshot();
    size_t centre = 24 * 65 + 32;
    near(before.motion[centre].z, 10, .01, "Transparency preserves opaque depth/motion");
    require(before.reactivity[centre] > .99, "Transparent emissive layer marks reactivity");
    double sum = 0;
    for (auto p : before.bloom[0])
        sum += p.r;
    require(sum > 0, "Transparent emission reaches bloom");
    r.resetTemporalHistory();
    frame(r, c, s);
    auto bloomBefore = r.readPostSnapshot();
    s.output.compensation = 1;
    r.resetTemporalHistory();
    frame(r, c, s);
    auto brighter = r.readPostSnapshot();
    for (size_t l = 0; l < before.bloom.size(); ++l)
        for (size_t p = 0; p < before.bloom[l].size(); ++p)
            near(brighter.bloom[l][p].r, 2 * bloomBefore.bloom[l][p].r, .006,
                 "Bloom thresholds and robust weights track pre-exposure");
    r.removeInstance(h);
    frame(r, c, s);
    auto after = r.readPostSnapshot();
    auto raw = r.readHdrSnapshot();
    near(after.resolved[centre].r, raw.pixels[centre].r, .001,
         "Disappearing transparent source leaves no centre trail");
    // A masked texture exercises actual shared alpha testing in depth/colour paths.
    TextureData texture;
    texture.format = TextureFormat::Srgb8;
    texture.width = texture.height = 2;
    texture.levels = 2;
    texture.bytes = {255, 255, 255, 0,   255, 255, 255, 255, 255, 255,
                     255, 255, 255, 255, 255, 0,   255, 255, 255, 128};
    m.alpha = AlphaMode::Mask;
    m.baseColour.a = 1;
    m.colourTexture = r.uploadTexture(texture);
    front.material = r.createMaterial(m);
    r.createInstance(front);
    s.visibility = VisibilityMode::HiZ;
    s.gtao.mode = GtaoMode::Full;
    for (int i = 0; i < 3; ++i)
        frame(r, c, s);
    finite(r.readPostSnapshot());
    require(r.validationErrors() == 0, "Fixed-binding masked/transparency integration validation");
}
void bloomImpulse() {
    Renderer r(options());
    auto s = scene();
    s.output.ev100 = 15;
    s.bloom.intensity = .05f;
    Camera c;
    c.local = {0, 0, 10};
    const float pixelMetres = 20 * std::tan(glm::radians(30.f)) / 49;
    MeshInstance source;
    source.mesh = r.uploadMesh(makeQuad());
    source.scale = glm::vec3(pixelMetres * .49f);
    InstanceHandle handle;
    MaterialHandle previousMaterial;
    double previousEnergy = 0;
    for (float emission : {10000.f, 20000.f, 80000.f}) {
        MeshMaterial m;
        m.unlit = m.doubleSided = true;
        m.baseColour = {0, 0, 0, 1};
        m.emissive = glm::vec3(emission);
        source.material = r.createMaterial(m);
        if (previousMaterial.generation != 0) {
            r.updateInstance(handle, source);
            r.releaseMaterial(previousMaterial);
        } else {
            handle = r.createInstance(source);
        }
        previousMaterial = source.material;
        frame(r, c, s);
        const auto raw = r.readHdrSnapshot();
        size_t lit = 0;
        for (auto pixel : raw.pixels)
            lit += pixel.r > 0;
        require(lit == 1, "Owned bloom fixture contains exactly one bright pixel");
        const auto post = r.readPostSnapshot();
        finite(post);
        double energy = 0;
        for (auto pixel : post.bloom[0])
            energy += pixel.r;
        require(energy > previousEnergy, "Isolated emissive ramp produces increasing bloom");
        previousEnergy = energy;
        const auto size = post.bloomSizes[0];
        const auto &level = post.bloom[0];
        require(level[(size.y / 2) * size.x + size.x / 2].r > level.front().r,
                "Impulse bloom remains concentrated around its source");
        for (uint32_t y = 0; y < size.y; ++y)
            for (uint32_t x = 0; x < size.x; ++x)
                near(level[y * size.x + x].r, level[y * size.x + size.x - 1 - x].r, .0001,
                     "Centred impulse reconstruction is symmetric at odd extents");
    }
    source.position.local.x = -32 * pixelMetres;
    r.updateInstance(handle, source);
    frame(r, c, s);
    const auto edge = r.readPostSnapshot();
    finite(edge);
    double left = 0, right = 0;
    const auto size = edge.bloomSizes[0];
    for (uint32_t y = 0; y < size.y; ++y) {
        left += edge.bloom[0][y * size.x].r;
        right += edge.bloom[0][y * size.x + size.x - 1].r;
    }
    require(left > right && left > 0, "Screen-edge bloom clamps without wrapping its source");
    require(r.validationErrors() == 0, "Impulse snapshot and screen-edge validation clean");
}
} // namespace
int main() {
    try {
        Renderer r(options());
        auto s = scene();
        Camera c;
        c.local = {0, 0, 10};
        MeshMaterial material;
        material.unlit = true;
        material.baseColour = {.5, .3, .2, 1};
        material.doubleSided = true;
        MeshInstance instance;
        instance.mesh = r.uploadMesh(makeQuad());
        instance.material = r.createMaterial(material);
        instance.scale = {3, 3, 3};
        auto handle = r.createInstance(instance);
        frame(r, c, s);
        auto raw = r.readHdrSnapshot();
        s.taa.enabled = true;
        frame(r, c, s);
        require(!r.postStats().historyValid, "Initial TAA history invalid");
        auto initial = r.readPostSnapshot();
        finite(initial);
        size_t centre = 24 * 65 + 32;
        near(initial.resolved[centre].r, raw.pixels[centre].r, .001, "Uniform current reconstruction");
        for (int i = 0; i < 10; ++i)
            frame(r, c, s);
        auto converged = r.readPostSnapshot();
        require(r.postStats().historyValid, "Submitted history valid");
        near(converged.motion[centre].x, 0, 1e-6, "Static geometry has no unjittered motion");
        near(converged.motion[centre].y, 0, 1e-6, "Jitter is excluded from motion");
        near(converged.motion[centre].z, 10, .01, "Previous view depth");
        require(converged.motion[centre].w == 1, "Stable mesh correspondence");
        s.output.compensation = 1;
        frame(r, c, s);
        auto exposed = r.readPostSnapshot();
        near(exposed.resolved[centre].r, 2 * converged.resolved[centre].r, .002,
             "History follows changed exposure immediately");
        require(r.postStats().historyValid, "Exposure preserves history");
        instance.position.local.x = .2;
        r.updateInstance(handle, instance);
        instance.position.local.x = .5;
        r.updateInstance(handle, instance);
        frame(r, c, s);
        auto moving = r.readPostSnapshot();
        float expected = -.5f / (10 * std::tan(glm::radians(30.f)) * (65.f / 49)) * .5f;
        near(moving.motion[centre].x, expected, 3e-5,
             "Velocity uses last submitted pose, not last CPU update");
        r.invalidateInstanceHistory(handle);
        frame(r, c, s);
        require(r.readPostSnapshot().motion[centre].w == 0,
                "Explicit instance teleport invalidates correspondence");
        frame(r, c, s);
        r.resetTemporalHistory();
        frame(r, c, s);
        require(!r.postStats().historyValid, "Explicit camera reset");
        frame(r, c, s);
        c.chunk.x += 1;
        c.local.x -= 4;
        frame(r, c, s);
        require(r.postStats().historyValid, "Representation-only camera rebase preserves history");
        near(r.readPostSnapshot().motion[centre].x, 0, 1e-6, "Rebase has zero physical motion");
        r.removeInstance(handle);
        auto replacement = r.createInstance(instance);
        require(replacement.slot == handle.slot && replacement.generation != handle.generation,
                "Fixture exercises slot reuse");
        frame(r, c, s);
        require(r.readPostSnapshot().motion[centre].w == 0, "Recycled generation rejects prior surface");
        for (auto visibility : {VisibilityMode::Direct, VisibilityMode::Indirect, VisibilityMode::HiZ}) {
            s.visibility = visibility;
            s.opaqueDepthPrepass = true;
            s.gtao.mode = GtaoMode::Half;
            frame(r, c, s);
            frame(r, c, s);
            finite(r.readPostSnapshot());
        }
        s.gtao.mode = GtaoMode::Off;
        s.opaqueDepthPrepass = false;
        s.visibility = VisibilityMode::Indirect;
        s.taa.enabled = false;
        r.removeInstance(replacement);
        s.output.compensation = 0;
        s.background[0] = s.background[1] = s.background[2] = 2;
        s.bloom.intensity = .05f;
        s.bloom.threshold = 1;
        for (auto extent : {glm::uvec2(65, 49), glm::uvec2(3, 5), glm::uvec2(1, 1)}) {
            r.resizeOffscreen(extent.x, extent.y);
            frame(r, c, s);
            frame(r, c, s);
            auto b = r.readPostSnapshot();
            finite(b);
            require(!b.bloom.empty(), "Bloom independent of TAA");
            const float expectedBloom = 2 * bloomContribution(2, exposure(s.output), s.bloom.knee);
            for (auto &level : b.bloom)
                for (auto p : level)
                    near(p.r, expectedBloom, .004,
                         "Normalized bloom preserves constant field including tiny/odd "
                         "edges");
        }
        s.bloom.intensity = 0;
        frame(r, c, s);
        require(!r.postStats().bloom && !r.postStats().taa, "Zero-intensity bypass");
        auto small = options();
        small.taaBudgetBytes = 1;
        small.bloomBudgetBytes = 1;
        Renderer constrained(small);
        s.taa.enabled = true;
        s.bloom.intensity = .05f;
        frame(constrained, c, s);
        require(constrained.postStats().taaFallback == PostFallback::Budget &&
                    constrained.postStats().bloomFallback == PostFallback::Budget,
                "Independent budget fallbacks");
        auto unsupportedOptions = options();
        unsupportedOptions.forceNoTaa = unsupportedOptions.forceNoBloom = true;
        {
            Renderer unsupported(unsupportedOptions);
            frame(unsupported, c, s);
            require(unsupported.postStats().taaFallback == PostFallback::Unsupported &&
                        unsupported.postStats().bloomFallback == PostFallback::Unsupported,
                    "Forced unsupported capability paths bypass both stages");
        }
        convergence();
        transparency();
        bloomImpulse();
        std::cout << "Temporal GPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

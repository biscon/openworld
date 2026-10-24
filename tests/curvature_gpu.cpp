#include "horizon.hpp"
#include "renderer.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>

using namespace ow;
namespace {
glm::dvec3 referenceNormal(glm::dvec3 p, glm::dvec3 n, glm::vec4 c) {
    // Differentiate the double-precision mapping, independently of the GPU frame formula.
    const auto derivative = [&](glm::dvec3 axis) {
        return (curvePosition(p + axis * .125, c) - curvePosition(p - axis * .125, c)) * 4.;
    };
    const auto x = derivative({1, 0, 0}), y = derivative({0, 1, 0}), z = derivative({0, 0, 1});
    return glm::normalize(n.x * glm::cross(y, z) + n.y * glm::cross(z, x) + n.z * glm::cross(x, y));
}
void precision(Renderer &r, float radius) {
    glm::vec4 c{radius, 619.55f, 1, 8192};
    std::vector<TerrainSample> points;
    const auto add = [&](float distance, float angle) {
        TerrainSample p{};
        p.base = {distance * std::cos(angle), -1.8f, distance * std::sin(angle), 0};
        p.eroded = glm::vec4(glm::normalize(glm::vec3(.3f, 1, -.2f)), 0);
        points.push_back(p);
    };
    // Slow movement through the former branch, in every quadrant. On the old
    // formula, bounded absolute sine error becomes centimetres of radial wobble.
    for (float angle : {.17f, 1.71f, 3.3f, 4.9f}) {
        for (int i = 0; i < 64; ++i)
            add(radius * 1e-6f + (i - 32) / 64.f, angle);
        for (float d : {0.f, .01f, 1.f, 4.f, 8.f, 16.f, 32.f, 64.f, 128.f, 1024.f, 8192.f, 64000.f, 1048576.f,
                        radius * .49999f, radius * .5f, radius * .50001f, radius, radius * 1.5f})
            add(d, angle);
    }
    auto exact = r.sampleCurvatureFixture(points, c, false);
    auto perturbed = r.sampleCurvatureFixture(points, c, true);
    double perturbation = 0;
    for (size_t i = 0; i < points.size(); ++i)
        if (glm::length(glm::vec3(points[i].base)) < 256)
            perturbation = std::max(perturbation,
                                    glm::length(glm::dvec3(exact[i].base) - glm::dvec3(perturbed[i].base)));
    std::cout << "radius=" << radius << " near trig perturbation=" << perturbation << " m\n";
    size_t protectedSamples = 0;
    double maximumNearError = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto p = glm::dvec3(points[i].base);
        const auto expected = curvePosition(p, c);
        const auto normal = referenceNormal(p, glm::dvec3(points[i].eroded), c);
        const double distance = std::hypot(p.x, p.z);
        // A few float operations over metre coordinates; independent CPU bounds
        // already allow 2e-6 of range. Reject much larger trig-induced motion.
        const double tolerance = 5e-6 + glm::length(p) * 5e-7;
        near(glm::length(glm::dvec3(exact[i].base) - expected), 0, tolerance,
             "curved position agrees with double precision");
        near(glm::length(glm::dvec3(exact[i].eroded) - normal), 0, 5e-6,
             "curved normal agrees with independently differentiated surface");
        if (distance < 256)
            maximumNearError = std::max(maximumNearError, glm::length(glm::dvec3(exact[i].base) - expected));
        if (distance < radius * .4999) {
            require(exact[i].base == perturbed[i].base && exact[i].eroded == perturbed[i].eroded,
                    "near curvature must not amplify bounded absolute trig error");
            ++protectedSamples;
        }
    }
    require(protectedSamples > 256, "precision fixture covers the moving near band");
    // Actual per-step displacement must follow the sphere, rather than jumping
    // between quantized radial rings. This also rejects local reversal/folding.
    constexpr size_t stride = 64 + 18;
    for (size_t row = 0; row < 4; ++row)
        for (size_t i = row * stride + 1; i < row * stride + 64; ++i) {
            const auto expected = curvePosition(glm::dvec3(points[i].base), c) -
                                  curvePosition(glm::dvec3(points[i - 1].base), c);
            const auto actual = glm::dvec3(perturbed[i].base - perturbed[i - 1].base);
            near(glm::length(actual - expected), 0, 1e-4, "walking steps stay smooth after curvature");
            require(glm::dot(actual, expected) > 0, "radial order never reverses while walking");
        }
    c.z = 0;
    auto flat = r.sampleCurvatureFixture(points, c, true);
    for (size_t i = 0; i < points.size(); ++i) {
        require(glm::vec3(flat[i].base) == glm::vec3(points[i].base),
                "disabled curvature preserves positions");
        near(glm::length(glm::vec3(flat[i].eroded) - glm::vec3(points[i].eroded)), 0, 1e-6,
             "disabled curvature preserves normals");
    }
    std::cout << "radius=" << radius << " near error=" << maximumNearError << " m\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        RendererOptions o;
        o.shaderDirectory = argc > 1 ? argv[1] : OPENWORLD_SHADER_DIR;
        o.requireValidation = true;
        o.ui = false;
        o.width = o.height = 16;
        o.terrainGrid = {32, .5f};
        o.terrainCapacity = 32;
        Renderer r(o);
        for (float radius : {6360000.f, 1e6f, 1e8f})
            precision(r, radius);
        rejects([&] { r.sampleCurvatureFixture({}, {6360000, 0, 1, 0}, false); },
                "empty fixture is rejected");
        rejects(
            [&] { r.sampleCurvatureFixture(std::vector<TerrainSample>(10000), {6360000, 0, 1, 0}, false); },
            "oversize fixture is rejected before scratch access");
        require(r.validationErrors() == 0, "curvature fixture Vulkan validation");
        std::cout << "Curvature precision checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

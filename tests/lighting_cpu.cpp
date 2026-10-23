#include "lighting.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
using namespace ow;
int main() {
    try {
        constexpr float pi = 3.14159265359f;
        for (int i = 0; i <= 1000; ++i) {
            float x = i / 1000.f;
            near(srgbToLinear(linearToSrgb(x)), x, 3e-7, "sRGB round trip");
        }
        near(linearToSrgb(.18f), .46135613, 1e-6, "18 percent linear grey encoding");
        OutputSettings output;
        near(exposure(output), 1.0 / (1.2 * 32768), 1e-11, "Photographic EV100 convention");
        float initial = exposure(output);
        output.compensation = 1;
        near(exposure(output), initial * 2, 1e-10, "One compensation stop doubles exposure");
        output.ev100 += 1;
        near(exposure(output), initial, 1e-10, "One EV cancels one compensation stop");
        output.ev100 = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { exposure(output); }, "Reject invalid exposure");
        near(toneMap(glm::vec3(.18f), ToneMapper::AcesFitted).x, .10559125, 2e-6,
             "ACES fitted middle grey reference");
        // Filament AgX neutral: independently evaluated double precision reference.
        near(toneMap(glm::vec3(.18f), ToneMapper::Agx).x, .215, .003, "AgX neutral middle grey reference");
        for (auto mapper : {ToneMapper::AcesFitted, ToneMapper::Agx}) {
            float previous = 0;
            for (int i = -8; i <= 16; i++) {
                auto v = toneMap(glm::vec3(std::exp2(float(i))), mapper);
                require(v.x >= previous - .0001f && v.x <= 1 && v.x >= 0,
                        "Neutral ramp monotonic and bounded");
                near(v.x, v.y, .0002, "Neutral ramp retains neutrality");
                near(v.y, v.z, .0002, "Neutral ramp retains neutrality");
                previous = v.x;
            }
            for (auto c : {glm::vec3(100, 0, 0), glm::vec3(0, 100, 0), glm::vec3(0, 0, 100)}) {
                auto v = toneMap(c, mapper);
                for (int k = 0; k < 3; k++)
                    require(std::isfinite(v[k]) && v[k] >= 0 && v[k] <= 1, "Saturated highlight finite");
            }
        }
        MaterialSettings material;
        material.baseColour = glm::vec3(.8f);
        for (float metal : {0.f, 1.f})
            for (float rough : {.045f, .15f, .35f, .6f, 1.f}) {
                material.metallic = metal;
                material.roughness = rough;
                for (float nv : {.00001f, .01f, .5f, 1.f}) {
                    auto v = glm::vec3(std::sqrt(1 - nv * nv), nv, 0);
                    auto value =
                        directBrdf(material, {0, 1, 0}, v, glm::normalize(glm::vec3(-.6f, .8f, .35f)));
                    require(std::isfinite(value.x) && value.x >= 0,
                            "Finite BRDF at grazing angles and endpoints");
                }
                if (rough < .35f)
                    continue; // Uniform quadrature cannot resolve the narrow specular
                              // peak.
                glm::vec3 integral(0);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 128; ++x) {
                        float cosine = (y + .5f) / 64, phi = 2 * pi * (x + .5f) / 128,
                              sine = std::sqrt(1 - cosine * cosine);
                        integral += directBrdf(material, {0, 1, 0}, {0, 1, 0},
                                               {sine * std::cos(phi), cosine, sine * std::sin(phi)}) *
                                    (2 * pi / (64 * 128));
                    }
                require(integral.x <= 1.02f, "Integrated reflection does not create energy");
            }
        const auto &bake = environmentBake();
        require(bake.cube.size() == 32766 && bake.brdf.size() == 128 * 128,
                "Complete bounded environment fixture");
        float sourcePeak = 0, filteredPeak = 0;
        for (size_t i = 0; i < bake.cube.size(); i++) {
            auto p = bake.cube[i];
            require(std::isfinite(p.x) && p.x >= 0, "Finite baked radiance");
            if (i < 64 * 64 * 6)
                sourcePeak = std::max(sourcePeak, p.x);
            if (i >= bake.cube.size() - 6)
                filteredPeak = std::max(filteredPeak, p.x);
        }
        require(filteredPeak < sourcePeak * .7f, "Roughness filters the broad reflection lobe");
        for (auto p : bake.brdf)
            require(std::isfinite(p.x) && p.x >= 0 && p.y >= 0 && p.x + p.y < 1.05f,
                    "BRDF lookup energy bound");
        // Independent hemisphere integration catches cube orientation and missing
        // irradiance pi factors.
        for (auto n : {glm::vec3(0, 1, 0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)}) {
            auto t = glm::normalize(glm::cross(n, glm::vec3(0, 0, 1))), b = glm::cross(n, t);
            glm::vec3 reference(0);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 128; x++) {
                    float c = (y + .5f) / 64, s = std::sqrt(1 - c * c), p = 2 * pi * (x + .5f) / 128;
                    reference += environmentRadiance(t * (s * std::cos(p)) + b * (s * std::sin(p)) + n * c) *
                                 c * (2 * pi / (64 * 128));
                }
            require(glm::length(evaluateIrradiance(bake.irradiance, n) - reference) / glm::length(reference) <
                        .07f,
                    "SH irradiance matches independent hemisphere integration");
        }
        std::cout << "Lighting CPU contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

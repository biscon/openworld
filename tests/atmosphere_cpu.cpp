#include "atmosphere.hpp"
#include "test_support.hpp"
#include <iostream>
using namespace ow;
int main() {
    try {
        AtmosphereSettings s;
        validateAtmosphere(s);
        auto key = atmosphereProfileKey(s);
        s.environmentMultiplier = 2;
        require(atmosphereProfileKey(s) == key,
                "Environment multiplier does not invalidate physical profiles");
        s.aerosolDensity = 2;
        require(atmosphereProfileKey(s) != key, "Aerosol density invalidates profiles");
        s.mieG = 1;
        rejects([&] { validateAtmosphere(s); }, "Reject singular phase function");
        s = {};
        s.rayleigh.x = NAN;
        rejects([&] { validateAtmosphere(s); }, "Reject NaN profiles");
        s = {};
        auto identity = atmosphereReference(s, 100, {0, 1, 0}, {0, 1, 0}, 0, 32, 16);
        near(glm::length(identity.transmittance - glm::dvec3(1)), 0, 1e-12,
             "Zero distance transmittance");
        near(glm::length(identity.scattering), 0, 1e-12, "Zero distance scattering");
        auto a = atmosphereReference(s, 100, {0, 1, 0}, {0, 1, 0}, 1000, 128, 128);
        auto b = atmosphereReference(s, 1100, {0, 1, 0}, {0, 1, 0}, 1000, 128, 128);
        auto c = atmosphereReference(s, 100, {0, 1, 0}, {0, 1, 0}, 2000, 256, 128),
             joined = composeTransport(a, b);
        near(glm::length(joined.transmittance - c.transmittance), 0, 1e-8,
             "Contiguous segment extinction");
        near(glm::length(joined.scattering - c.scattering), 0, 1e-8,
             "Contiguous segment scattering");
        require(c.transmittance.b < c.transmittance.r, "Rayleigh RGB extinction");
        s.rayleigh = s.ozone = glm::vec3(0);
        s.mieScattering = s.mieAbsorption = 0;
        auto vacuum = atmosphereReference(s, 100, {1, 0, 0}, {0, 1, 0}, 50000, 32, 16);
        near(glm::length(vacuum.transmittance - glm::dvec3(1)), 0, 1e-12, "Vacuum transmission");
        near(glm::length(vacuum.scattering), 0, 1e-12, "Vacuum radiance");
        std::cout << "Atmosphere CPU checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

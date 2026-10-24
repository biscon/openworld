#include "vegetation.hpp"
#include <iostream>
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("Usage: vegetation_fixture OUTPUT_DIR");
        ow::writeVegetationAssets(argv[1], ow::makeVegetationAssets());
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

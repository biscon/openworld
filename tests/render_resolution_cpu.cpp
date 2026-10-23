#include "render_resolution.hpp"
#include "test_support.hpp"
#include <iostream>

int main() {
    try {
        for (auto output : {ow::RenderExtent{1920, 1080}, {3840, 2160}, {960, 540}}) {
            auto scene = ow::sceneRenderExtent(output.width, output.height, 1080);
            require(scene.width == 1920 && scene.height == 1080,
                    "Same aspect retains scene resolution above and below output size");
        }
        auto wide = ow::sceneRenderExtent(3440, 1440, 1080);
        require(wide.width == 2580 && wide.height == 1080, "Ultrawide fills the output aspect");
        auto odd = ow::sceneRenderExtent(513, 257, 120);
        require(odd.width == 240 && odd.height == 120, "Noninteger width rounds to nearest pixel");
        auto portrait = ow::sceneRenderExtent(900, 1600, 1080);
        require(portrait.width == 608 && portrait.height == 1080, "Portrait width rounds half up");
        auto native = ow::sceneRenderExtent(513, 257, 0);
        require(native.width == 513 && native.height == 257, "Native mode preserves exact output");
        auto bounded = ow::sceneRenderExtent(4000, 1000, 1080, 2048, 1024);
        require(bounded.width == 2048 && bounded.height == 512, "Device limit scales both axes");
        bounded = ow::sceneRenderExtent(1000, 4000, 1080, 2048, 512);
        require(bounded.width == 128 && bounded.height == 512, "Height limit preserves aspect");
        bounded = ow::sceneRenderExtent(16384, 1, 16384);
        require(bounded.width == 16384 && bounded.height == 1, "Extreme wide output stays bounded");
        bounded = ow::sceneRenderExtent(1, 16384, 1);
        require(bounded.width == 1 && bounded.height == 1, "Scene dimensions never become zero");
        rejects([] { ow::sceneRenderExtent(0, 100, 1080); }, "Reject zero output width");
        rejects([] { ow::sceneRenderExtent(100, 0, 1080); }, "Reject zero output height");
        rejects([] { ow::sceneRenderExtent(100, 100, 16385); }, "Reject excessive height");
        rejects([] { ow::sceneRenderExtent(100, 100, 1080, 0, 100); }, "Reject zero device limit");
        std::cout << "Scene resolution CPU contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

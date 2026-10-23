# Third-party sources

Dear ImGui **v1.92.5**, from https://github.com/ocornut/imgui/tree/v1.92.5.
Only core files, the demo, and SDL3/Vulkan backends are included, unmodified.
License: `imgui/LICENSE.txt` (MIT). No network download is needed during configure.

SDL3, Vulkan, GLM, and DXC are supplied by installed packages / the Vulkan SDK.

Erosion shader provenance and MPL/MIT notices are in [erosion/README.md](erosion/README.md).
The HLSL port lives in `shaders/`; no additional library is built.

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

namespace ow {
enum class UiMenu { Terrain, Rendering, View, Diagnostics };
enum class UiWindow : size_t {
    TerrainSettings,
    BaseTerrain,
    TerrainMaterials,
    RenderingSettings,
    Vegetation,
    Lighting,
    Atmosphere,
    Volumetrics,
    Output,
    Camera,
    Background,
    FrameDiagnostics,
    StreamingDiagnostics,
    LodDiagnostics,
    VisibilityDiagnostics,
    ShadowDiagnostics,
    GtaoDiagnostics,
    ClusterDiagnostics,
    MaterialDiagnostics,
    PostDiagnostics,
    AtmosphereDiagnostics,
    VolumetricDiagnostics,
    ImGuiDemo,
    Count
};
constexpr size_t uiWindowCount = size_t(UiWindow::Count);
struct UiWindowInfo {
    const char *id;
    const char *label;
    const char *title; // ### suffix keeps ImGui identity independent of display text.
    UiMenu menu;
    float width, height;
};
extern const std::array<UiWindowInfo, uiWindowCount> uiWindows;

struct UiState {
    std::array<bool, uiWindowCount> open{};
    bool &visible(UiWindow window) { return open[size_t(window)]; }
    bool visible(UiWindow window) const { return open[size_t(window)]; }
};

// Require a current ImGui context. Loading happens before the first NewFrame;
// saving happens after the last frame and before context destruction.
// Missing files are normal. Other failures throw without changing state/files.
void loadUiState(const std::filesystem::path &path, UiState &state);
void saveUiState(const std::filesystem::path &path, const UiState &state);
} // namespace ow

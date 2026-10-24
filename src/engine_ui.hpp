#pragma once

#include "renderer.hpp"
#include "ui_state.hpp"

namespace ow {
struct LightingDemoControls {
    bool enabled = false;
    int lights = 32, layers = 3;
    bool overlap = false, animation = false;
};

struct UiContext {
    Renderer &renderer;
    RendererOptions &options;
    SceneSettings &scene;
    TerrainRequest &terrain;
    Camera &camera;
    LightingDemoControls &lightingDemo;
    const std::string &terrainError;
    const std::string &reviewStatus;
    const std::string &reviewPath;
};

struct UiActions {
    bool vegetationReset = false;
    bool regenerate = false, resetCamera = false;
    bool saveReview = false, loadReview = false;
    bool renderHeightChanged = false, reloadPalette = false;
    bool lightingDemoChanged = false, lightingDemoReanchor = false;
    int fogPreset = -1;
};

// Call within an ImGui frame. beginUiWindow always requires ImGui::End(),
// including when collapsed. Shared with the CPU integration tests.
void drawUiMenu(UiState &state);
bool beginUiWindow(UiWindow window, bool &open);

class EngineUi {
  public:
    UiState state;
    UiActions draw(UiContext &context);

  private:
    void drawSettings(UiWindow window, UiContext &context, UiActions &actions);
    void drawDiagnostics(UiWindow window, UiContext &context, UiActions &actions);
};
} // namespace ow

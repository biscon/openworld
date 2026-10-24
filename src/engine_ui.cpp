#include "engine_ui.hpp"

#include <algorithm>
#include <imgui.h>

namespace ow {
void drawUiMenu(UiState &state) {
    if (!ImGui::BeginMainMenuBar())
        return;
    constexpr const char *menus[] = {"Terrain", "Rendering", "View", "Diagnostics"};
    for (size_t menu = 0; menu < std::size(menus); ++menu) {
        if (!ImGui::BeginMenu(menus[menu]))
            continue;
        for (size_t i = 0; i < uiWindowCount; ++i)
            if (uiWindows[i].menu == UiMenu(menu))
                ImGui::MenuItem(uiWindows[i].label, nullptr, &state.open[i]);
        if (UiMenu(menu) == UiMenu::View) {
            ImGui::Separator();
            if (ImGui::MenuItem("Close All Windows"))
                state.open.fill(false);
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

bool beginUiWindow(UiWindow window, bool &open) {
    const auto &info = uiWindows[size_t(window)];
    // WorkPos is updated on the next frame by ImGui's menu reservation. Also
    // reserve today's menu height so first-frame/restored windows clear it.
    const auto *viewport = ImGui::GetMainViewport();
    const ImVec2 origin(viewport->WorkPos.x,
                        std::max(viewport->WorkPos.y, viewport->Pos.y + ImGui::GetFrameHeight()));
    const ImVec2 available(std::max(1.f, viewport->WorkSize.x),
                           std::max(1.f, viewport->WorkPos.y + viewport->WorkSize.y - origin.y));
    const float offset = float(size_t(window) % 8) * 24.f;
    ImGui::SetNextWindowPos(ImVec2(origin.x + 16 + offset, origin.y + 16 + offset), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(info.width, available.x), std::min(info.height, available.y)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(300.f, available.x), std::min(100.f, available.y)),
                                        available);
    const bool contents = ImGui::Begin(info.title, &open);
    const auto size = ImGui::GetWindowSize();
    const auto position = ImGui::GetWindowPos();
    const ImVec2 corrected(std::clamp(position.x, origin.x, origin.x + std::max(0.f, available.x - size.x)),
                           std::clamp(position.y, origin.y, origin.y + std::max(0.f, available.y - size.y)));
    if (corrected.x != position.x || corrected.y != position.y)
        ImGui::SetWindowPos(corrected);
    return contents && open;
}

UiActions EngineUi::draw(UiContext &context) {
    UiActions actions;
    drawUiMenu(state);
    for (size_t i = 0; i < uiWindowCount; ++i) {
        if (!state.open[i])
            continue;
        const auto window = UiWindow(i);
        const bool contents = beginUiWindow(window, state.open[i]);
        if (contents && window != UiWindow::ImGuiDemo) {
            ImGui::PushItemWidth(std::max(100.f, ImGui::GetContentRegionAvail().x * .48f));
            if (uiWindows[i].menu == UiMenu::Diagnostics)
                drawDiagnostics(window, context, actions);
            else
                drawSettings(window, context, actions);
            ImGui::PopItemWidth();
        }
        ImGui::End();
        // The demo appends to its own window after the same bounds repair.
        if (window == UiWindow::ImGuiDemo && state.open[i])
            ImGui::ShowDemoWindow(&state.open[i]);
    }
    return actions;
}
} // namespace ow

#include "engine_ui.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h> // Locate displayed menu/close-button bounds for input simulation only.
#include <iostream>
#include <nlohmann/json.hpp>

using namespace ow;
namespace {
struct Fixture {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("ow-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UiState state;
    std::array<ImVec2, uiWindowCount> positions{}, sizes{};
    std::array<bool, uiWindowCount> collapsed{};
    Fixture() {
        std::filesystem::create_directories(directory);
        resetContext();
    }
    ~Fixture() {
        ImGui::DestroyContext();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void resetContext() {
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1280, 800};
        io.DeltaTime = 1.f / 60;
        unsigned char *pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        state = {};
    }
    void frame() {
        ImGui::NewFrame();
        drawUiMenu(state);
        for (size_t i = 0; i < uiWindowCount; ++i) {
            if (!state.open[i])
                continue;
            if (beginUiWindow(UiWindow(i), state.open[i]))
                ImGui::TextUnformatted(uiWindows[i].label);
            positions[i] = ImGui::GetWindowPos();
            sizes[i] = ImGui::GetWindowSize();
            collapsed[i] = ImGui::IsWindowCollapsed();
            ImGui::End();
        }
        ImGui::Render();
    }
    void click(ImVec2 position) {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
    }
    void setLayout(UiWindow window, ImVec2 position, ImVec2 size, bool collapse = false) {
        state.visible(window) = true;
        frame();
        ImGui::NewFrame();
        drawUiMenu(state);
        beginUiWindow(window, state.visible(window));
        ImGui::SetWindowPos(position);
        ImGui::SetWindowSize(size);
        ImGui::SetWindowCollapsed(collapse);
        ImGui::End();
        ImGui::Render();
        frame();
    }
};
void failedLoad(const std::filesystem::path &path, UiState &state) {
    const auto before = state.open;
    bool failed = false;
    try {
        loadUiState(path, state);
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed && state.open == before, "Invalid load leaves live visibility unchanged");
}
nlohmann::json read(const std::filesystem::path &path) {
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}
void roundTrip(Fixture &fixture) {
    const auto path = fixture.directory / "ui-state.json";
    loadUiState(path, fixture.state);
    fixture.frame();
    require(std::none_of(fixture.state.open.begin(), fixture.state.open.end(), [](bool v) { return v; }),
            "Missing state starts menu-only");
    size_t length;
    const auto initial = std::string(ImGui::SaveIniSettingsToMemory(&length));
    for (const auto &window : uiWindows)
        require(initial.find(window.id) == std::string::npos, "Menu-only startup creates no engine panels");
    fixture.setLayout(UiWindow::BaseTerrain, {345, 165}, {570, 390}, true);
    fixture.setLayout(UiWindow::ShadowDiagnostics, {65, 200}, {610, 410});
    fixture.state.visible(UiWindow::ShadowDiagnostics) = false;
    fixture.frame();
    saveUiState(path, fixture.state);
    const auto saved = read(path);
    require(saved.size() == 3 && saved.contains("version") && saved.contains("windows") &&
                saved.contains("imgui_layout"),
            "Only UI state is persisted");
    fixture.resetContext();
    loadUiState(path, fixture.state);
    require(fixture.state.visible(UiWindow::BaseTerrain) &&
                !fixture.state.visible(UiWindow::ShadowDiagnostics),
            "Fresh context restores independent open/closed flags");
    fixture.frame();
    const auto base = size_t(UiWindow::BaseTerrain);
    require(fixture.collapsed[base], "Collapsed state survives restart");
    near(fixture.positions[base].x, 345, 1, "Position X survives restart");
    near(fixture.positions[base].y, 165, 1, "Position Y survives restart");
    // Save while the shadow window has never been instantiated in this process.
    saveUiState(path, fixture.state);
    fixture.resetContext();
    loadUiState(path, fixture.state);
    fixture.state.visible(UiWindow::ShadowDiagnostics) = true;
    fixture.frame();
    const auto shadow = size_t(UiWindow::ShadowDiagnostics);
    near(fixture.positions[shadow].x, 65, 1, "Closed panel retains its layout across another restart");
    near(fixture.sizes[shadow].y, 410, 1, "Closed panel retains its size");
    ImGui::NewFrame();
    drawUiMenu(fixture.state);
    beginUiWindow(UiWindow::BaseTerrain, fixture.state.visible(UiWindow::BaseTerrain));
    ImGui::SetWindowCollapsed(false);
    ImGui::End();
    ImGui::Render();
    fixture.frame();
    near(fixture.sizes[base].x, 570, 1, "Collapsed panel retains full width");
    near(fixture.sizes[base].y, 390, 1, "Collapsed panel retains full height");
}
void failures(Fixture &fixture) {
    const auto path = fixture.directory / "ui-state.json";
    auto document = read(path);
    document["windows"]["future_panel"] = {{"unrecognized", true}};
    document["windows"].erase("output");
    std::ofstream(path) << document;
    fixture.resetContext();
    loadUiState(path, fixture.state);
    require(!fixture.state.visible(UiWindow::Output), "Unknown IDs ignored; missing IDs default closed");
    const auto valid = document;
    for (int mode = 0; mode < 5; ++mode) {
        document = valid;
        if (mode == 0)
            document["version"] = 99;
        if (mode == 1)
            document["windows"]["base_terrain"] = "yes";
        if (mode == 2)
            document["imgui_layout"] = 42;
        if (mode == 3)
            document["windows"] = nlohmann::json::array();
        if (mode == 4)
            document["imgui_layout"] = std::string("bad\0layout", 10);
        std::ofstream(path) << document;
        failedLoad(path, fixture.state);
    }
    std::ofstream(path) << "{truncated";
    failedLoad(path, fixture.state);
    std::ofstream(path) << std::string(1024 * 1024 + 1, ' ');
    failedLoad(path, fixture.state);
    saveUiState(path, fixture.state);
    const auto previous = read(path);
    // A too-large native layout must fail before replacing an existing file.
    const std::string large = "[Window][" + std::string(1024 * 1024, 'x') + "]\nPos=1,1\nSize=10,10\n";
    ImGui::LoadIniSettingsFromMemory(large.c_str(), large.size());
    bool failed = false;
    try {
        saveUiState(path, fixture.state);
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed && read(path) == previous, "Oversized save preserves previous document");
    fixture.resetContext();
    const auto blocked = fixture.directory / "blocked.json";
    std::filesystem::create_directory(blocked);
    std::ofstream(blocked / "owned-marker") << "keep";
    failed = false;
    try {
        saveUiState(blocked, fixture.state);
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed && std::filesystem::exists(blocked / "owned-marker"),
            "Failed rename preserves destination");
    for (const auto &entry : std::filesystem::directory_iterator(fixture.directory))
        require(entry.path().filename().string().find(".tmp-") == std::string::npos,
                "Failed save cleans its temporary file");
}
void inputAndBounds(Fixture &fixture) {
    fixture.resetContext();
    fixture.frame();
    fixture.click({30, ImGui::GetFrameHeight() / 2});
    // Locate the actual displayed menu, rather than hard-coding font-dependent row bounds.
    auto *popup = GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
    require(popup && popup->Active, "Terrain menu opens through input");
    const ImVec2 firstItem(popup->Pos.x + popup->WindowPadding.x + 30,
                           popup->Pos.y + popup->WindowPadding.y + ImGui::GetFontSize() / 2);
    fixture.click(firstItem);
    require(fixture.state.visible(UiWindow::TerrainSettings), "Menu item opens its panel");
    fixture.click({30, ImGui::GetFrameHeight() / 2});
    fixture.click(firstItem);
    require(!fixture.state.visible(UiWindow::TerrainSettings), "Checked menu item closes its panel");
    fixture.setLayout(UiWindow::TerrainSettings, {85, 95}, {450, 300});
    const auto *window = ImGui::FindWindowByName(uiWindows[size_t(UiWindow::TerrainSettings)].title);
    const auto title = window->TitleBarRect();
    fixture.click(
        {title.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::GetFontSize() / 2, title.GetCenter().y});
    require(!fixture.state.visible(UiWindow::TerrainSettings), "Title close button updates menu visibility");
    fixture.setLayout(UiWindow::BaseTerrain, {900, 450}, {370, 330});
    ImGui::GetIO().DisplaySize = {320, 180};
    fixture.frame();
    const auto base = size_t(UiWindow::BaseTerrain);
    require(fixture.positions[base].x >= 0 && fixture.positions[base].y >= ImGui::GetFrameHeight(),
            "Restored panel clears menu bar");
    require(fixture.positions[base].x + fixture.sizes[base].x <= 321 &&
                fixture.positions[base].y + fixture.sizes[base].y <= 181,
            "Panel fits reduced viewport");
    ImGui::GetIO().DisplaySize = {1280, 800};
    fixture.frame();
}
} // namespace
int main() {
    try {
        Fixture fixture;
        roundTrip(fixture);
        failures(fixture);
        inputAndBounds(fixture);
        std::cout << "UI state and interaction tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

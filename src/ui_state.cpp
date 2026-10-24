#include "ui_state.hpp"

#include <chrono>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace ow {
const std::array<UiWindowInfo, uiWindowCount> uiWindows{{
    {"terrain_settings", "Terrain Settings", "Terrain Settings###terrain_settings", UiMenu::Terrain, 450,
     300},
    {"base_terrain", "Base Terrain Generation", "Base Terrain Generation###base_terrain", UiMenu::Terrain,
     450, 300},
    {"terrain_materials", "Terrain Materials", "Terrain Materials###terrain_materials", UiMenu::Terrain, 460,
     550},
    {"rendering_settings", "Rendering Settings", "Rendering Settings###rendering_settings", UiMenu::Rendering,
     460, 590},
    {"vegetation", "Vegetation", "Vegetation###vegetation", UiMenu::Rendering, 480, 470},
    {"lighting", "Lighting & Materials", "Lighting & Materials###lighting", UiMenu::Rendering, 460, 520},
    {"atmosphere", "Atmosphere", "Atmosphere###atmosphere", UiMenu::Rendering, 440, 310},
    {"volumetrics", "Local Volumetrics", "Local Volumetrics###volumetrics", UiMenu::Rendering, 520, 590},
    {"output", "Output", "Output###output", UiMenu::Rendering, 440, 340},
    {"camera", "Camera & Review", "Camera & Review###camera", UiMenu::View, 470, 300},
    {"background", "Background", "Background###background", UiMenu::View, 400, 150},
    {"frame_diagnostics", "Frame & Device", "Frame & Device###frame_diagnostics", UiMenu::Diagnostics, 560,
     310},
    {"streaming_diagnostics", "Terrain Streaming & Generation",
     "Terrain Streaming & Generation###streaming_diagnostics", UiMenu::Diagnostics, 610, 620},
    {"lod_diagnostics", "Terrain LOD", "Terrain LOD###lod_diagnostics", UiMenu::Diagnostics, 560, 570},
    {"visibility_diagnostics", "Rendering & Visibility", "Rendering & Visibility###visibility_diagnostics",
     UiMenu::Diagnostics, 620, 350},
    {"shadow_diagnostics", "Shadows", "Shadows###shadow_diagnostics", UiMenu::Diagnostics, 590, 370},
    {"gtao_diagnostics", "GTAO", "GTAO###gtao_diagnostics", UiMenu::Diagnostics, 550, 200},
    {"cluster_diagnostics", "Lighting & Clusters", "Lighting & Clusters###cluster_diagnostics",
     UiMenu::Diagnostics, 580, 400},
    {"material_diagnostics", "Terrain Materials", "Terrain Material Diagnostics###material_diagnostics",
     UiMenu::Diagnostics, 490, 320},
    {"post_diagnostics", "Post-processing", "Post-processing###post_diagnostics", UiMenu::Diagnostics, 650,
     230},
    {"atmosphere_diagnostics", "Atmosphere & Horizon", "Atmosphere & Horizon###atmosphere_diagnostics",
     UiMenu::Diagnostics, 540, 250},
    {"volumetric_diagnostics", "Volumetrics", "Volumetrics###volumetric_diagnostics", UiMenu::Diagnostics,
     700, 240},
    {"imgui_demo", "ImGui Demo", "Dear ImGui Demo", UiMenu::Diagnostics, 550, 650},
}};

namespace {
constexpr size_t maximumFileBytes = 1024 * 1024;
using nlohmann::json;
} // namespace

void loadUiState(const std::filesystem::path &path, UiState &state) {
    if (!std::filesystem::exists(path))
        return;
    const auto bytes = std::filesystem::file_size(path);
    if (bytes > maximumFileBytes)
        throw std::runtime_error("UI state exceeds 1 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open UI state: " + path.string());
    // Bound the actual read as well, in case the file changes after file_size().
    std::string data(size_t(bytes), '\0');
    input.read(data.data(), std::streamsize(data.size()));
    if (input.gcount() != std::streamsize(data.size()) || input.peek() != EOF)
        throw std::runtime_error("UI state changed while reading");
    const auto document = json::parse(data);
    if (!document.is_object() || !document.contains("version") || !document["version"].is_number_integer() ||
        document["version"] != 1)
        throw std::runtime_error("Unsupported UI state version");
    const auto &windows = document.at("windows");
    const auto layout = document.at("imgui_layout").get<std::string>();
    if (!windows.is_object() || layout.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid UI state");
    UiState loaded;
    for (size_t i = 0; i < uiWindowCount; ++i) {
        const auto entry = windows.find(uiWindows[i].id);
        if (entry != windows.end())
            loaded.open[i] = entry->get<bool>();
    }
    ImGui::LoadIniSettingsFromMemory(layout.c_str(), layout.size());
    state = loaded;
}

void saveUiState(const std::filesystem::path &path, const UiState &state) {
    json windows = json::object();
    for (size_t i = 0; i < uiWindowCount; ++i)
        windows[uiWindows[i].id] = state.open[i];
    size_t length = 0;
    const char *layout = ImGui::SaveIniSettingsToMemory(&length);
    const auto data =
        json{{"version", 1}, {"windows", windows}, {"imgui_layout", std::string(layout, length)}}.dump(2) +
        '\n';
    if (data.size() > maximumFileBytes)
        throw std::runtime_error("UI state exceeds 1 MiB");
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(data.data(), std::streamsize(data.size()));
        output.close();
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace ow

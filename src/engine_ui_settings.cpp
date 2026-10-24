#include "engine_ui.hpp"
#include <cmath>
#include <cstdio>
#include <imgui.h>

namespace ow {
void EngineUi::drawSettings(UiWindow window, UiContext &context, UiActions &actions) {
    auto &renderer = context.renderer;
    auto &options = context.options;
    auto &scene = context.scene;
    auto &terrain = context.terrain;
    auto &camera = context.camera;
    const auto &terrainError = context.terrainError;
    switch (window) {
    case UiWindow::TerrainSettings: {
        ImGui::Text("Chunk size: %.1f m (startup setting)", options.terrainGrid.extent());
        if (!terrainError.empty())
            ImGui::TextWrapped("%s (terrain unchanged)", terrainError.c_str());
        if (options.streaming.enabled)
            ImGui::SliderInt("View radius (base chunks)", &scene.viewRadius, 0,
                             ow::terrainMaximumRadius(options.terrainGrid));
        ImGui::SliderFloat("Erosion wavelength (m)", &terrain.settings.erosionWavelength, 64, 8192, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Erosion strength", &terrain.settings.erosionStrength, 0, 1, "%.3f",
                           ImGuiSliderFlags_AlwaysClamp);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Crease rounding", &terrain.settings.creaseRounding, 0, 1, "%.3f",
                           ImGuiSliderFlags_AlwaysClamp);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::Checkbox("Show eroded terrain", &scene.eroded);
        ImGui::TextWrapped("Terrain edits apply on release. Base/eroded comparison "
                           "uses cached heights and normals.");
        break;
    }
    case UiWindow::BaseTerrain: {
        if (!terrainError.empty())
            ImGui::TextWrapped("%s (terrain unchanged)", terrainError.c_str());

        ImGui::InputScalar("Seed", ImGuiDataType_U32, &terrain.settings.seed);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Regional wavelength (m)", &terrain.settings.regionWavelength, 4096, 65536, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Mountain wavelength (m)", &terrain.settings.wavelength, 64, 8192, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Height scale (m)", &terrain.settings.heightScale, 0, 2000, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Ridge strength", &terrain.settings.ridgeStrength, 0, 1, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0: rounded hills. 1: ridged mountains. Changes "
                              "the base before erosion.");
        ImGui::SliderFloat("Base detail", &terrain.settings.baseDetail, 0, 1, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp);
        actions.regenerate |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0: broad, smooth landforms. 1: full smaller-scale detail.\n"
                              "Set both sliders to 1 for the original terrain shape.");

        ImGui::TextWrapped("Terrain edits apply on release. Use Show eroded "
                           "terrain in Terrain Settings to compare.");
        break;
    }
    case UiWindow::Vegetation: {
        if (!options.vegetation.streaming.enabled) {
            ImGui::TextWrapped("Streamed vegetation requires hierarchical terrain.");
            break;
        }
        auto &settings = scene.vegetation;
        ImGui::Checkbox("Enable vegetation", &settings.enabled);
        ImGui::SliderFloat("Render distance (m)", &settings.distance, 20, 200, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Density", &settings.density, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        int preset = settings.habitat.weights.z > .5f ? 2 : settings.habitat.weights.y > .5f ? 1 : 0;
        if (ImGui::Combo("Habitat preset", &preset, "Meadow\0Moor\0Understory\0"))
            settings.habitat = vegetationHabitat(VegetationHabitat(preset));
        ImGui::SliderFloat("Wind strength", &settings.wind, 0, 2, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        float angle = std::atan2(settings.windDirection.y, settings.windDirection.x);
        if (ImGui::SliderAngle("Wind direction", &angle, -180, 180))
            settings.windDirection = {std::cos(angle), std::sin(angle)};
        ImGui::Checkbox("Foliage transmission", &settings.transmission);
        ImGui::Checkbox("Vegetation occlusion (Hi-Z)", &settings.occlusion);
        ImGui::TextWrapped("Range increases stream in progressively. Nearby density is preserved; "
                           "distant plants use cheaper detail and stable thinning.");
        if (ImGui::CollapsingHeader("Diagnostics")) {
            const auto &v = renderer.vegetationStats();
            ImGui::Text("Cells: %u ready, %u pending, %u retiring", v.residentCells, v.pendingCells,
                        v.retiringCells);
            ImGui::Text("Updates: %u (%u new), missing terrain: %u", v.updatedCells, v.generatedCells,
                        v.missingTerrainCells);
            ImGui::Text("Capacity-deferred cells: %u; overflow: %u", v.deferredCells, v.overflow);
            ImGui::Text("Visible entries: %u; casters: %u", v.visible, v.casters);
            ImGui::Text("GPU: %.3f ms select, %.3f depth, %.3f colour", v.selectionMilliseconds,
                        v.depthMilliseconds, v.colourMilliseconds);
            ImGui::Text("GPU: %.3f ms shadows, %.3f updates", v.shadowMilliseconds, v.updateMilliseconds);
            ImGui::Text("%.1f MiB allocated; CPU %.3f ms", double(v.allocatedBytes) / (1024 * 1024),
                        v.preparationMilliseconds);
        }
        break;
    }
    case UiWindow::RenderingSettings: {
        ImGui::SeparatorText("Geometry & visibility");
        char renderHeightLabel[32];
        std::snprintf(renderHeightLabel, sizeof(renderHeightLabel), "%u pixels", options.renderHeight);
        if (ImGui::BeginCombo("Render height", renderHeightLabel)) {
            for (uint32_t height : {720u, 900u, 1080u, 1440u, 2160u}) {
                char label[32];
                std::snprintf(label, sizeof(label), "%u pixels", height);
                if (ImGui::Selectable(label, options.renderHeight == height)) {
                    options.renderHeight = height;
                    actions.renderHeightChanged = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Terrain LOD", &scene.lod);
        ImGui::SameLine();
        ImGui::Checkbox("Frustum culling", &scene.frustumCulling);
        ImGui::Checkbox("Opaque depth prepass", &scene.opaqueDepthPrepass);
        if (scene.visibility == ow::VisibilityMode::HiZ ||
            renderer.gtaoStats().effective != ow::GtaoMode::Off)
            ImGui::TextWrapped("Hi-Z / GTAO requires opaque depth prepass");
        int visibilityMode = int(scene.visibility);
        if (ImGui::Combo("Visibility", &visibilityMode, "Direct\0Indirect\0Hi-Z\0"))
            scene.visibility = ow::VisibilityMode(visibilityMode);
        ImGui::Checkbox("Additional error refinement", &scene.terrainLodQuality);
        ImGui::SliderFloat("LOD error (pixels)", &scene.terrainLodPixelError, .5f, 8.f, "%.1f");
        ImGui::SeparatorText("Sun shadows");
        int shadowMode = int(scene.shadows.mode);
        if (ImGui::Combo("Sun shadows", &shadowMode, "Off\0CSM\0"))
            scene.shadows.mode = ow::ShadowMode(shadowMode);
        ImGui::SliderFloat("Near shadows (m)", &scene.shadows.nearDistance, 100, 4000);
        ImGui::SliderFloat("Far shadows (0 = view)", &scene.shadows.farDistance, 0, 16000);
        ImGui::SliderFloat("Shadow bias (texels)", &scene.shadows.biasTexels, 0, 4);
        ImGui::SliderFloat("Normal bias (texels)", &scene.shadows.normalBiasTexels, 0, 2);
        ImGui::SeparatorText("Ambient occlusion");
        int aoMode = int(scene.gtao.mode);
        if (ImGui::Combo("GTAO", &aoMode, "Off\0Half\0Full\0"))
            scene.gtao.mode = ow::GtaoMode(aoMode);
        ImGui::SliderFloat("AO radius (m)", &scene.gtao.radius, .1f, 5.f, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp);

        break;
    }
    case UiWindow::Lighting: {
        int localMode = int(scene.localLights);
        if (ImGui::Combo("Local-light path", &localMode, "Disabled\0Brute force\0Clustered\0"))
            scene.localLights = ow::LocalLightMode(localMode);
        if (ImGui::CollapsingHeader("Sun & environment", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Sun", &scene.lighting.sunEnabled);
            ImGui::SliderFloat("Illuminance (lux)", &scene.lighting.sunLux, 0, 200000, "%.0f");
            ImGui::ColorEdit3("Sun colour", &scene.lighting.sunColour.x);
            ImGui::SliderFloat3("Direction to sun", &scene.lighting.sunDirection.x, -1, 1);
            ImGui::Checkbox("Environment", &scene.lighting.environmentEnabled);
            if (scene.atmosphere.mode == ow::AtmosphereMode::Physical)
                ImGui::SliderFloat("Sky lighting multiplier", &scene.atmosphere.environmentMultiplier, 0, 3);
            else {
                ImGui::SliderFloat("Brightness (nits)", &scene.lighting.environmentNits, 0, 20000, "%.0f");
                ImGui::SliderAngle("Environment rotation", &scene.lighting.environmentRotation, -180, 180);
            }
        }
        if (ImGui::CollapsingHeader("Constant material", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto &material = scene.material;
            ImGui::ColorEdit3("Base colour (linear)", &material.baseColour.x);
            ImGui::SliderFloat("Roughness", &material.roughness, .045f, 1);
            ImGui::SliderFloat("Metallic", &material.metallic, 0, 1);
            ImGui::SliderFloat("Ambient occlusion", &material.ao, 0, 1);
            ImGui::DragFloat3("Emissive (nits)", &material.emissive.x, 100, 0, 1000000, "%.0f");
        }
        break;
    }
    case UiWindow::Atmosphere: {
        int mode = int(scene.atmosphere.mode), quality = int(scene.atmosphere.quality);
        if (ImGui::Combo("Sky", &mode, "Off\0Analytic\0Physical\0"))
            scene.atmosphere.mode = ow::AtmosphereMode(mode);
        if (ImGui::Combo("Atmosphere quality", &quality, "Normal\0High\0"))
            scene.atmosphere.quality = ow::AtmosphereQuality(quality);
        ImGui::Checkbox("Aerial perspective", &scene.atmosphere.aerialPerspective);
        if (ImGui::Button("Clear air"))
            scene.atmosphere.aerosolDensity = 1;
        ImGui::SameLine();
        if (ImGui::Button("Hazy air"))
            scene.atmosphere.aerosolDensity = 4;
        ImGui::SliderFloat("Aerosol density", &scene.atmosphere.aerosolDensity, 0, 10);
        ImGui::SliderFloat("Boundary haze (artistic)", &scene.atmosphere.boundaryHaze, 0, 3);
        break;
    }
    case UiWindow::Volumetrics: {
        auto &fog = scene.volumetrics;
        ImGui::Checkbox("Enable local fog", &fog.enabled);
        if (ImGui::Button("Mist preset"))
            actions.fogPreset = 1;
        ImGui::SameLine();
        if (ImGui::Button("Shafts preset"))
            actions.fogPreset = 2;
        ImGui::SameLine();
        if (ImGui::Button("Clear fog fixtures"))
            actions.fogPreset = 0;
        int quality = int(fog.quality), samples = int(fog.samples);
        if (ImGui::Combo("Fog quality", &quality, "Normal\0High\0"))
            fog.quality = ow::VolumetricQuality(quality);
        ImGui::SliderFloat("Fog distance (m)", &fog.distance, 50, 4000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Depth covered by the fog grid, not fog thickness.\n"
                              "Shorter distances give finer depth detail.");
        ImGui::TextUnformatted("Height fog (world-wide layer)");
        ImGui::SliderFloat("Base density (/m)", &fog.extinction, 0, .02f, "%.5f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extinction at the reference altitude. Higher = thicker.\n"
                              "0 disables the height layer. Local volumes are separate.\n"
                              "At 0.001/m, 37%% of light remains after 1000 m at that altitude.");
        ImGui::SliderFloat("Reference altitude (m)", &fog.height, -1000, 10000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Altitude where Base density applies, not a top boundary.\n"
                              "Density increases below it and decreases above it.");
        ImGui::SliderFloat("Vertical falloff (m)", &fog.scaleHeight, 1, 500);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rise this far and density falls to 37%%.\n"
                              "Larger = a deeper layer; smaller = a sharper height transition.");
        ImGui::ColorEdit3("Height-fog albedo", &fog.albedo.x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fraction of removed light scattered instead of absorbed.\n"
                              "This changes brightness/colour, not extinction.");
        ImGui::SliderFloat("Local volume density", &fog.localDensity, 0, 4, "%.2fx");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Thickness multiplier for all authored boxes/spheres, including "
                              "presets.\n"
                              "0 disables local volumes; height fog is unaffected.\n"
                              "At 1x the preset box is 0.025/m (mist) or 0.035/m (shafts).");
        ImGui::SliderFloat("Fog anisotropy", &fog.anisotropy, -.9f, .9f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scattering direction for height fog and local volumes.\n"
                              "Positive values brighten fog when looking toward the light.\n"
                              "0 scatters equally in all directions; this is not density.");
        if (ImGui::SliderInt("Samples per cell", &samples, 1, 2))
            fog.samples = uint32_t(samples);
        ImGui::Checkbox("Fog temporal history", &fog.temporal);
        ImGui::SliderFloat("Fog history weight", &fog.historyWeight, 0, .9f);
        break;
    }
    case UiWindow::Output: {
        ImGui::Checkbox("Temporal antialiasing", &scene.taa.enabled);
        ImGui::Checkbox("Thin geometry stability", &scene.taa.thinGeometry);
        ImGui::SliderFloat("Bloom intensity", &scene.bloom.intensity, 0, .5f);
        ImGui::SliderFloat("Bloom threshold", &scene.bloom.threshold, 0, 100000, "%.0f");
        ImGui::SliderFloat("Bloom knee", &scene.bloom.knee, 0, 1);
        ImGui::SliderFloat("Bloom radius", &scene.bloom.radius, .5f, 2);
        ImGui::SliderFloat("EV100", &scene.output.ev100, 0, 24);
        ImGui::SliderFloat("Compensation (stops)", &scene.output.compensation, -10, 10);
        int mapper = int(scene.output.toneMapper);
        if (ImGui::Combo("Tone mapper", &mapper, "ACES fitted\0AgX neutral\0"))
            scene.output.toneMapper = ow::ToneMapper(mapper);
        break;
    }
    case UiWindow::TerrainMaterials: {
        auto &m = scene.terrainMaterial;
        if (ImGui::Button("Baseline"))
            ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::Baseline);
        ImGui::SameLine();
        if (ImGui::Button("Balanced"))
            ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::Balanced);
        ImGui::SameLine();
        if (ImGui::Button("High"))
            ow::applyMaterialPreset(m, ow::TerrainMaterialPreset::High);
        ImGui::Checkbox("Textured terrain", &m.enabled);
        if (scene.shading == ow::ShadingMode::Legacy || scene.normalView || scene.lodColours ||
            scene.terrainLodErrorView || scene.wireframe || scene.chunkBorders)
            ImGui::TextWrapped("Terrain diagnostics currently override material shading.");
        ImGui::Checkbox("Height blend", &m.heightBlend);
        ImGui::SliderFloat("Height influence", &m.heightInfluence, 0, 1);
        ImGui::SliderFloat("Blend width", &m.blendWidth, .01f, 1);
        ImGui::Checkbox("Cliff projection", &m.cliffs);
        ImGui::Checkbox("Prune weak axis (comparison)", &m.twoAxis);
        ImGui::Checkbox("Macro variation", &m.macro);
        ImGui::SliderFloat("Macro strength", &m.macroStrength, 0, 1);
        ImGui::SliderFloat("Macro repeat (m)", &m.macroRepeatMetres, 16, 1024, "%.0f",
                           ImGuiSliderFlags_Logarithmic);

        ImGui::SliderFloat("Repeat (metres)", &m.repeatMetres, .25f, 32.f, "%.2f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Normal strength", &m.normalStrength, 0, 2);
        ImGui::SliderFloat("Roughness multiplier", &m.roughness, .1f, 2);
        ImGui::ColorEdit3("Tint", &m.tint.x);
        ImGui::Combo("Micro filtering", &m.filtering, "Trilinear\0Anisotropic 8x\0Anisotropic 16x\0");
        if (ImGui::Button("Reload palette"))
            actions.reloadPalette = true;
        if (!renderer.materialStats().error.empty())
            ImGui::TextWrapped("%s", renderer.materialStats().error.c_str());
        break;
    }
    case UiWindow::Camera: {
        ImGui::SliderFloat("Flight speed (m/s)", &camera.speed, 1, 1000, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        actions.resetCamera |= ImGui::Button("Reset camera");
        ImGui::Separator();
        actions.saveReview |= ImGui::Button("Save review view");
        ImGui::SameLine();
        actions.loadReview |= ImGui::Button("Load review view");
        ImGui::TextWrapped("Bookmark: %s", context.reviewPath.c_str());
        if (!context.reviewStatus.empty())
            ImGui::TextWrapped("%s", context.reviewStatus.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("Hold RMB: look + WASD/QE fly. Shift: boost. Escape: "
                           "release / quit. F10: fullscreen.");
        break;
    }
    case UiWindow::Background: {
        ImGui::ColorEdit3("Background", scene.background);
        break;
    }
    default:
        break;
    }
}
} // namespace ow

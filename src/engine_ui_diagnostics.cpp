#include "engine_ui.hpp"
#include <algorithm>
#include <imgui.h>

namespace ow {
void EngineUi::drawDiagnostics(UiWindow window, UiContext &context, UiActions &actions) {
    auto &renderer = context.renderer;
    auto &options = context.options;
    auto &scene = context.scene;
    auto &terrain = context.terrain;
    auto &camera = context.camera;
    switch (window) {
    case UiWindow::FrameDiagnostics: {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1), "VULKAN / TERRAIN");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", renderer.deviceName().c_str());
        ImGui::TextWrapped("Scene: %u x %u | Output: %u x %u", renderer.renderWidth(),
                           renderer.renderHeight(), renderer.width(), renderer.height());
        ImGui::TextWrapped("%.1f FPS | F10: fullscreen", ImGui::GetIO().Framerate);
        ImGui::TextWrapped("Validation: %s", renderer.validationEnabled() ? "enabled" : "disabled");
        ImGui::TextWrapped("Validation errors: %u", renderer.validationErrors());
        const auto &stats = renderer.renderingStats();
        ImGui::TextWrapped("HDR: %.2f MiB | Lighting: %.2f MiB", stats.hdrBytes / 1048576.0,
                           stats.lightingBytes / 1048576.0);
        if (stats.timingsAvailable)
            ImGui::TextWrapped("Scene: %.2f ms | Output + UI: %.2f ms", stats.sceneMilliseconds,
                               stats.outputMilliseconds);
        if (stats.timingsAvailable)
            ImGui::TextWrapped("Full GPU frame: %.2f ms", stats.frameMilliseconds);
        break;
    }
    case UiWindow::StreamingDiagnostics: {
        if (!options.streaming.enabled)
            ImGui::TextWrapped("Centre: (%lld, %lld) | %u chunks", static_cast<long long>(terrain.chunk.x),
                               static_cast<long long>(terrain.chunk.z), renderer.terrainChunkCount());
        if (options.streaming.enabled) {
            const auto &s = renderer.terrainStreamingStats();
            ImGui::TextWrapped("Radius: %d requested / %d effective (max %d)", scene.viewRadius,
                               std::min(scene.viewRadius, s.maximumRadius), s.maximumRadius);
            ImGui::TextWrapped("View: %.2f km requested / %.2f km effective",
                               scene.viewRadius * options.terrainGrid.extent() / 1000,
                               s.effectiveRadius * options.terrainGrid.extent() / 1000);
            ImGui::TextWrapped("Resident: %u coverage / %u detail", s.coverage, s.detail);
            ImGui::TextWrapped("Detail demand: %u eligible / %u selected / %u ready", s.detailEligible,
                               s.detailSelected, s.detailReady);
            for (uint32_t l = 0; l < s.residentLevels.size(); ++l)
                if (s.residentLevels[l])
                    ImGui::TextWrapped("Tile L%u: %u resident", l, s.residentLevels[l]);
            ImGui::TextWrapped("Pool deferrals: %llu", (unsigned long long)s.pressureEvents);
            ImGui::TextWrapped("Pending: %u coverage / %u detail / %u I/O", s.pending, s.detailPending,
                               s.ioJobs);
            ImGui::TextWrapped("View footprint: %.0f m square",
                               options.terrainGrid.extent() *
                                   (2 * std::min(scene.viewRadius, s.maximumRadius) + 1));
            ImGui::TextWrapped("Cache: %llu hits / %llu misses / %llu errors", (unsigned long long)s.hits,
                               (unsigned long long)s.misses, (unsigned long long)s.errors);
            ImGui::TextWrapped("Host working storage: %.1f MiB", s.hostBytes / double(ow::terrainMiB));
            ImGui::TextWrapped("Generation recorded: %u batches / %u stages", s.dispatches,
                               s.generationSegments);
            ImGui::TextWrapped("Estimated: %.2f / %.2f ms; %s", s.generationEstimatedMilliseconds,
                               ow::TerrainGenerationBudget::targetMilliseconds,
                               ow::terrainGenerationStopName(s.generationStop));
            ImGui::TextWrapped("Completed GPU: %.2f ms / %u groups / %u stages", s.generationMilliseconds,
                               s.completedGenerationGroups, s.completedGenerationSegments);
            ImGui::TextWrapped("Transfer: %.2f MiB", s.transferBytes / double(ow::terrainMiB));
            ImGui::TextWrapped("Detail cancel/preempt: %llu / %llu; stale reads: %llu",
                               (unsigned long long)s.detailCancellations,
                               (unsigned long long)s.detailPreemptions, (unsigned long long)s.discardedReads);
            if (s.activeDetail || s.detailRetiring)
                ImGui::TextWrapped("Detail (%lld,%lld L%u): %.0f%% %s", (long long)s.activeDetailTile.x,
                                   (long long)s.activeDetailTile.z, s.activeDetailTile.level,
                                   s.activeDetailProgress * 100, s.detailRetiring ? "retiring" : "working");
            ImGui::TextWrapped("Recorded groups: %u / %u hard limit", s.generationGroups,
                               s.generationGroupBudget);
            ImGui::TextWrapped("Terrain GPU: %.2f ms", s.terrainMilliseconds);
        }
        ImGui::TextWrapped("Terrain allocation: %.2f MiB", renderer.terrainAllocatedBytes() / 1048576.0);
        ImGui::TextWrapped("Chunk generations: %llu",
                           static_cast<unsigned long long>(renderer.terrainGenerationCount()));
        break;
    }
    case UiWindow::LodDiagnostics: {
        ImGui::Checkbox("Filtered terrain normals", &scene.filteredNormals);
        ImGui::Checkbox("Normal colours", &scene.normalView);
        ImGui::SameLine();

        ImGui::Checkbox("Chunk borders", &scene.chunkBorders);
        ImGui::SameLine();
        ImGui::Checkbox("LOD colours", &scene.lodColours);
        ImGui::Checkbox("Freeze LOD camera", &scene.freezeLod);
        ImGui::Checkbox("LOD error colours", &scene.terrainLodErrorView);
        ImGui::TextWrapped("Camera chunk: (%lld, %lld)", static_cast<long long>(camera.chunk.x),
                           static_cast<long long>(camera.chunk.z));
        ImGui::TextWrapped("Mesh: %.2f m | Normals: %.2f m", options.terrainGrid.spacing,
                           options.terrainGrid.spacing / options.normalSubdivisions);
        ImGui::TextWrapped("%u x %u cells/chunk", options.terrainGrid.cells, options.terrainGrid.cells);
        const auto &stats = renderer.terrainRenderStats();
        ImGui::TextWrapped("CPU selected: %llu triangles | %u patches",
                           static_cast<unsigned long long>(stats.triangles), stats.selected);
        ImGui::TextWrapped("Culled: %u | Bounds: %s", stats.culled, stats.boundsReady ? "ready" : "pending");
        ImGui::TextWrapped("LOD first range: %.1f m", stats.firstRange);
        ImGui::TextWrapped("LOD error: %.1f px | %u over target", stats.maximumErrorPixels,
                           stats.overTargetPatches);
        if (options.streaming.hierarchical) {
            ImGui::TextWrapped("Replacement: %.2f px | refined %u", stats.maximumTransitionPixels,
                               stats.refined);
            ImGui::TextWrapped("Ground: %u patches | %u limited", stats.protectedPatches,
                               stats.protectedMissing);
            ImGui::TextWrapped("Budget: %llu tris | %u deferred", (unsigned long long)stats.triangleBudget,
                               stats.budgetLimited);
            ImGui::TextWrapped("Residency limited: %u", stats.residencyLimited);
        }
        for (uint32_t level = 0; level < stats.perLevel.size(); ++level)
            if (stats.perLevel[level])
                ImGui::TextWrapped("LOD %u: %u patches | %llu triangles (%.1f m)", level,
                                   stats.perLevel[level], (unsigned long long)stats.trianglesPerLevel[level],
                                   stats.firstRange * float(1u << level));
        const auto &timings = renderer.renderingStats();
        ImGui::TextWrapped("Terrain CPU: select %.2f | prep %.2f | final %.2f ms",
                           timings.terrainSelectionMilliseconds, timings.terrainPreparationMilliseconds,
                           timings.terrainFinalizeMilliseconds);
        break;
    }
    case UiWindow::VisibilityDiagnostics: {
        ImGui::Checkbox("Wireframe", &scene.wireframe);
        int shading = int(scene.shading);
        if (ImGui::Combo("Shading", &shading, "PBR\0Legacy diagnostic\0"))
            scene.shading = ow::ShadingMode(shading);
        if (scene.shading == ow::ShadingMode::Legacy ||
            (scene.terrainVisible && (scene.normalView || scene.wireframe || scene.chunkBorders ||
                                      scene.lodColours || scene.terrainLodErrorView)))
            ImGui::TextWrapped("Diagnostic shading: exposure and tone mapping bypassed.");
        const auto &visibility = renderer.visibilityStats();
        const char *modes[] = {"Direct", "Indirect", "Hi-Z"};
        const char *fallbacks[] = {"None", "Indirect unsupported", "Hi-Z unsupported", "Candidate capacity",
                                   "Wireframe"};
        ImGui::TextWrapped("Visibility: %s | Fallback: %s", modes[uint32_t(visibility.effective)],
                           fallbacks[uint32_t(visibility.fallback)]);
        ImGui::TextWrapped("Candidates: %u | Batches: %u | %.2f MiB", visibility.candidates,
                           visibility.batches, visibility.allocatedBytes / 1048576.0);
        if (visibility.countsAvailable) {
            ImGui::TextWrapped("Completed frame %llu (%s): %u drawn | %u frustum | %u occluded",
                               (unsigned long long)visibility.completedFrame,
                               modes[uint32_t(visibility.completedMode)], visibility.submitted,
                               visibility.frustumRejected, visibility.occlusionRejected);
            ImGui::TextWrapped("Depth triangles: %llu terrain / %llu mesh",
                               (unsigned long long)visibility.depthTriangles.terrain,
                               (unsigned long long)visibility.depthTriangles.meshes);
            ImGui::TextWrapped("Opaque colour triangles: %llu terrain / %llu mesh",
                               (unsigned long long)visibility.colourTriangles.terrain,
                               (unsigned long long)visibility.colourTriangles.meshes);
        }
        if (visibility.timingsAvailable)
            ImGui::TextWrapped("Depth %.2f | Pyramid %.2f | Cull %.2f | Colour %.2f ms",
                               visibility.depthMilliseconds, visibility.pyramidMilliseconds,
                               visibility.cullingMilliseconds, visibility.colourMilliseconds);
        break;
    }
    case UiWindow::ShadowDiagnostics: {
        int shadowView = int(scene.shadows.diagnostic);
        if (ImGui::Combo("Shadow view", &shadowView, "Shaded\0Visibility\0Cascades\0Coverage\0"))
            scene.shadows.diagnostic = uint32_t(shadowView);
        const auto &shadowStats = renderer.shadowStats();
        ImGui::TextWrapped("Shadow casters: %u | overflow %u | incomplete %u", shadowStats.casters,
                           shadowStats.overflow, shadowStats.incomplete);
        uint64_t shadowTriangles = 0;
        for (auto triangles : shadowStats.triangles)
            shadowTriangles += triangles;
        ImGui::TextWrapped("Shadow raster triangles: %llu", (unsigned long long)shadowTriangles);
        ImGui::TextWrapped("Shadow storage: %.2f MiB | range %.0f / %.0f m",
                           double(shadowStats.allocatedBytes) / (1024 * 1024), shadowStats.nearDistance,
                           shadowStats.farDistance);
        const char *shadowFallbacks[] = {"none", "sampled depth unavailable", "caster capacity",
                                         "disabled/wireframe"};
        ImGui::TextWrapped("Effective shadows: %s | %s",
                           shadowStats.effective == ow::ShadowMode::CSM ? "CSM" : "Off",
                           shadowFallbacks[unsigned(shadowStats.fallback)]);
        ImGui::TextWrapped("Shadow preparation: %.2f ms", shadowStats.preparationMilliseconds);
        if (shadowStats.timingsAvailable)
            ImGui::TextWrapped("Shadow GPU: %.2f ms", shadowStats.totalMilliseconds);
        if (ImGui::TreeNode("Cascade details")) {
            for (uint32_t i = 0; i < ow::shadowCascades; ++i)
                ImGui::TextWrapped("%u: %u casters | %u tris | %.2f ms", i + 1, shadowStats.cascadeCasters[i],
                                   shadowStats.triangles[i], shadowStats.rasterMilliseconds[i]);
            ImGui::TreePop();
        }
        const auto &s = renderer.terrainStreamingStats();
        ImGui::TextWrapped("Shadow terrain: %u reserved slots | %u requested | %u "
                           "missing | %.2f MiB",
                           s.shadowSlots, s.shadowRequested, s.shadowMissing,
                           double(s.shadowBytes) / ow::terrainMiB);
        break;
    }
    case UiWindow::GtaoDiagnostics: {
        int aoDiagnostic = int(scene.gtao.diagnostic);
        if (ImGui::Combo("AO view", &aoDiagnostic, "Shaded\0Visibility\0Raw\0Normals\0Edges\0"))
            scene.gtao.diagnostic = ow::GtaoDiagnostic(aoDiagnostic);
        const auto &ao = renderer.gtaoStats();
        const char *aoModes[] = {"Off", "Half", "Full"};
        const char *aoFallbacks[] = {"None", "Unsupported", "Budget", "Wireframe"};
        ImGui::TextWrapped("GTAO: %s | %s | %.2f MiB", aoModes[uint32_t(ao.effective)],
                           aoFallbacks[uint32_t(ao.fallback)], ao.allocatedBytes / 1048576.0);
        if (ao.timingsAvailable)
            ImGui::TextWrapped("Completed %s AO: Prep %.2f | Eval %.2f | Filter %.2f ms",
                               aoModes[uint32_t(ao.completedMode)], ao.prepareMilliseconds,
                               ao.evaluateMilliseconds, ao.filterMilliseconds);
        break;
    }
    case UiWindow::ClusterDiagnostics: {
        auto &demo = context.lightingDemo;
        if (ImGui::Checkbox("Lighting demo", &demo.enabled)) {
            actions.lightingDemoReanchor = true;
            actions.lightingDemoChanged = true;
        }
        if (demo.enabled) {
            actions.lightingDemoChanged |= ImGui::SliderInt("Local lights", &demo.lights, 0, 4096);
            actions.lightingDemoChanged |= ImGui::Checkbox("Overlapping lights", &demo.overlap);
            actions.lightingDemoChanged |= ImGui::SliderInt("Transparent layers", &demo.layers, 0, 16);
            ImGui::Checkbox("Animate lights", &demo.animation);
            if (ImGui::Button("Evening lighting")) {
                scene.lighting.sunLux = 1000;
                scene.lighting.environmentNits = 30;
                scene.output.ev100 = 9;
            }
        }
        int diagnostic = int(scene.clusterDiagnostic);
        if (ImGui::Combo("Clusters", &diagnostic, "Shaded\0Occupancy\0Overflow\0"))
            scene.clusterDiagnostic = uint32_t(diagnostic);
        const auto &cs = renderer.sceneStats();
        ImGui::TextWrapped("Lights %u | clusters %u / %u | overflow %u", cs.activeLights, cs.occupiedClusters,
                           cs.grid.count(), cs.overflowClusters);
        ImGui::TextWrapped("Occupancy mean %.1f max %u | tile %u | slots %u", cs.meanOccupancy,
                           cs.maximumOccupancy, cs.grid.tile, cs.grid.indices);
        ImGui::TextWrapped("Clusters %.2f MiB | geometry %.2f MiB", cs.clusterBytes / 1048576.,
                           cs.geometryBytes / 1048576.);
        if (cs.timingsAvailable)
            ImGui::TextWrapped("Assign %.2f | opaque %.2f | blend %.2f ms", cs.assignmentMilliseconds,
                               cs.opaqueMilliseconds, cs.transparentMilliseconds);
        break;
    }
    case UiWindow::MaterialDiagnostics: {
        auto &m = scene.terrainMaterial;
        ImGui::Checkbox("Coverage weights", &m.weights);
        ImGui::Combo("Material diagnostic", &m.diagnostic,
                     "Shaded\0Adjusted weights\0Rock projections\0Shading "
                     "normals\0Logical reads\0");
        int isolation = m.isolate + 1;
        if (ImGui::Combo("Material", &isolation, "Blend\0Grass\0Soil\0Rock\0Gravel\0"))
            m.isolate = isolation - 1;
        const auto &ms = renderer.materialStats();
        ImGui::TextWrapped("Macro: %s | Max logical reads: %u", ms.macroLoaded ? "loaded" : "neutral",
                           ms.maximumTextureReads);
        ImGui::TextWrapped("%s | Effective AF: %.0fx", ms.bindless ? "Bindless" : "Fixed palette",
                           ms.effectiveAnisotropy);
        ImGui::TextWrapped("Images: %.2f MiB / %.0f MiB", ms.imageBytes / 1048576.,
                           options.materialBudgetBytes / 1048576.);
        if (!ms.error.empty())
            ImGui::TextWrapped("%s", ms.error.c_str());
        break;
    }
    case UiWindow::PostDiagnostics: {
        const auto &post = renderer.postStats();
        const auto &aa = renderer.spatialAaStats();
        ImGui::Text("AA: %s (requested %s)", antialiasingName(aa.effective), antialiasingName(aa.requested));
        ImGui::Text("Spatial AA: %.2f MiB | %s", aa.bytes / 1048576., postFallbackName(aa.fallback));
        if (aa.timingsAvailable)
            ImGui::Text("Spatial AA including preparation: %.3f ms", aa.milliseconds);
        ImGui::Text("Thin geometry: %s | %s | %.1f MiB", post.thinGeometry ? "on" : "off",
                    ow::postFallbackName(post.thinGeometryFallback), post.thinGeometryBytes / 1048576.);
        ImGui::TextWrapped("TAA %s | bloom %s | %.1f / %.1f MiB", post.taa ? "on" : "off",
                           post.bloom ? "on" : "off", post.taaBytes / 1048576., post.bloomBytes / 1048576.);
        ImGui::TextWrapped("Post GPU %.3f / %.3f ms | history %s | reset %s", post.taaMilliseconds,
                           post.bloomMilliseconds, post.historyValid ? "valid" : "reset",
                           ow::temporalResetName(post.reset));
        ImGui::TextWrapped("Post availability: %s / %s | rejected %u | overflow %u",
                           ow::postFallbackName(post.taaFallback), ow::postFallbackName(post.bloomFallback),
                           post.rejectedSurfaces, post.trackingOverflow);
        break;
    }
    case UiWindow::AtmosphereDiagnostics: {
        const auto &a = renderer.atmosphereStats();
        ImGui::TextWrapped("Atmosphere %.2f MiB | %s", a.bytes / 1048576.,
                           ow::atmosphereFallbackName(a.fallback));
        if (a.timingsAvailable)
            ImGui::TextWrapped("LUT %.3f | IBL %.3f | transport %.3f ms", a.lutMilliseconds,
                               a.environmentMilliseconds, a.compositeMilliseconds);
        const auto &t = renderer.terrainStreamingStats();
        ImGui::TextWrapped("Outer coverage %.1f km | horizon %u / %u",
                           t.outerEffectiveRadius * options.terrainGrid.extent() / 1000, t.horizonReady,
                           t.horizonRequested);
        ImGui::TextWrapped("Distant levels %u | %.1f MiB%s", t.horizonLevels,
                           double(t.horizonBytes) / (1024 * 1024),
                           t.horizonLimited ? " | range limited" : "");
        break;
    }
    case UiWindow::VolumetricDiagnostics: {
        auto &fog = scene.volumetrics;
        int diagnostic = int(fog.diagnostic);
        if (ImGui::Combo("Fog diagnostic", &diagnostic,
                         "Shaded\0Transmission\0Scattering\0Shadow "
                         "coverage\0History rejection\0"))
            fog.diagnostic = uint32_t(diagnostic);
        const auto &v = renderer.volumetricStats();
        ImGui::TextWrapped("%s | %ux%ux%u | %.2f MiB", ow::volumetricFallbackName(v.fallback), v.grid.x,
                           v.grid.y, v.grid.z, v.bytes / 1048576.);
        ImGui::TextWrapped("Lights %u | excluded %u | overflow %u | rejected %u", v.selectedLights,
                           v.excludedLights, v.overflowClusters, v.rejectedCells);
        if (v.timingsAvailable)
            ImGui::TextWrapped("Assign %.3f | inject %.3f | history %.3f | integrate %.3f | sky "
                               "%.3f | composite %.3f ms",
                               v.assignmentMilliseconds, v.injectionMilliseconds, v.temporalMilliseconds,
                               v.integrationMilliseconds, v.skyMilliseconds, v.compositeMilliseconds);
        break;
    }
    default:
        break;
    }
}
} // namespace ow

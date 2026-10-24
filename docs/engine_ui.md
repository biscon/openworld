# Engine UI and layout persistence

Status: implemented; Debug/Release and software Vulkan verification pass. Native
desktop review remains pending because the attempted run failed with an X11
`BadDrawable` error.

## Responsibilities and ownership

`main` owns platform/backend lifetime, engine settings, camera, demos and renderer,
and executes application actions. `EngineUi` owns window visibility and draws
menus and cohesive panels through a typed context of short-lived references.
It returns explicit actions for terrain regeneration, camera reset, review
bookmarks, render-height changes, palette reload and demo presets. Drawing does
not own renderer resources or change their lifetimes. Lighting-demo control
values are shared with the existing demo update loop; hiding a window does not
stop animations or reset renderer settings.

The menu/window shell, settings panels and diagnostics panels have separate
implementation files. JSON loading/saving and the fixed window registry live in
`ui_state`, with no dependency on a renderer instance. Review paths are formatted
once outside frame processing. Diagnostic text wraps when panels are narrowed.

A fixed registry gives each panel a stable ID, menu category, label and default
geometry. All panels start closed. Every panel has one checked menu entry and a
title-bar close button using the same visibility flag. `View / Close All Windows`
hides all panels. There is no docking or separate platform-window support.

## Organization

- Terrain: Terrain Settings (radius, erosion and base/eroded comparison), Base
  Terrain Generation, Terrain Materials (presets and material tuning).
- Rendering: Rendering Settings (resolution, LOD/refinement, visibility, depth,
  shadows and GTAO), Vegetation (enabled state, 20–200 m range, density, habitat,
  wind, transmission, optional occlusion and collapsible diagnostics), Lighting &
  Materials, Atmosphere, Local Volumetrics, Output.
- View: Camera & Review, Background, Close All Windows.
- Diagnostics: Frame & Device; Terrain Streaming & Generation; Terrain LOD;
  Rendering & Visibility; Shadows; GTAO; Lighting & Clusters; Terrain Materials;
  Post-processing; Atmosphere & Horizon; Volumetrics; ImGui Demo.

Diagnostic panels combine existing counters, memory, fallback and completed GPU
timing information with their associated visualizations and comparison tools.
Lighting test fixtures live with cluster diagnostics. Operation errors remain
beside the affected controls. Generation still applies on edit release; closing
a diagnostic panel does not disable its visualization. Default panel positions
are staggered beneath the menu. Bounds repair keeps restored panels inside the
usable viewport following resolution or fullscreen changes.

## Persistence and data layout

`UiState` owns a fixed array of visibility flags. A separate persistence module
loads/saves version-1 `ui-state.json` beneath
`SDL_GetPrefPath("OpenWorld", "OpenWorld")`. It uses the installed nlohmann JSON
dependency already required for review views. Fields are `version`, `windows`
(stable ID to boolean), and `imgui_layout` (the native ImGui settings text).
ImGui's public memory settings API preserves positions, full uncollapsed sizes
and collapsed state, including panels closed before shutdown. Automatic `.ini`
file access stays disabled. Layout and flags load before the first frame.

Missing files use defaults. Unknown window IDs are ignored; malformed data,
unsupported versions and files larger than 1 MiB are rejected with a warning
and menu-only defaults. Saves use a same-directory temporary file and rename;
failure preserves the previous destination and reports a warning. Save occurs
on clean exit before ImGui context destruction, never during ordinary frames.
Headless and `--no-ui` runs neither load nor save interactive UI state.

No terrain, rendering, camera, fixture or diagnostic-mode values are serialized.
Existing explicit review bookmarks remain separate. There is no crash recovery
or cross-process merge; the last cleanly exiting interactive process wins.

## Performance and verification

Panel lookup/storage is fixed and bounded; drawing uses direct references and
small action values rather than allocating callbacks or a generic event bus.
Closed panels perform no widget construction. JSON and filesystem work occur
only at startup/shutdown. ImGui may allocate as windows are first opened, as it
does today; no new steady-state storage growth is introduced by the UI module.

Owned temporary fixtures and fresh ImGui contexts test initial visibility,
menu/close-state integration, layout round trips (including collapsed and closed
panels), unknown IDs, invalid/oversized documents and atomic-save failures.
Viewport tests cover reduced display sizes and title-bar clearance. Debug and
Release builds, review bookmark regressions, a small validation-enabled Vulkan
smoke run, and interactive menu/restart checks verify integration. Hardware or
interactive checks that cannot run are recorded explicitly.

### Verification results

- Debug and Release engine builds pass. In each configuration, `ui_state`,
  `engine_ui_gpu`, `review_view`, `cli_help`, `render_resolution_cpu` and
  `render_resolution_gpu` pass. GPU checks use llvmpipe with required Vulkan
  validation; no validation errors were reported.
- CPU tests inject mouse input into the real menu and title-bar close controls,
  and use fresh ImGui contexts to verify restart restoration. They cover
  collapsed full sizes, panels remaining closed across another restart,
  reduced viewport bounds, invalid documents, size limits and failed publication.
- The GPU UI fixture draws every registered panel, checks that idle drawing
  requests no engine actions, and verifies that hiding diagnostic panels retains
  active visualization settings. Owned captures were inspected for menu-only
  startup and panel layout.
- A small headless engine capture with UI and Vulkan validation passes. An
  isolated follow-up with `--headless --no-ui` leaves a pre-existing layout marker
  unchanged without attempting to parse it. The first headless run creates no
  interactive layout file.
- A native SDL/Vulkan desktop launch was attempted with temporary user settings
  and llvmpipe, but X11 terminated it with `BadDrawable` before interactive
  review. Native restart/fullscreen and target-GPU acceptance remain unverified;
  automated input, layout and offscreen rendering checks are not a substitute
  for those checks.

# Terrain generation and inspection

Status: implemented (milestone 1 algorithms, including crease rounding and finer filtered terrain normals). Milestone 2a extends the renderer to a fixed neighbourhood; see [neighbourhood ownership and verification](terrain_neighbourhood.md). Milestone 2b adds [LOD, padded normal levels and asynchronous bounds](terrain_lod.md); that design supersedes the original full-grid rendering layout described here.

## Responsibilities and ownership

`TerrainGrid`, `TerrainSettings`, `ChunkCoord`, and `TerrainRequest` are CPU value types. Grid topology and request validation are independent of Vulkan. `Camera` accepts plain input values, owns chunk/local position and yaw/pitch, and produces a camera-relative view. SDL input and ImGui controls belong to the demo.

`VulkanTerrain` owns shared height scratch and index buffers, aligned per-chunk sample slices, a normal texture array with two layers per chunk and a shared sampler, compute pipelines, descriptors, and two persistently mapped parameter buffers with per-slot slices. It receives a non-owning Vulkan device context; the renderer owns it and destroys it before the device. Fixed private slots need no general resource registry. Vulkan stays behind the renderer boundary.

## Generation and data layout

The current base algorithm is [regional geography](terrain_geography.md), replacing
the original uniform noise. The storage and normal-generation responsibilities below remain in use.

Application defaults: 128 metre base chunks with 256 cells, 0.5 metre spacing, seed 1, mountain wavelength 2048 metres, regional wavelength 16384 metres, erosion wavelength 1024 metres, and height scale 1200 metres. Eight base-noise evaluations form regional elevation, distribution and local relief; five erosion octaves add mountain detail. Hierarchical streaming now supplies the default neighbourhood. Erosion strength defaults to 0.22 and crease rounding to 0.3; shape constants follow the author-linked implementation.

A compute pass generates base/eroded heights as float2 for an (N×R+3)² grid (R = normal subdivisions, default 2), including a one-sample halo. A second pass uses central differences and physical sample spacing to produce normal textures at the dense rate and two float4 values per mesh vertex (normal.xyz, height.w), taking every Rth height/normal sample. The (N+1)² vertices are pulled by index in the vertex shader; a uint32 index buffer describes two upward-facing triangles per cell. The normal pass also rebuilds indices when cell count changes. Selecting the base/eroded variant is a draw parameter and never regenerates terrain.

The filter is a port of Rune Skovbo Johansen's advanced erosion technique, cross-checked against Luke Mitchell's C# translation. Retain upstream notices and record source provenance in third_party. The port substitutes seeded integer hashing for float-based hashing and omits the unused ridge-map output. Rendering normals come from final heights, not approximate filter derivatives.

## Fixed demo spacing and configurable footprint

The demo uses 0.5 m spacing. The previous 1 m / 4 m selector and `--terrain-spacing` option have been removed. Chunk footprint is selected at startup in the application. The core retains capacity-bounded grid changes and camera rebasing for tests/tools. Camera reset accounts for the terrain's reference elevation.

`--terrain-size` accepts power-of-two widths from 4 to 512 m. `--terrain-cells` selects power-of-two counts from 8–1024 at the same spacing, providing smaller footprints for quick tests; specifying both options is rejected. The core grid value type retains physical spacing for coordinate math and reduced test fixtures. There are no alternative spacing paths in the demo UI.

Preallocate storage to the startup footprint and chunk count (default: nine slots, 256 cells each). The milestone 2a layout allocated 58.52 MiB on lavapipe; the current LOD layout allocates 71.48 MiB, excluding render targets and driver overhead. Core grid changes reuse descriptors and storage. Invalid dimensions/settings are rejected rather than growing resources. The LOD renderer uses multiple regular-grid patches per chunk.

## Filtered terrain normals

The normal compute pass additionally writes base/eroded normals into two layers per chunk of a preallocated RGBA16F 2D array texture; per-slot views preserve local base/eroded layer addressing. Full-precision sample storage remains the source of geometry, diagnostic heights/normals, and the vertex-normal comparison. Startup checks storage-image, sampling, and linear-filter support for RGBA16F.

The vertex shader passes local grid coordinates to the fragment shader. A bilinear clamp sampler addresses texel centres at dense terrain samples; coordinates are clamped to the active patch inside capacity-sized storage. The fragment shader samples the selected layer and renormalizes the result before lighting. This replaces triangle-based interpolation with grid-cell bilinear interpolation. The demo mesh remains at 0.5 m, while normals use actual procedural height evaluations at 0.25 m. `RendererOptions::normalSubdivisions` selects 1 or 2 at startup; capacity and layouts stay fixed during rendering.

The default-on **Filtered terrain normals** checkbox switches to the previous vertex-normal interpolation when unchecked. `--vertex-normals` selects that comparison at startup. These are draw-only settings, independent of the base/eroded toggle; neither changes geometry nor regenerates terrain.

The original diagnostic path uses one normal level. The current LOD renderer uses explicit padded, filtered levels and blends neighbouring levels during morphs. This change cannot repair angular silhouettes or recover detail absent from the sampled terrain, and bilinear interpolation is not a guarantee of eliminating every shading artifact.

## Coordinates and limits

Chunk X/Z are signed 64-bit integers. Each noise octave receives a signed 32-bit lattice origin plus a small fractional offset, computed in double precision on the CPU. Shaders add local sample offsets and hash integer lattice coordinates; they never convert absolute world positions to float. Validate lattice bounds with halo/neighbour margin before dispatch. Negative coordinates use floor decomposition. Camera coordinates use chunk/local doubles; only relative positions become floats.

Core bounds: 8–1024 cells, spacing 0.01–1024 m, mountain/erosion wavelength 64–8192 m, regional wavelength 4096–65536 m, height scale 0–2000 m, erosion strength and crease rounding 0–1. The demo fixes spacing at 0.5 m. Non-finite values are rejected. Each octave requires its origin plus grid/halo/neighbour margin to fit within signed 32-bit lattice bounds (conservatively below 2,147,483,000). These checks happen before pending state changes. This is not a claim of infinite numerical precision; fixed erosion octaves can be undersampled at short wavelengths or coarse test grids.

## Updates, synchronization, and allocations

Requests are validated before replacing pending settings. Equal settings and grids do nothing; multiple edits before rendering coalesce into the latest request. A request is marked submitted only after successful queue submission. Skipped frames and swapchain recreation preserve pending work.

Allocate resources at startup. Frame fences protect reuse of per-slot parameter slices in the two frame buffers. Sequential chunk generation reuses one height scratch buffer with explicit write-after-read ordering. On generation frames, synchronization2 barriers order previous terrain reads/writes before compute overwrites, height writes before normal reads, sample writes before vertex reads, and index writes before index-input reads.

Each chunk's two normal-image layers transition from UNDEFINED on first use, and thereafter from SHADER_READ_ONLY_OPTIMAL to GENERAL before compute writes. A subsequent transition makes those writes visible to fragment sampling and returns it to SHADER_READ_ONLY_OPTIMAL. These image dependencies also cover diagnostic fixture writes and in-flight footprint changes. All work uses the existing graphics/compute queue.

Ordinary frames draw cached data. No terrain-owned host/device allocation, descriptor update, blocking readback, or device-idle wait occurs during ordinary regeneration or footprint edits. UI and driver internals are outside that allocation claim. Shutdown, swapchain resize, capture, and explicit diagnostics may wait.

## Camera and interaction

Hold right mouse outside ImGui for relative mouse look and WASD/QE flight. Shift boosts speed fourfold; default speed is 100 m/s. Normalize combined movement and clamp pitch. Release capture on mouse release, focus loss, or minimization. Escape releases capture first, otherwise exits. Reset camera uses a conservative height bound (1.25 times height scale); single-chunk inspection subsequently fits its actual mesh bound with a blocking snapshot at startup/explicit reset only. Generation edits, including crease rounding, commit on edit release; display/camera edits act immediately. Normal-colour and wireframe inspection switches are draw-only; wireframe pulls triangle indices and uses barycentric coordinates and screen derivatives, requiring no optional polygon-mode feature.

Base Terrain Generation contains seed, regional wavelength, mountain wavelength,
height scale, ridge strength and base detail. The latter two default to 0.50 and 0.65 respectively; both at one recover the previous ridged terrain. See
[base smoothness](terrain_geography.md#adjustable-base-smoothness).
OpenWorld retains erosion, view/camera controls and diagnostics.
Both windows resize, scroll and collapse independently to their title bars; edits
are coalesced after both window bodies, including when either is collapsed.

## Public and diagnostic interfaces

`RendererOptions::terrainGrid` selects the initial grid, `terrainCapacity` bounds preallocated storage, `setTerrainGrid` queues a validated grid change, and `ui=false` permits offscreen tests without an ImGui context. `requestTerrain` validates and coalesces requests; `render(Camera, SceneSettings)` consumes pending generation only on submission. `terrainGenerationCount` counts submitted generations.

`readTerrainSnapshot` and `readTerrainIndices` wait and return owned active data, rejecting calls when requested generation is pending. `normalsForFixture` uploads owned dense-grid halo heights, runs the production normal pass (including texture writes), reads its output, and invalidates cached procedural terrain so the next render restores it. These diagnostics never run in ordinary frames.

## Verification

CPU tests cover topology/winding, coordinate bounds and negatives, grid validation, request coalescing, camera movement/rebasing, and allocation-free core updates. GPU tests use small owned procedural and planar fixtures; they check finite heights, normals, zero-strength identity, seeds, repeatability, adjacent/distant coordinates, generated indices, repeated in-flight edits, capacity/footprint changes, and caching under synchronization validation.

The lighting integration test projects the terrain triangles and reconstructs sparse visible pixel samples on the CPU. It compares captured fragment lighting against bilinearly sampled normal data, and separately against vertex interpolation, for both base/eroded variants and several active sizes smaller than the allocated texture. A tolerance of three 8-bit colour levels accounts for half-float normal storage, sampler precision, and sRGB conversion. A curved fixture must visibly distinguish the two shading modes. Toggling either comparison must leave generation count unchanged. Tests check our mapping, layers, and resource reuse rather than re-testing Vulkan filtering in isolation.

No game assets are required. Readbacks and captures use isolated temporary outputs. Same-device repeatability does not imply cross-device bitwise identity. Routine workloads are deliberately small; full-size captures are separate visual/validation checks. Prefer automated offscreen checks over scripted UI text entry.

Fixed neighbourhoods, LOD and hierarchical streaming are implemented; their linked designs supersede the original milestone 1 residency limits. Collision, materials and advanced lighting remain deferred.

## Sampling investigation

See [sampling report](terrain_sampling.md) for measured geometry/normal errors, continuity checks, before/after captures, diagnostic tooling, regression tests, and the rationale for crease rounding 0.3 with 0.25 m normals. The finest mesh remains at 0.5 m. The study uses production GPU samples and independent dense references without UI automation.

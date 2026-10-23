# OpenWorld

A C++17 starting point for an open-world engine: **SDL3 + Vulkan + HLSL + Dear ImGui**.
Milestone 4 streams hierarchical procedural terrain around a free-fly camera, with Rune's erosion
filter, smooth geometry LOD, filtered normal detail, HDR/PBR lighting and textured terrain.
Two background workers load and maintain a disposable disk cache. Cache misses use
incremental GPU generation; uploads/readbacks and residency have explicit budgets.
ImGui provides generation controls, a view-radius slider, base/eroded comparison and diagnostics.

Slice 5E.5 adds spatial GTAO for terrain and opaque/masked meshes. It is **Off**
by default; select **Half** or **Full**, adjust the radius, or inspect AO buffers in
the terrain window. It affects indirect diffuse lighting only and forces the current
depth prepass independently of the visibility mode. See [GTAO design, checks and probes](docs/rendering_gtao.md).

## Dependencies

On Arch Linux, use the graphical privilege prompt:

```sh
pkexec pacman -S --needed base-devel cmake ninja sdl3 glm vulkan-headers vulkan-icd-loader directx-shader-compiler vulkan-validation-layers libpng libsquish nlohmann-json python
```

A working Vulkan driver for your GPU is also required (normally installed with
your graphics driver). Optional tools / a CPU driver for automated checks:

```sh
pkexec pacman -S --needed vulkan-tools vulkan-swrast
```

Dear ImGui v1.92.5 is included under `third_party/imgui`, with its MIT license.
Only its core, demo, and SDL3/Vulkan backends are built. The erosion port's
attribution and licenses are in `third_party/erosion`. Configuration does not
download or build SDL, a Vulkan SDK, LLVM, or DXC from source.

The runtime requires **Vulkan 1.3**, `dynamicRendering`, `synchronization2`, and
a queue family supporting both graphics and compute (and presentation in windowed
mode). D32 depth attachments are required. These are checked during startup;
unsupported devices produce an error rather than silently changing renderers.

The offline `material_import` target uses system libpng, libsquish and nlohmann JSON,
plus the small vendored BC7 encoder. Runtime loading needs none of these libraries.
Building `engine` also builds `terrain_assets`, importing the tracked source PNGs
and palette recipe into `<build>/assets/terrain/runtime`. This happens on the first
build and when inputs change or outputs are missing; unchanged builds reuse the
textures. Generated DDS files are not versioned. To build only the palette, run
`cmake --build cmake-build-release --target terrain_assets`. See
[asset provenance and import instructions](assets/terrain/README.md).

## CLion

Open this directory as a CMake project. CLion can import `CMakePresets.json`;
enable the **debug** or **release** configure preset and select the **engine**
run target. If you prefer CLion's own CMake profile, the root `CMakeLists.txt`
works directly with a Debug or Release build type and Ninja.

No particular working directory is required: the executable knows its build's
shader and default material directories. Use `--material-dir` when relocating assets. Shader edits rebuild automatically as part of the `engine`
target. Stop/restart the executable after shader edits; runtime hot reload is
not implemented yet. `compile_commands.json` is generated for editor tooling.

## Command line

```sh
cmake --preset debug
cmake --build --preset debug -j 4
./cmake-build-debug/engine
```

Release:

```sh
cmake --preset release
cmake --build --preset release -j 4
./cmake-build-release/engine
```

On GCC/Clang, Release uses `-O3 -DNDEBUG`, with link-time optimization enabled
when supported by the compiler. Use `-DOPENWORLD_RELEASE_IPO=OFF` to disable
link-time optimization for build diagnostics. Debug keeps its normal settings;
HLSL shaders are compiled with DXC `-O3` in both configurations.

Hold **right mouse outside the panel** to look and fly: **WASD** moves forward/back
and sideways, **Q/E** moves down/up, and **Shift** boosts speed fourfold. Release
right mouse to use the UI. **Escape** releases mouse capture first, then exits
when UI keyboard input is not active. Capture also releases on focus loss or
minimization. The panel includes a camera reset and flight speed control.

**F10** toggles borderless desktop fullscreen. The initial window is **1920×1080**.
Scene rendering defaults to **1080 pixels high**, with width following the window
aspect ratio; ImGui stays at native framebuffer resolution. Select a **Render height**
preset in the OpenWorld panel or launch with `--render-height 720` (custom heights
1–16384 are accepted). Shrinking the window still renders at the chosen scene height
and downscales. The panel shows both scene and output dimensions. See
[scene resolution and presentation](docs/rendering_resolution.md) for resource
ownership and verification.

Terrain sliders and numeric fields commit changes when editing finishes. The
base/eroded checkbox switches cached data immediately; it does not regenerate.
The default uses **128 m base chunks at 0.5 m spacing**, with **view radius 64**
(8.192 km outward). The **View radius (base chunks)** slider supports up to 128
(16.384 km) at this footprint. Higher data levels cover progressively larger areas
with coarser samples. The engine does not load every base chunk in the square.
Frustum culling and patch LOD determine the geometry actually drawn.

Nearby terrain gains fine heights and 0.25 m normals asynchronously. Resident
parents retain coarse coverage while children load or fade, including the
neighbouring parents needed to preserve shared edges. Cold generation publishes
directly on the GPU before optional disk-cache export. Missing initial coverage
appears progressively; world-setting changes start a fresh demand epoch.
See [hierarchical terrain design](docs/terrain_hierarchy.md).

Chunk footprint is a **startup setting**, selected with `--terrain-size` (power-of-two
widths from 4–512 m) or `--terrain-cells` (power-of-two counts from 8–1024).
`--chunk-x` / `--chunk-z` select the initial camera region. `--single-chunk` retains
fixed inspection without streaming. The supported radius depends on footprint and
budget; requested/effective radius are shown explicitly. The default **512 MiB GPU** budget targets the full slider range; the actual
supported distance depends on driver allocation requirements.

The UI reports coverage/detail residency, pending work, cache hits/misses/errors,
managed GPU/host storage, transfers, generation/terrain GPU timing, residents by data level and LOD draw statistics.
GPU pools and host buffers are allocated at startup; adjusting radius does not
resize them. `--terrain-gpu-mib`, `--terrain-host-mib` and `--cache-mib` set startup
budgets (defaults 512, 256 and 8192 MiB). `--cache-dir` overrides the per-user cache
directory; `--no-cache` retains streaming while disabling disk persistence.
On Linux the default is `$XDG_CACHE_HOME/openworld/terrain-v1`, falling back to
`$HOME/.cache/openworld/terrain-v1`. Cache entries are disposable and may be deleted
while the application is stopped. Persistent player edits are not stored here.

Lighting samples a bilinearly filtered terrain-normal texture per pixel. The
**Filtered terrain normals** checkbox compares this with the vertex-normal
interpolation, without regenerating terrain or changing the mesh. It is enabled by
default. Nearby geometry stays at **0.5 m**; level-zero procedural normals use
**0.25 m** spacing. Coarser levels use padded, filtered normal images.
Crease rounding defaults to **0.3**, softening under-resolved gully bottoms. The
**Rendering diagnostics** section exposes **Normal colours**, actual-triangle
**Wireframe**, **LOD colours**, chunk borders, and **Freeze LOD camera**. Freezing
keeps the distance observer fixed while visibility follows the live camera.
**Terrain LOD** / `--no-lod` provides a full-detail comparison; **Frustum culling**
can also be toggled. These controls do not regenerate terrain.
See the [sampling investigation](docs/terrain_sampling.md) for measurements and
before/after captures.

The **Base Terrain Generation** window controls seed, regional wavelength,
mountain wavelength, and height scale. **OpenWorld** contains erosion wavelength,
strength and rounding, camera/rendering controls, and diagnostics. **Lighting &
Materials** controls PBR lighting, manual exposure, ACES fitted/AgX output and a
toggleable lighting demo with point/spot lights, textured meshes and transparent layers. **Terrain Materials** controls
the generated grass/soil/rock/gravel palette, stable world mapping, tint, normal strength,
roughness, material isolation and trilinear/8×/16× filtering. All four windows can
be moved, resized, and collapsed to their title bars. See the
[material design and verification](docs/rendering_materials.md) and the
[HDR/PBR design and verification](docs/rendering_hdr_pbr.md). Regional plains,
foothills, and mountain ranges are visible in the uneroded base terrain. Heights are
an artistic procedural approximation, not a simulation of flowing water.
The window is resizable and uses FIFO presentation (VSync).

Debug builds request validation automatically. `--require-validation` turns
missing validation support into an error. `--validation` and `--no-validation`
override the build default. Validation messages go to stderr, and runtime
validation errors cause a nonzero exit. Synchronization validation is enabled
when the validation layer exposes `VK_EXT_validation_features`.

The lighting demo is opt-in and anchors its fixtures at the camera's location when enabled.
Use **Evening lighting** to inspect local lights, and compare **Brute force** with
**Clustered**. Occupancy/overflow views and completed GPU timings expose list pressure.
The former material-preview scene has been removed; its regression swatches now use
the ordinary mesh API. See [slice 5D](docs/rendering_clustered_lighting.md) for resource
limits, transparency limitations and profiling commands.

Other useful options:

```sh
./cmake-build-debug/engine --view-radius 128 --require-validation
./cmake-build-debug/engine --lighting-demo --require-validation
./cmake-build-debug/engine --lighting-demo --agx --ev100 15
./cmake-build-debug/engine --constant-material
./cmake-build-debug/engine --fixed-materials --rgba-materials
./cmake-build-debug/engine --material-dir /path/to/palette
./cmake-build-debug/engine --legacy-lighting
./cmake-build-debug/engine --no-cache
./cmake-build-debug/engine --cache-dir /tmp/openworld-terrain-cache
./cmake-build-debug/engine --shader-dir /path/to/compiled/shaders
./cmake-build-debug/engine --terrain-size 256
./cmake-build-debug/engine --chunk-x -3 --chunk-z 2 --chunk-borders
./cmake-build-debug/engine --single-chunk --terrain-size 512
./cmake-build-debug/engine --lod-colours
./cmake-build-debug/engine --no-lod
./cmake-build-debug/engine --vertex-normals
./cmake-build-debug/engine --terrain-cells 128
./cmake-build-debug/engine --headless --single-chunk --base --no-ui --capture /tmp/base.ppm
./cmake-build-debug/engine --help
```

## Offscreen validation

The offscreen mode renders the same terrain, compute passes, depth buffer, and ImGui
overlay into ordinary Vulkan images, without an SDL window or display server.
Camera input uses a fixed timestep and the CLI defaults to eight frames. Streaming
completion depends on worker/GPU progress; eight frames are insufficient to fill a
cold landscape. Use fixed inspection for a quick deterministic capture, or the
streaming probe below to wait for coverage.

```sh
./cmake-build-debug/engine --headless --single-chunk --frames 8 --require-validation --capture /tmp/openworld.ppm
```

To deliberately use Arch's software driver instead of the hardware driver:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-debug/engine --headless --single-chunk --frames 8 --require-validation --capture /tmp/openworld.ppm
```

CPU correctness tests are enabled by CTest (`BUILD_TESTING`). Enable the opt-in
GPU integration and smoke tests with:

```sh
cmake --preset debug -DOPENWORLD_GPU_TESTS=ON
cmake --build --preset debug -j 4
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --preset debug
```

The suite checks grid topology, coordinate bounds, request caching, camera motion,
and allocation-free CPU updates, including transactional neighbourhood requests. GPU integration uses small synthetic fixtures to
check planar normals, erosion, seeds, repeatability, shared edges, distant
coordinates, generated indices, and settings/footprint updates with frames in
flight. Sparse captured pixels are checked against a CPU lighting reference for
filtered normals, vertex interpolation, and both terrain variants, with a defined
tolerance for precision differences across drivers. Fixtures use a unique
temporary directory and are removed after each GPU test.

The neighbourhood suite compares every shared edge and corner, dense normal-texture
edges, reversed generation order, negative/distant coordinates, and tiled versus
single-patch captured lighting/coverage. Repeated in-flight batch and grid changes
exercise shared scratch, topology, texture layers, and slot reactivation.

LOD tests check all quadrant masks, coverage, morph winding, shared edges/corners,
normal filtering against an independent continuous reference, camera paths,
conservative culling, stale bounds, and inactive-slot reactivation. CPU selection
is checked for steady-state allocations. Flat-fixture captures compare LOD and
full detail pixel-for-pixel; curved fixtures check continuity at a selection change.

Streaming tests cover cache round trips/invalidation/corruption, disk failures and
pruning, deliberately blocked workers, stale handles, bounded pools and retirement.
GPU tests use small owned grids and a 64 KiB transfer allowance to force partial
transfers. They compare cached/generated heights and every normal level with the
established generator, compare near-ground captures, exercise a 6 MiB residency
budget, and change epochs/radii/coordinates while work is in flight.

The hierarchical suite additionally checks shared parent/child samples, balanced selection with delayed data, GPU vertex continuity and sky-hole captures during eviction, direct publication, a 32 KiB transfer budget and a 4 MiB residency budget.

The milestone 4 probe saves ground/vista captures and per-frame CSV timings:

```sh
./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-16km 256 128
```

The earlier flat streaming probe remains a diagnostic baseline. It fills a region, restarts with a warm cache, crosses
boundaries, changes radius and teleports with requests pending. Use a fresh output
directory for a cold run. Optional arguments select cells per chunk and radius:

```sh
cmake --preset release -DOPENWORLD_GPU_TESTS=ON
cmake --build --preset release -j 4
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_stream_probe /tmp/openworld-stream-review 256 2
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_stream_probe /tmp/openworld-stream-radius8 64 8
```

For a separate default-size camera sequence and review captures (not a routine test):

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-debug/terrain_lod_probe /tmp/openworld-lod-review
```

The default overview submits 52,224 triangles versus 1,148,928 in full detail with
the same camera and culling enabled. The ground-level probe submits 134,656 versus
393,216. These are geometry counts, not hardware frame-rate measurements.

The eight-frame smoke test uses one fixed chunk with 32 cells per side and saves
`cmake-build-debug/smoke.ppm`. GPU tests require a Vulkan 1.3 driver and validation
layer. Remove the environment override to use the default driver. Offscreen tests
do not exercise the swapchain or SDL input.

`--terrain-size M` defaults to 128 m at fixed 0.5 m spacing. Alternatively,
`--terrain-cells N` accepts powers of two in 8–1024 and sets the footprint to N × 0.5 m; the two
options cannot be combined. `--terrain-spacing` and the old spacing selector have
been removed. Smaller grids reduce computation for checks. `--base` selects the
uneroded variant; `--no-ui` hides the overlay. Tests can independently choose
spacing and dimensions through the terrain API. No test depends on game assets.

## Layout and design

- `src/main.cpp`: SDL lifetime/events, ImGui controls, frame timing, CLI.
- `src/terrain.*`: terrain value types, neighbourhood construction, transactional batch validation, grid topology, request state.
- `src/terrain_lod.*`: bounded quadtree selection, shared patch topology, bounds and draw statistics.
- `src/terrain_stream.*`: bounded residency policy, spatial lookup, handles, payload layout and budgets.
- `src/terrain_cache.*`: two filesystem workers, immutable payload leases, versioned cache and bounded pruning.
- `src/vulkan_terrain_stream.*`: incremental generation, transfer slices, GPU pools, normal-detail transitions and retirement.
- `src/camera.*`: input-independent camera movement and camera-relative transforms.
- `src/vulkan_terrain.*`: reusable GPU terrain buffers, generation, and diagnostic readback.
- `src/renderer.*`: Vulkan device, presentation, frame synchronization, terrain draw, and capture.
- `shaders/`: HLSL terrain generation, normals, drawing, and filter includes. CMake
  tracks includes and invokes DXC to produce Vulkan 1.3 SPIR-V.
- `tests/`: owned synthetic fixtures and CPU/GPU correctness tests.

See [streaming design](docs/terrain_streaming.md), [LOD design and measurements](docs/terrain_lod.md), [system design](docs/system_design.md), [neighbourhood design](docs/terrain_neighbourhood.md), [terrain design](docs/terrain_generation.md),
and [milestones](docs/milestones.md) for responsibilities, constraints, and next steps.

The renderer uses one graphics/compute queue and two frames in flight. Cache workers
never call Vulkan. Main-thread cache coordination never waits for disk or mailbox
capacity. Generation uses reusable scratch and per-kernel GPU timing estimates
targeting a soft 1 ms budget. Adjacent workgroups are batched into larger dispatches;
the allowance starts at four groups and adapts up to 4,096 groups per frame.
The UI reports submitted groups and their current allowance. Combined upload/readback traffic is limited to 8 MiB per frame.
A small startup compute warm-up pays deferred first-use compilation before streaming.
Existing frame fences retire transfers and slots; no additional terrain-specific
idle waits or synchronous diagnostic readbacks occur during streaming.

Default terrain GPU allocation is approximately **450.45 MiB** on llvmpipe, with
about **44.27 MiB** of managed host working storage. These are startup reservations,
not memory proportional to the current slider value. They exclude render targets,
thread/runtime and opaque driver overhead. Cached products are uncompressed;
background pruning maintains the disk budget asynchronously.

Swapchain recreation, shutdown and explicit diagnostic captures/readbacks may wait.
The first implementation routes generated products through bounded readback/upload
buffers, sharing the same publication path with disk loads. Avoiding that extra
cold-generation transfer is a future profiling opportunity. The 1 ms generation
target is soft: an individual dispatch and driver first-use work can exceed it.

Split noise lattice origins preserve precision at large coordinates; supported
coordinate ranges are configuration-dependent and validated. Cache compatibility
includes shader and GPU/driver fingerprints. Infinite precision and cross-device
bitwise reproducibility are not assumed. Collision and persistent edits remain future work.
Terrain materials currently use world-XZ mapping and smooth weights; cliff triplanar
projection, height blending and repetition breakup remain slice 5C.

## Portability and verification

The source uses SDL's Vulkan surface API and enables portability enumeration and
`VK_KHR_portability_subset` when advertised. On Windows, supply SDL3 and GLM CMake
packages plus a Vulkan SDK with DXC. On macOS, also supply a MoltenVK version/device
that meets the feature requirements above. No Windows or macOS build has been
verified yet; support is an implementation target, not a tested guarantee.

Verification on Arch Linux uses Debug/Release CTest with llvmpipe and Vulkan
synchronization validation. The streaming CPU tests also run under ThreadSanitizer.
The separate probes verified 49 coverage chunks at the default footprint and all
361 coverage chunks for radius 8 with a reduced footprint, with zero Vulkan
validation errors. Warm-cache starts required zero generation. See the streaming
design for measured counts and limitations.

No NVIDIA device nodes are available in this environment. Software-renderer timings
are not target-GPU frame-time evidence; startup warm-up addresses the first-use
generation spikes found by the probe.
Interactive swapchain/input behaviour and Windows/macOS builds remain unverified.

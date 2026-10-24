# 5J-V2: reusable vegetation assets and foliage rendering

Status: implemented and validated with Debug/Release, software Vulkan and native
RTX 3060. Measurements, captures and remaining acceptance limits are recorded in
the [native report](measurements/vegetation_5j_v2/rtx3060/README.md). Mixed/GTAO GPU
budget and reference-quality acceptance remain open.
This implements the agreed V2 plan in [the roadmap](rendering_vegetation.md).

## Boundary and ownership

An immutable library owns up to eight assets, three LODs and two material parts per
LOD (48 bins). An immutable initial population owns at most 4096 world-positioned
cells and 524288 compact instances. Runtime loads saved assets; offline recipes
build mesh geometry and DDS textures. No streamed placement, live asset replacement,
terrain attachment updates, habitat system or player interaction is added here.
The V1 fixture remains an explicit legacy input adapted to this same GPU backend.

Each instance is 32 bytes: cell-local root/scale, yaw, variation, integer identity,
and packed asset/cell index. A checked typed asset-ID packing function is exposed;
category metadata controls grass thinning and caster exclusion independently of
asset order. Cells hold explicit contiguous ranges; dispatch work
is split into at most 128 candidates per group. CPU frame preparation visits cells,
not plants. Integer world origins are reduced before camera-relative conversion.
Static per-cell wind phases preserve animation across camera rebasing.

Library/population data is validated before upload. Mesh serialization writes named
32-bit fields in little-endian order (format version 2), never compiler struct
padding. Recyclable
materials/textures use existing generation-checked handles; immutable asset IDs are
validated indices. No live library slot recycling is exposed. Partial initialization
uses RAII and releases every acquired material/texture. Frame-owned buffers and
material descriptor references are reused only after their renderer fence. Shutdown
waits for submitted readers before releasing resources.

## Assets and build

Two nested-subset grass tufts, one fern and one bilberry-like shrub establish the
representative set. Grass uses blade geometry; fern uses subdivided fronds; shrub
uses stems and masked leaves. Vertices carry normalized height, bend/flutter weights
and stable phase. Asset metadata defines stiffness, displacement envelopes, LOD
ranges and casting distance. Roots remain anchored. Invalid geometry, nonfinite
values, unsupported versions and mismatched texture payloads fail explicitly.

Tracked JSON recipes and source PNGs live under assets/vegetation/source. The offline
builder emits versioned mesh/metadata and complete DDS chains into the build tree.
CMake tracks referenced source files and the builder; a completion stamp is written
last. No-op rebuilds do no work; tests use isolated synthetic inputs.
Colour mips filter in linear light with opacity weighting, safe transparent colour
and cutoff-coverage preservation. BC7 sRGB and RGBA colour variants accompany BC5
and RGBA normals. Normals are an explicit authored shallow surface approximation,
not inferred physical measurements from colour. Roughness/transmission are material
constants. Separate fern/shrub colour and normal textures consume four of the five
available non-fallback fixed-binding image slots; no atlas is necessary.

## GPU work and shading

Camera selection and caster selection use separate bounded work ranges. Grass is
excluded from production caster indices and bounds. Grass-only populations issue no
cascade selection or shadow draws; fern starts at 25 m casting and shrub at 50 m,
with 5 m fades. Selection remains count/prefix/scatter with bounded indirect commands.
Multi-part assets may occupy two bins, so camera list capacity includes both parts;
caster lists are sized from casting candidates, not the full grass population.
Sparse caster candidates are packed into 128-lane groups across cell boundaries;
each lane resolves its instance's cell. Keeping a full group per nearly empty cell
wasted most lanes in the first mixed measurement (1.53 ms close, 0.29 ms selection).
Camera groups remain contiguous per cell. This changes scheduling only, preserving
plant identity, caster bounds, distances and output lists.
Overflow clamps writes and reports lost entries, invalidating acceptance runs.

The GPU ABI uses 64-byte vertices, 32-byte instances and 112-byte frame-local cells.
Cells contain camera and caster bounds, current/previous relative origins and a
stable wind phase. A 16-byte work record identifies a contiguous range or a range
in the immutable caster index list. Each of six views has 48 fixed indirect bins
and an overflow word (385 words). Both frame slots own their lists, counters,
uniforms, cell uploads and delayed diagnostic readback. The immutable CPU population
is copied at initialization; caller edits do not modify submitted data. The saved
library is consumed at startup, with no filesystem reads in frame processing.

A 128 MiB vegetation budget includes driver allocation requirements, uploads,
frame resources and diagnostics. Immutable uploads may remain retained and charged;
there is no steady-state allocation, list growth, readback decision or added wait.
The existing CSM/depth/GTAO/colour ordering is preserved. V2 defaults to the
existing opaque depth prepass (independently disableable); both off/on measurement
frames retain this configured pass. Short native tuning measured ~1.31 ms with it
versus ~1.94 ms without it; the final report includes the full paired comparison.
No Hi-Z integration is added.
Shared lighting supports a restrained thin-leaf diffuse transmission response for
sun/local lights and opposite-side environment irradiance. Sun visibility applies
to transmission; AO applies only to indirect diffuse. Ordinary surfaces use zero
transmission. Exposure, atmospheric lighting and transport remain shared.

Wind combines shared direction, broad gusts and fine flutter, constrained by asset
maximum displacement. Shared vertex evaluation and alpha/coverage evaluation serve
colour, depth and shadows. Previous vertices use previous successful submission,
including wind, camera origin and settings. Incompatible LOD, newly selected plants,
reset/disable and time discontinuities invalidate correspondence. Normal and tangent
frames follow the deformation; integer identities do not pass through float storage.

## Review and gates

The automated probe has legacy and V2 profiles. An isolated SDL3/ImGui viewer loads
the same library and provides fixed meadow/mixed/asset views and lighting, wind,
LOD, TAA, AO and shadow controls. Normal application vegetation remains disabled.
The V1 historical 25 m casting default remains confined to its compatibility input.

CPU/build tests cover round trips, malformed input, deterministic LOD subsets,
coverage/normal mips, explicit cell ranges, large coordinates, capacity limits and
incremental builds. Small owned GPU fixtures cover both binding paths, mask/pass
agreement, transmission, grass exclusion, offscreen casters, submitted history,
rebasing, bounds, overflow and frame reuse with synchronization validation.
Release target-GPU runs use 1080p, 120 warmup/600 measured frames, three alternating
vegetation off/on pairs. Report total median/p95, incremental pass and CPU costs,
actual memory, allocations and coverage for all five V1 views, grass and mixed sets.
Budgets remain ~1.5 ms incremental GPU, ~0.25 ms CPU preparation and 128 MiB; full-scene
FPS and streaming remain unproven. AO is enabled in both members of AO comparison
pairs. Existing V1 measurements are the baseline, not a request for user reruns.

## Initial native tuning

An initial 24/12/6-blade library exceeded the GPU budget (~3.5 ms moving). A
48-byte per-instance animation cache had no meaningful total-frame win and was
removed rather than consuming 48 MiB. The revised grass keeps 24 nearby blades,
uses 6/2 middle/far blades and 3/2/1 segments, with narrower 2 cm blade bases.
Final paired runs and visual review are recorded in the native report; these
measurements do not establish complete-game or reference-quality acceptance.

## Public usage

Build `vegetation_assets`, `vegetation_probe` and `vegetation_viewer`. Launch
`cmake-build-release/vegetation_viewer` for the isolated SDL3/ImGui review; right mouse
plus WASD/QE moves the camera. Content and view controls select mixed/meadow scenes
or individual specimens; exposure, wind, transmission, LOD, prepass, TAA, GTAO and
CSM controls are independent. Save/load view uses `vegetation-view.txt` in the working
directory. `--headless --frames 8`, `--fixed` and `--rgba` provide smoke/fallback runs.

The probe keeps its V1 default. Use `--profile v2` for grass-only, add `--mixed` for
fern/shrub content, or `--asset grass|fern|shrub` for specimen captures. Use
`--no-prepass` for the measured prepass comparison and `--exposure 14` for brighter
artwork inspection without changing the standard EV100 15 measurement profile.
`--quick` is a reduced correctness fixture, never native-performance evidence.

Runtime callers load `readVegetationLibrary(directory, rgba)`, then supply immutable
`VegetationOptions::library` and `population`. No per-tuft `MeshInstance` submission
is involved. The old `fixture` option is mutually exclusive and retained only for
V1 compatibility. `VegetationSnapshot::binCount` defines the view-major count stride;
visible/caster counts are draw entries (two for a selected two-part shrub), not
unique plants. Diagnostics return full uint32 identities, including above 2^24.

V2 instance transforms/populations are immutable. Attachment or population changes
require reconstruction and fresh history in this slice; generation-checked streaming
slots and incremental updates belong to V3. Wind time is finite in [0,1000000];
backwards time or a gap above .25 seconds rejects correspondence. Scene shadow
reception remains active independently of vegetation casting. Mesh/lod publication,
retirement under streaming, interaction-field history and production terrain ground
coverage remain the later slice contracts, not claimed V2 behaviour.

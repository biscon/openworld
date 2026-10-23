# Slice 5C: terrain blending, cliffs and macro variation

Status: implemented. Current performance-revision verification is recorded below.
This extends [5B](rendering_materials.md) and implements [roadmap slice 5C](rendering_roadmap.md).

[Slice 5D](rendering_clustered_lighting.md) generalizes the optional prepass to opaque/masked
meshes and retains attachment depth through transparency. Material-probe plane workloads use
tool-owned shaders on ordinary meshes; references to preview workloads below describe their
historical equivalent. No sampleable depth/Hi-Z is introduced.

## Performance revision

Spatial stochastic sampling, its controls and lattice state have been removed.
Ordinary micro textures, height blending and signed cliff projections remain. High
now selects 16× micro AF; it does not multiply samples. Macro colour/roughness artwork keeps
its four layers and signed projections, but uses a separate trilinear sampler
(binding 8) because the imported signal is deliberately low frequency. The
additional per-fragment regional hash noise is removed. This preserves dedicated macro art
without paying micro anisotropy or procedural hash cost for it. The uniform shrinks
from 640 to 608 bytes; bounded logical reads become 12–24. Hardware speedup remains
unmeasured until RTX 3060 comparison; logical reads do not count AF taps.

`SceneSettings.terrainDepthPrepass` is optional and off by default. Terrain
selection, streaming weights and morph state are prepared once, then the exact
same patches, vertex shader and constants are drawn into reversed-Z depth. Fixed
terrain needs no fragment shader; streamed terrain uses a minimal shader with the
same requested-view clipping as shading. A depth write/read barrier separates the
passes. The prepass has no colour attachment; HDR is cleared only when shading
begins. Shading uses EQUAL with depth writes disabled. Preview remains single pass.
The renderer owns the extra pipelines and recreates/destroys them alongside its
existing pipelines; depth storage and frame lifetimes remain unchanged. No new
frame allocations or duplicate LOD selection are needed. Scene timestamps include
both passes so a faster shading pass cannot hide the prepass cost.

This is a measurable option, not a universal optimization: it doubles vertex and
draw work and only saves hidden fragments. It cannot reduce visible-pixel material
cost, and low view range can still fill the screen with expensive material work.
Hi-Z later complements this by rejecting entire hidden patches before shading;
this option is not yet the retained/sampleable depth foundation for slice 5E.

Verification: owned fixed and streamed fixtures compare prepass on/off HDR output,
including view clipping, morph/LOD and wireframe. Run synchronization validation,
material CPU/import/GPU and terrain/lighting regressions. Keep macro-off isolation
and add paired prepass probe modes with total scene timings. Target-GPU timings
and live camera motion remain an explicit follow-up.

## Responsibilities and material evaluation

Material evaluation owns appearance only: stable coordinates → projected samples
→ normalized height blending → shared SurfaceMaterial. Generation, cache identities,
coverage classification and residency remain unchanged. Fixed, streamed and preview
rendering share HLSL evaluation and fixed/bindless semantics.

Grass, soil and gravel use XZ projection. Rock transitions between top projection
and three signed projections from 25° to 60° using the filtered terrain normal.
Normalized fourth-power axis weights preserve the top-only path on plains. The
comparison-only two-axis control smoothly suppresses the smallest weight below
0.08, reaching zero at 0.01; it retains all three axes where removal creates a seam.
Decoded normal slopes become world-space surface gradients, projected onto the
actual tangent plane and blended before perturbing the normal. Neutral maps retain
signed/oblique geometric normals. UNORM neutral bias has a small dead zone and the
slope denominator is bounded.

Derivatives precede all material branches; reads use SampleGrad. CPU integer-origin
reduction supplies XYZ micro/macro repeat phases without converting a distant world
position to float. Projection loops remain bounded; zero coverage/axis contributions
skip reads. There are no stochastic cells, sample rotations or temporal randomness.

Packed height is artistic relief, not displacement. Scores are
`coverage + 0.25 * (height - 0.5)`. Contributions are
`coverage * max(score - maximumActiveScore + blendWidth, 0)`, then normalized and
mixed with original coverage by the footprint-faded influence (default 0.5).
Absent layers cannot appear; zero coverage falls back to grass. These weights
compose colour, roughness, AO and normal gradients. Height influence and micro
normal strength fade from 1/32 to 1/4 texture repeat per pixel (0.125–1 m at the
4 m default repeat). Existing normal-variance roughness mips remain active.

## Macro assets and ownership

Four image-generated artworks describe broad variation over 64 m. Source images,
prompts and processing settings remain with the palette. Import filters the signal
to 32×32 before producing a 512-square four-layer BC7 sRGB array and RGBA8 fallback,
with full mips. RGB contains normalized colour modulation (linear 0.5 neutral);
alpha contains separately authored roughness modulation (0.5 neutral). Colour
modulation is bounded to ±20%, roughness to ±0.08. Macro uses the same signed
projections as each material but its own trilinear sampler; micro AF is unchanged.

Missing macro files select a neutral fallback and disable sampling. Malformed
supplied files reject the whole palette replacement. Typed array slots, immutable
records, two frame snapshots, publication and retirement retain the existing fence
ownership. The 320 MiB budget includes fallback/current/retiring images. Macro
payload remains about 1.33 MiB BC7 or 5.33 MiB RGBA8. No steady-state allocations,
new streaming system or per-pixel atomics are added.

## Interfaces and diagnostics

`TerrainMaterialSettings` owns height/cliff/macro controls, blend width, influence,
macro scale/strength and diagnostic selection. Baseline disables 5C features;
Balanced enables them with 8× micro AF; High selects 16× micro AF. Presets preserve
isolation, tint and world state. The uniform is 608 bytes, with coordinates at 544,
blend at 576 and features at 592; CPU assertions check the HLSL layout. Each frame
also owns 64 32-byte material records. Fallback array slots are 0–3, palette slots
4–7. Fixed images use bindings 3/4/5/7; micro/macro samplers use 6/8. Bindless image
capacities are unchanged. The macro sampler reuses the existing trilinear object.

`MaterialStats` reports availability, allocation bytes and the conservative logical
read bound: 12 baseline, 18 with all rock projections, 24 including macro. Actual
zero contributions skip reads. These counts exclude anisotropic taps and the
terrain normal map. Diagnostics show coverage, adjusted weights, projections,
final normals and reads (normalized to 24); colours bypass exposure/tone mapping.

## Verification strategy and tools

Owned palettes cover normalized/zero-support height weights, neutral and directional
normals on signed faces, projection continuity, phase precision/rebasing, optional
and malformed assets, budgets and fence-safe replacement. Numerical GPU fixtures
exercise actual fixed/bindless shaders. Integration compares fixed/streamed output
and preserves terrain generation identities. Import tests verify sRGB RGB, linear
alpha, normalization and mip means. Depth comparisons cover fixed geometry,
mixed-level morphing, streamed view clipping and wireframe diagnostics.

`material_probe` retains multi-seed captures and warmed median/p95 total scene/output
timestamps. Compare `balanced_8x` and `depth_prepass` on the terrain workload; preview
intentionally has no prepass. `macro` versus `baseline` isolates macro, while
`balanced_trilinear`/`balanced_8x`/`balanced_16x` separate micro filter costs.
No software Vulkan result establishes RTX 3060 performance or live shimmer.

## Performance-revision verification

Release regression: **20/20 passed** in 154.18 seconds using llvmpipe and Vulkan
synchronization validation. Focused Debug regression: **6/6 passed** in 33.83 seconds
(material CPU/import/GPU, lighting GPU, terrain LOD GPU and terrain LOD quality GPU).
Prepass on/off comparisons include fixed and streamed
textured terrain, wireframe, mixed LOD morphs, hierarchical clipping and switching
back to the single pass. Fixtures assert visible terrain so empty captures cannot
pass equivalence checks. Material presets still do not regenerate terrain.

The final three-seed capture/timing smoke completed without validation errors.
[Raw timings](measurements/material_5c_revision_llvmpipe.csv) and
[device/command metadata](measurements/material_5c_revision_llvmpipe.txt) record
320×180 RGBA/fixed rendering on llvmpipe, four warm-up and eight measured frames.
This is a small software smoke, not a hardware benchmark. Scene median/p95 ms:

| Workload | Baseline | Macro only | Balanced 8× | Balanced + depth prepass |
| --- | ---: | ---: | ---: | ---: |
| Dense terrain patch | 27.43 / 29.47 | 28.40 / 31.18 | 41.45 / 44.45 | 52.50 / 54.75 |
| Four-layer cliff plane | 2.63 / 2.68 | 2.73 / 3.06 | 3.83 / 4.36 | Not applicable |

The dense 256×256-cell patch at this low resolution makes extra geometry costly;
the prepass loses in this smoke. The live hierarchical RTX 3060 scene has different
geometry, overdraw and pixel costs. `/dev/dri` and NVIDIA device nodes are unavailable
here, so target-GPU performance remains unverified. After restarting the demo,
compare the **Opaque depth prepass** checkbox at a fixed camera using **Scene**
GPU milliseconds; this includes both passes. Keep it off unless total time improves.
The probe's `balanced_8x` and `depth_prepass` terrain rows make the same comparison.

## Historical verification before the performance revision

Release regression: **20/20 passed** in 156.71 seconds. Focused Debug checks:
**8/8 passed** in 41.73 seconds (material CPU/import/GPU, lighting CPU/GPU,
terrain GPU, terrain LOD GPU and terrain LOD quality GPU). All GPU checks use
llvmpipe with synchronization validation. Final material/lighting reruns after the
diagnostic-display correction also passed: **2/2 Release** (13.90 s) and **2/2
Debug** (16.19 s). The regression compares captured diagnostic output under ACES
and AgX with different exposures; it must remain byte-identical.

The numerical shader fixture validates normalized/zero-support height weights,
signed/oblique neutral normals, directional normal orientation, quarter-turn
transforms, projection continuity, vertical camera phase, spatial rebasing,
negative-origin regional continuity and the 78-read maximum combination. Integration
checks additionally compare fixed/bindless and streamed/fixed outputs, exercise
macro removal/malformed replacement and protect existing resource budgets/lifetimes.
No production artwork is required by these tests.

Reviewed [macro repeated/offset grids](images/materials/macro_review.png),
[three-seed preset comparisons](images/materials/terrain_5c_comparison.png),
[ground views](images/materials/terrain_5c_ground.png) and a
[captured camera sweep](images/materials/terrain_5c_motion.webp). Macro patterns
remain visible as micro detail converges; no hard projection or stochastic-cell
seams were apparent in the inspected captures. The fixed 128 m review patches have
visible outer boundaries; these are not streamed-world coverage tests. Captured
frames do not establish live shimmer behaviour or target-GPU frame times.

The full 640×360 BC/bindless capture sweep produced all three seeds and 96 motion
frames. Its process was terminated during the subsequent long software timing run;
those incomplete timing samples are not used as measurements. Short isolated
fixture runs below completed with zero validation errors. They use the
[controlled four-layer cliff plane](images/materials/terrain_5c_fixture.png), omit
the procedural coverage classifier, and measure the scene pass (material plus
lighting), not an isolated hardware fragment counter.

**Software Vulkan only:** eight measured frames after four warm-up frames;
RGBA/fixed binding for the full-resolution runs. Values are median/p95 scene ms.
1280×800 is the demo's default window size, not a verified live native framebuffer.

| Variant | Logical reads | 1920×1080 | 1280×800 |
| --- | ---: | ---: | ---: |
| Baseline, 8× | 12 | 65.79 / 66.23 | 35.91 / 36.65 |
| Balanced, trilinear | 24 | 52.16 / 53.40 | 26.75 / 27.37 |
| Balanced, 8× | 24 | 106.78 / 108.29 | 55.13 / 55.91 |
| High, 16× | 60 | 270.82 / 279.26 | 138.56 / 140.61 |
| All stochastic, 16× | 78 | 379.67 / 407.11 | 201.69 / 212.98 |

These are not RTX 3060 estimates or rendering-budget acceptance. A separate
320×180 BC/bindless run completed all modes: Balanced 8× was 74.47/75.29 ms versus
3.95/4.26 ms for RGBA/fixed at the same dimensions. This large software format-cost
difference is another reason not to extrapolate to hardware. Image allocations were
68,511,488 bytes BC and 274,046,976 bytes RGBA, including fallback/padding. The
matched 320×180 BC/RGBA captures differed by at most 3 display-code values
(mean absolute difference below 0.44/255) for Balanced and all-stochastic modes.
No performance benefit is established for weak-axis pruning.

Raw matrices and device metadata: [1080p](measurements/material_5c_1080_llvmpipe.csv),
[1280×800](measurements/material_5c_default_llvmpipe.csv),
[320×180 RGBA](measurements/material_5c_fixed_llvmpipe.csv),
[320×180 BC](measurements/material_5c_bc_llvmpipe.csv). Adjacent `.txt` files record
resolution, device, binding and image bytes.

Known limits: grass, soil and gravel retain XZ projection, including steep eroded
areas where the stable broad-landform classifier does not select rock. Macro artwork
is artistic modulation, not measured material data. Spatial averaging may reduce
micro contrast; histogram preprocessing remains deferred. Axis pruning remains a
comparison option, not a measured performance win. RTX 3060 timings at 1080p/native
resolution and live flight/shimmer review remain pending hardware access.

## Reproduction

```sh
cmake --build cmake-build-release --target engine material_import material_probe
cmake-build-release/material_import assets/terrain/source/palette.json assets/terrain/runtime --macro-only
# Full review and timing matrix (120 measured frames/mode after 16 warm-up frames):
cmake-build-release/material_probe assets/terrain/runtime /tmp/material-5c-review 1920 1080 --motion
# Use the actual demo framebuffer dimensions for the second target-GPU run.
# Small software correctness/timing smoke, including fixed RGBA binding:
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/material_probe assets/terrain/runtime /tmp/material-5c-fixed 320 180 --quick --fixed --rgba --fixture-only
# Isolate one preset at full resolution:
cmake-build-release/material_probe assets/terrain/runtime /tmp/material-5c-1080 1920 1080 --quick --fixture-only --mode balanced_8x
```

`--quick` measures eight frames after four warm-up frames; it is a smoke check,
not a robust tail-latency benchmark. CSV output records workload, median/p95 scene
and output timings, logical read bound, effective anisotropy and sample count.
`review.txt` records device, dimensions, binding mode and image allocation bytes.
Missing timestamp support is reported as unavailable rather than as zero cost.

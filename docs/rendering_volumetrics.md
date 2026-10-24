# Local froxel volumetrics — slice 5I

Status: implemented; software verification is recorded below. Target-GPU and live
motion acceptance remain pending.

## Stability correction

Native review found blinking mist and visible bands. The initial implementation
jittered density without accumulating extinction, rejected the resulting source
changes, and projected arithmetic depth centres into log texture coordinates with
a small systematic offset. Thus even a stationary scene changed opacity and
diffused its history. The implemented correction:

- Use deterministic stratified quadrature on a camera grid independent of image
  TAA jitter. Keep raster reconstruction and atmospheric-tail coordinates matched
  to their own jittered projections. Reproject to the actual stored cell centres.
  Temporal filtering still smooths camera motion; it no longer invents random
  density/shadow changes in a stationary scene.
- Redistribute the existing depth budget with a 16 m logarithmic offset instead
  of 1 m, reducing coarse distant layers without adding textures or passes. Use
  two deterministic samples by default; keep the one-sample cost option. Preserve
  continuous terminal-cell transport rather than snapping surfaces to slices.
- Expose a separate local-volume density multiplier, with explicit height-density,
  reference-altitude and vertical-falloff labels/tooltips. Mist creates only mist;
  slats, test lights and transparent panels belong to the shafts review preset.

GPU regressions compare repeated frames with image TAA both on and off,
check opacity against independent height-density quadrature across depth layers,
and check shadow-edge stability, local-density scaling and existing mixed-media
transport. Software captures/timing supplement these tests; native motion review
is still required. Ownership, resource capacities and pass synchronization stay
as documented below; the density multiplier resets fog and image history like
other medium edits.

## Shaft filtering correction

Subsequent native review found a repeating pattern with Shafts, while Mist looked
acceptable. Stable point samples of sharp slat shadows are still undersampled:
temporal self-reprojection preserves that spatial alias rather than removing it.
The implemented correction integrates sun visibility over each density stratum's
three-dimensional footprint. A bounded deterministic quadrature filters shadow visibility
before multiplying scattering; density and local-light sampling stay unchanged.
Projected footprint vectors are reused inside a cascade. Two depth taps handle
crossing an occluder even when looking along the light. Up to three subdivisions
double the tap count along the longest remaining projected spacing, stopping when
that spacing fits one shadow texel: **2–16 bilinear comparisons per stratum per
sampled cascade**, at most 32 per cascade with the default two density samples.
Existing overlap/fallback can consult additional cascades, bounded by the five
available maps. This spends samples on depth for
long, narrow cells and on XY for wide cells, avoiding the residual alias from a
fixed 2x2 lateral filter. The grid's ray/footprint basis is calculated once per
froxel and reused across density strata. Each cascade must contain the whole
footprint; otherwise the existing farther-cascade fallback applies. Centre coverage,
depth bias and far fade retain their existing semantics. Work is bounded, with no new textures,
allocations, history resources or passes. Hardware comparison filtering can use
the existing sampler when supported; a manual comparison path remains available.

Validation goes beyond stationary stability: an owned periodic-slat fixture
compares fog-cell visibility with independent fine integration of geometric
occlusion, including shifted camera/sample alignments. Before/after review images,
existing opacity/history and CSM coverage tests, Vulkan validation, and a separate
timing probe accompany the change. Target-GPU cost and native visual acceptance
remain separate from software verification.

Post-correction verification (2026-10-24): the owned fixture uses nine 3 m slats
at 5 m spacing, a 16x12x64 fog grid, two camera alignments and 4096 geometric
visibility samples per reference cell. It does not read the shader's sampling
pattern or depend on game assets. Mean visibility error fell from **0.3098**
(95th percentile **0.5861**) to **0.0248 / 0.0239** in the two alignments
(95th percentile **0.0666 / 0.0652**). Default and forced-manual comparison paths
pass. The earlier opacity regressions still measure zero stationary variation;
stationary shaft scattering varies by less than 0.045% of peak.

Focused volumetric CPU/GPU and atmosphere GPU checks passed in Debug (**3/3,
41.29 s**) and Release (**3/3, 30.69 s**) with Vulkan synchronization validation
and no reported errors. All 83 SPIR-V modules in each build passed Vulkan 1.3
validation. The full suite recorded later in this document predates this correction.
Both engine builds are rebuilt. An owned 960x540 review view reproduces the
crosshatch before the correction; the regular pattern is absent afterward.
These captures do not replace native motion review.

A paired Release software probe (llvmpipe, 320x180, 16 warm-up/16 measured frames,
validation off) recorded Normal whole-frame GPU median/p95 changing from
**16.46/17.42 ms** to **17.85/18.46 ms**. High changed from **17.48/18.35 ms** to
**19.90/20.85 ms**; Off stayed approximately **12.7/13.5 ms**. This is additional
GPU work, not a performance improvement. All nine cases retained zero measured
C++ allocation calls during render and unchanged image/buffer memory. Native RTX
3060 cost, the 1 ms incremental target and the user's exact view remain unverified.

Evidence: [before capture](measurements/rendering_5i/shaft_filter/before_normal.png),
[after capture](measurements/rendering_5i/shaft_filter/after_normal.png),
[failing baseline](measurements/rendering_5i/shaft_filter/before_failure.log),
[Debug checks](measurements/rendering_5i/shaft_filter/debug.log),
[Release checks](measurements/rendering_5i/shaft_filter/release.log),
[numeric details](measurements/rendering_5i/shaft_filter/release_details.log),
[before timings](measurements/rendering_5i/shaft_filter/before_timings.csv) and
[after timings](measurements/rendering_5i/shaft_filter/after_timings.csv).

`volumetrics_probe OUTPUT --shadow-review --width 960 --height 540 --frames 1`
captures Normal/High from an oblique view toward the sun. `--shader-dir DIR`
allows a matched baseline shader build; the selected path and review mode are
recorded in probe metadata. Use longer measured intervals without validation for
timing; the one-frame review run is for visual comparison only.

## Responsibility and public contract

`VulkanVolumetrics` owns independent bounded density/light assignment, scattering
history, integrated transport, sky transport, descriptors and timing queries.
The renderer owns frame ordering and scene depth/HDR transitions. CPU volume
settings, validation and reference integration have no Vulkan dependency.
`SceneSettings::volumetrics` defaults off, including in the demo. Review presets
are opt-in. The atmosphere and procedural world retain their existing identities.

Fog consists of exponential height density plus additive sphere/oriented-box
volumes with smooth edge falloff. Extinction is inverse metres, albedo is linear
RGB in [0,1], and one HG anisotropy describes local scattering. WorldPosition and
generation-checked handles follow ordinary scene ownership. Pools allocate at
construction; exhaustion returns an invalid handle, stale mutations return false.
Local lights expose a scattering multiplier and history-participation flag.

## Grid, transport and budgets

Normal is ceil(scene extent / 12), aspect-preserving capped at 192x108, 64 cells.
High uses /8, capped at 256x144, 96 cells. Logarithmic depth has an explicit zero
boundary, a 16 m distribution offset, default 1 km and range 50 m–4 km. At 1 km,
Normal's last interval is about 64 m rather than the original 102 m; this spends
fewer slices on the first few metres. One or two deterministic samples per cell
are independent of grid quality (default two). This doubles injection sample work
relative to one sample, but does not change resource memory or the other passes.
Local density fades over the final 10% of depth.
Default limits are 32 volumes, 128 selected lights, 32 indices per coarse 4x4x4
froxel cluster, and 384 MiB including both frame slots and histories. The budget
and statistics cover driver-reported image/buffer allocations; driver-private
pipelines/descriptors/queries and CPU storage are excluded. Assignment
covers empty screen regions. Stable nearest-influence ordering selects lights;
overflow truncates deterministically and reports counts, never scanning all scene
lights per froxel. All hot-path containers are reserved at construction.

Extinction and sources from atmosphere and local media are added before integrating
with the constant-cell exponential integral. Arithmetic is FP32, image storage is
FP16, and output uses the existing pre-exposure. Atmosphere's existing MS/haze
source remains unchanged; added fog sunlight uses CSM. Volume shadow sampling has
no normal bias or fragment derivatives and respects existing cascade coverage.
Uncovered regions fade to unshadowed. Point/spot lights are unshadowed.

Atmospheric tail transport is integrated directly from the fog endpoint, avoiding
subtraction/division of nearly opaque cumulative transport. Near/far composition
is `(Tn*Tf, Sn+Tn*Sf)`. The full atmosphere sky/IBL products remain independent.
Opaque and transparent fragments apply one shared transport; transparency keeps
coverage alpha. Sky integration retains the ground and solar disk terms and never
passes through the opaque composite. Aerial-off surfaces receive only local fog;
sky still receives atmospheric scattering. Off/Analytic atmosphere retains its
existing backdrop. Zero local density bypasses to 5H exactly.

Integrated images contain cell-boundary transport; terminal-cell analytic
integration preserves surface depth rather than snapping to a whole cell.
Density uses inverse-curved geographic positions; shadows/lights use rendered
positions. Both current and previous curvature participate in reprojection.

## History and GPU lifetime

Current extinction is never temporally blended. Scattering source is reprojected,
exposure-rescaled and neighbourhood-clipped before front-to-back integration.
History has its own submitted camera, independent of image TAA. The froxel grid
uses the unjittered camera; image raster reconstruction and atmospheric-tail lookup
explicitly account for image TAA jitter. Arithmetic cell centres map back to their
actual history texel centres, avoiding stationary depth diffusion.
Cuts (256 m/45 degrees), explicit reset, resize, configuration/profile/world changes,
volume edits and diagnostics reject history. Rebases retain valid correspondence.
Changed/removed lights reject both old and new influence bounds. History-disabled
lights reject affected cells each frame. Fog reactivity also reaches image TAA.

Each frame slot is updated after its fence. Graphics-queue dependencies protect
previous-submission history reads and resource reuse. State advances only after
successful submission. Explicit reconfiguration uses the existing idle boundary;
ordinary frames introduce no extra waits. Unsupported capabilities/budget disable
fog with a visible reason; allocation errors propagate. Diagnostic snapshots are
blocking observers, never runtime inputs. Minimal fallback images/metadata remain
descriptor-valid even below budget, and their allocation is reported.

## Verification strategy and limitations

CPU fixtures cover analytic homogeneous transport, independent mixed-medium
quadrature, zero density, pool lifetimes/capacity, validation, coordinate curvature,
light selection and allocation-free updates. GPU fixtures use small generated
scenes for near/far composition, transparency, sky, shafts, point/spot lighting,
volume edges, motion/cuts/rebasing/exposure, fallbacks and resource recreation.
Targets: 0.002 absolute homogeneous T, 0.015 mixed T, 8% non-negligible mixed S;
FP16 tolerance for zero-density identity. Vulkan synchronization validation,
Debug/Release CTest and SPIR-V validation accompany implementation.

Separate probes record median/p95 CPU/GPU time, memory, overflow and allocation
counts at 1080p/4K with stationary/moving cameras and lights. Initial target:
1 ms incremental p95, Normal 1080p on RTX 3060, within the existing 8.33 ms frame
goal. Software runs do not establish hardware budgets or visual motion quality.
Fog self-shadowing, local multiple scattering, fog shadows on surfaces, clouds,
animated noise and particle density authoring remain deferred.

## Implementation details and review controls

The six compute stages are coarse assignment, density/lighting injection, source
reprojection, prefix integration, sky integration, and opaque composition. Source
history is clipped to six axial neighbours and reduced/rejected on radiance
change. Changed-light spheres conservatively reject whole coarse assignment cells;
this bounds rejection work independently of the number of fine froxels. Volume
edits also reset image TAA. Disabling volume history keeps image TAA available;
only actual moving-light bounds remain reactive in that spatial reference mode.
Shadows use bounded footprint quadrature with existing receiver depth bias,
without surface-normal offsets. Each bilinear comparison uses the existing
comparison sampler when supported, or four manual depth loads otherwise.

Each frame owns three cell textures (extinction, raw source, filtered source),
two boundary textures with N+1 slices, and two sky textures (radiance excluding
the solar disk, and disk transmission). All use RGBA16F. The 400-byte uniform and
112-byte local-volume records use explicit float4 layouts. Extinction alpha holds
shadow coverage; source and integrated transmission alpha carry history rejection.
Shader descriptors are cached until resource/scene-view recreation. The shared
lighting bindings reserve 25–31 for fog; material descriptors reserve 80 nonmaterial
resources, preserving the fixed material path. Previous source/extinction reads
are ordered on the same graphics queue, including two frames in flight.

Atmospheric aerial volumes become **tail** products while fog is active; slice zero
is the exact local endpoint. `AtmosphereSnapshot::aerialStartDistance` reports this
change of origin. The sky view/environment LUTs remain full atmospheric products.
Aerial-off sky integration adds the physical atmosphere to the current local-only
coefficients. Its near integration stops at the virtual ground/atmosphere boundary.
The full-resolution solar disk is evaluated separately from low-resolution sky
radiance, preserving its angular size.

`--fog mist` adds owned boxes/spheres; `--fog shafts` also adds roof slats,
point/spot lights and two transparent panels in front of the activation camera.
They stay world anchored. The Local volumetrics UI offers the same presets, clearing fixtures,
quality, depth, density/height/albedo/phase, one/two samples, history weight and
transmission/scattering/coverage/rejection views. Clearing fixtures leaves the
world and other scene objects intact.

The height layer follows `density(y) = baseDensity * exp((referenceAltitude-y) /
verticalFalloff)`. Base density controls thickness at the reference altitude;
reference altitude is not a top boundary. A rise of one falloff distance reduces
density to 37%; below the reference altitude density increases. Larger falloff
distances make the vertical gradient gentler. The separate **Local volume density**
multiplier scales all authored boxes/spheres without changing the height layer;
zero disables their contribution. Its [0,10] API range is validated, multiplication
happens while packing existing volume records, and changes reset both fog and image
history. The UI offers [0,4] with 1x as the authored density.

At 1x, the preset box is 0.025/m (mist) or 0.035/m (shafts), versus height-layer
defaults of 0.0008/m and 0.0002/m respectively. In a homogeneous region, unscattered
light falls to 37% after `1/density` metres: 40 m inside the mist box versus 1250 m
at the mist height-layer reference altitude. Albedo controls scattering versus
absorption; anisotropy controls scattering direction. Neither is a density control.
Fog distance controls the grid's reach/resolution, not medium thickness. UI labels
and tooltips distinguish all of these. CSM occlusion can legitimately make fog
darker under slats; local ambient/multiple scattering remains unsupported, so these
shadows can be stronger than a fully coupled lighting solution.

The probe accepts `--quick`, `--require-validation`, and explicit width/height/frame
counts. Its CSV includes per-frame CPU/whole-frame GPU/volume totals, median/p95
frame costs, last completed per-stage costs, memory, exclusions, list overflow,
timing availability, and C++ new/new[] calls inside render. These allocation counts
exclude malloc and do not establish driver-wide allocation freedom. Off/Normal/
High/single-sample/spatial/camera/moving-light/history-disabled-light/dense-overlap cases share
owned content. The dense case requests 259 lights and reports deterministic
exclusion of 131 at the 128-light limit.

```sh
cmake --build cmake-build-release --target engine volumetrics_cpu_tests volumetrics_gpu_tests volumetrics_probe
ctest --test-dir cmake-build-release -R '^volumetrics_' --output-on-failure
cmake-build-release/engine --fog shafts --require-validation
cmake-build-release/volumetrics_probe /tmp/5i-1080 --require-validation
cmake-build-release/volumetrics_probe /tmp/5i-4k --width 3840 --height 2160
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/volumetrics_probe /tmp/5i-small --quick
```

The fog source is single scattering with unshadowed local lights and no attenuation
of light paths by the local medium itself. Atmospheric multiple scattering is the
existing 5H closure, not a coupled local/atmospheric multiple-scattering solution.
The independent fog source and sky grids are finite-resolution approximations;
small, sharp volumes/light cones need High quality or a shorter view distance.
Existing 5H angular interpolation near the virtual ground/horizon remains a
limitation: strict far-ray reference comparisons are away from that boundary.
The supported geographic/flight envelope remains the existing horizon envelope.

## Stability correction verification (2026-10-24)

The new owned stationary-mist regression failed before the correction with a
**0.06738** maximum frame-to-frame transmission change (6.7 percentage points).
It now measures **zero** with either one or two samples, with image TAA on or off.
The stationary CSM edge test measures less than **0.09%** of peak scattering
variation over 16 frames. Fine independent quadrature through the height layer,
including the far fade, differs by at most **0.000485** transmission across all
tested depth boundaries. Local-density scaling also matches the expected optical
thickness, and zero local density bypasses an otherwise empty medium.

Focused Debug checks passed (volumetric CPU/GPU: 2/2, 19.69 s; atmosphere GPU:
1/1, 18.75 s). Release passed the same three checks together (3/3, 26.40 s).
GPU checks require Vulkan synchronization validation and reported no errors.
All 83 SPIR-V modules in each build passed Vulkan 1.3 validation. Both engine
builds compile with the new controls. The full 41-test suite below predates this
correction; these focused checks are the post-correction runs.
The rebuilt Release application also rendered eight headless Mist-preset frames
with an owned 4 m terrain patch, no material assets and required validation:
one terrain generation, zero validation errors
([smoke log](measurements/rendering_5i/stability/mist_smoke.log)).

The corrected Release probe at 320x180, 16 warm-up/16 measured frames, llvmpipe,
validation disabled, reports whole-frame GPU median/p95 **17.78/18.96 ms** for
Normal (two samples), **19.65/21.37 ms** for High, and **14.35/17.38 ms** for Off.
All nine cases, including the one-sample option, recorded zero C++ allocations
inside render. These noisy software timings do not establish native GPU cost or
an improvement over the earlier run. No texture capacity or pass count increased.
Native review of the reported view, depth bands and camera motion is still pending.

Retained evidence: [before failure](measurements/rendering_5i/stability/before.log),
[Debug checks](measurements/rendering_5i/stability/debug.log),
[Debug atmosphere](measurements/rendering_5i/stability/debug_atmosphere.log),
[Release checks](measurements/rendering_5i/stability/release.log),
[numeric details](measurements/rendering_5i/stability/release_details.log),
[probe timings](measurements/rendering_5i/stability/timings.csv),
[per-frame samples](measurements/rendering_5i/stability/frames.csv),
[probe metadata](measurements/rendering_5i/stability/metadata.txt),
[corrected Normal capture](measurements/rendering_5i/stability/normal.png) and
[transmission](measurements/rendering_5i/stability/normal_T.png).

## Initial software probe results (2026-10-24, before stability correction)

Release llvmpipe (LLVM 23.1.3), 320x180, validation disabled, 16 warm-up and 16
measured frames per mode, owned slats/lights/transparency, TAA and bloom enabled:

| Mode | Whole-frame GPU median / p95 (ms) | Fog allocations (bytes) | C++ allocation calls |
| --- | --- | --- | --- |
| Off | 13.051 / 16.221 | 25,224 | 0 |
| Normal | 15.860 / 16.804 | 2,792,832 | 0 |
| High | 17.694 / 21.966 | 7,834,752 | 0 |
| Dense overlap | 22.260 / 24.107 | 2,792,832 | 0 |

All eight modes recorded zero C++ allocation calls during the measured render
intervals. The separate validation-enabled probe completed all modes without
validation errors. These are software instrumentation results, not a target-GPU
budget or production terrain benchmark. The recorded final probe ran after the
regression checks, with validation disabled.
The NVIDIA driver cannot communicate with the RTX 3060, so native 1080p/4K timing,
windowed motion review and the 1 ms incremental acceptance target remain unverified.

Retained [timings](measurements/rendering_5i/software_timings.csv),
[per-frame samples](measurements/rendering_5i/software_frames.csv) and
[metadata](measurements/rendering_5i/software_metadata.txt) describe the run.
The [off capture](measurements/rendering_5i/off.png),
[Normal capture](measurements/rendering_5i/normal.png),
[High capture](measurements/rendering_5i/high.png),
[history-disabled light](measurements/rendering_5i/transient_light.png), and
[terminal transmission](measurements/rendering_5i/normal_T.png) use the same owned
review scene. Static captures do not establish temporal quality in motion.

## Initial correctness verification (2026-10-24, before stability correction)

- Full Release CTest: **41/41 passed**, 551.21 seconds. All 41 Debug cases passed
  across the full run and focused reruns. The initial Debug hierarchy test exceeded
  its 120-second deadline while builds shared the host; its isolated rerun passed
  in 103.28 seconds without changing the fixture or timeout. Release hierarchy
  passed in 81.75 seconds.
- Final Debug and Release rendering reruns each passed **3/3** (volumetric CPU/GPU
  and atmosphere GPU). Subsequent volumetric GPU reruns passed in both builds after
  adding rejection of invalid capacities before pool allocation. Fixtures include
  the independent spatial/image-TAA controls and atmospheric diagnostic bypass.
- All **83 SPIR-V modules in each build** passed `spirv-val --target-env vulkan1.3`.
  GPU checks used Vulkan synchronization validation and reported no validation errors.
- The Release application rendered eight headless frames with `--fog shafts`, an
  owned generated 4 m terrain patch, no production material assets and validation
  required: one terrain generation and **zero validation errors**.

The GPU suite exercises analytic absorption, mixed single-scattering reference,
1 m–12 km surface transport including both sides of the local endpoint, own-depth
transparency, point/spot lights over empty backgrounds, CSM occluders and coverage,
128-light selection/32-entry overflow, box boundaries, moving/removed lights,
volume edits, independent history, rebasing/cuts/exposure, tiny resize, High quality,
Direct/Indirect/Hi-Z with GTAO/TAA/bloom, fixed descriptors, curved large coordinates,
atmosphere-unavailable operation, and budget/capability/invalid-capacity failures.

Retained logs: [Release regression](measurements/rendering_5i/release_regression.log),
[isolated Debug rerun](measurements/rendering_5i/debug_isolated_rerun.log),
[Debug rendering](measurements/rendering_5i/debug_rendering.log),
[Release rendering](measurements/rendering_5i/release_rendering.log),
[final Debug volume checks](measurements/rendering_5i/debug_volume_final.log),
[final Release volume checks](measurements/rendering_5i/release_volume_final.log), and
[application smoke](measurements/rendering_5i/cli_smoke.log).

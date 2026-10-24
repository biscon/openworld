# Cascaded sun shadows (slice 5F replacement)

Status: implemented and software-validated on 2026-10-23. The virtual shadow
implementation and its dense clipmap fallback are removed. Native RTX 3060
visual/performance acceptance remains pending; software validation is not that gate.

## Rendering contract

One default-on CSM path renders four near cascades ending at 50, 150, 400 and
1,000 metres and a far mountain cascade extending to the effective view distance
(up to 16 km, 8 km for fixed scenes). Near ranges scale with near-shadow distance;
short far ranges compress them and disable the redundant fifth cascade. Overlaps
occupy the last 10% of the preceding interval. The final range fades to sunlight.

Stable square orthographic projections enclose each overlapping frustum slice in
a bounding sphere. Extents depend on FOV/aspect/splits, never camera rotation or
caster bounds. Centres snap to texels in a world-stable light basis. Integer world
coordinates and long-double phase arithmetic precede camera-relative float transforms. The
above-horizon basis is continuous, including zenith. Conservative depth bounds
include receivers and offscreen upstream casters. Depth is reversed Z throughout.

CPU culling fills compact per-cascade commands; compatible draws use ordinary
indirect rendering with direct fallback. Five separate layer passes need no shader
layer output, GPU BVH, demand compute, page allocation, cache or indirect count.
All active maps refresh every frame before forward lighting. Shadows do not force
a camera depth prepass. Only direct sun is shadowed; blended surfaces receive but
do not cast, and masked materials retain their alpha/sidedness contract.

## Terrain surface and residency

Camera and shadow vertices share position evaluation, including filtered heights,
LOD morph and parent transition weights. Shadow selection uses committed camera
LOD ranges without camera-frustum/Hi-Z rejection or a second adaptive quality solve.
Inside camera coverage, the active parent/child surface and masks are identical.
Outside it, coarse shadow-only tiles supply offscreen occluders without duplicate
coverage. No canonical-height receiver lookup remains.

Existing coarse residency stays bounded to 64 extra slots / 64 MiB, with up to
8 km upstream demand and lower generation priority than camera coverage. Frame
serials retain all referenced height/parent data. Missing upstream bounds project
into transient 32x32 uint coverage masks per cascade (20 KiB per frame),
conservatively expanded and faded at valid edges. Complete cascades skip mask
sampling. This metadata is not a page cache. Shadows fade from normalized
sun Y=0.1 to zero at Y=0 because the finite upstream envelope cannot certify horizon
coverage.

## Sampling, storage and ownership

Depth-array defaults: five 2048-square D32 layers (80 MiB raw), startup-configurable
resolution, total shadow GPU budget 128 MiB including driver padding and two frame
scratch sets. Reserve 65,536 source records and at most five commands per source;
no steady-state growth. Exhaustion invalidates affected cascade coverage, never
silently truncates casters. Unsupported sampled depth selects Off with diagnostics.
Default buffers add approximately 29 MiB to the depth array; driver allocation
requirements determine the actual total. Extra terrain residency has its own budget.

Sampling blends adjacent cascades and uses the fractional footprint of four bilinear
PCF samples. Nine unique texels are compared explicitly, each with its own finite
receiver-plane correction; this avoids the incorrect common reference and old
four-texel correction clamp on steep receivers. Cleared depth is always unoccluded.
The manual-filter override is retained for source compatibility; both paths now use
the same texel comparisons. Geometric-normal offset remains separate. See the
[surface correction](rendering_surface_review.md) for the regression and tradeoffs. Default depth/normal biases are 0.75/0.25 world texels.
Actual rendered receiver positions are used, including transparent surfaces.

The existing graphics queue serializes shared depth-array reads/writes. Barriers
order previous fragment reads, current depth writes and current fragment reads.
Frame-owned mapped uniforms/records/commands are reused after their fence. Blocking
snapshots are diagnostics only; ordinary rendering performs no readback or waits.

Public modes are Off/CSM. Runtime controls cover distances, biases and diagnostic
views; startup controls cover resolution/capacity. Statistics expose per-cascade
casters/triangles, CPU preparation and GPU raster time, memory and missing coverage.
Full-frame GPU timing includes uploads/generation through output/UI; existing scene
and output timings retain their narrower meanings.

`shadows.hpp/.cpp` own settings, GPU uniform layout and pure cascade fitting.
`VulkanShadows` owns the depth array, per-layer views, pipeline resources and two
frame scratch sets, and compacts caller-provided caster bounds into cascade draws.
The renderer collects mesh and terrain casters and records shadows before camera
rendering. `TerrainLod` optionally collects unfrustum-culled draws during the same
selection that commits visible terrain; terrain renderers supply shared draw records,
surface evaluation and serial-based residency pins. Shadow vertex entry points reuse
the colour vertex source with a minimal shadow output interface.

## Verification and acceptance

CPU checks cover split fitting, overlap, snapping, world rebasing, sun basis,
conservative caster/coverage selection, capacities and resource retirement. GPU
checks cover mesh/material behavior, terrain self-shadow and matching positions,
LOD/parent transitions, offscreen casting, cascades, moving light, resize, fallback
and capacity failure under validation. Run the full routine suite and Debug/Release
shader validation. The probe records deterministic camera/sun flights and Off/CSM
comparisons using owned fixtures.

RTX 3060 gates: native 2560x1363 camera-flight review without TAA; 1080p/native
Release timings including CPU preparation, cascade raster, full-frame GPU p50/p95,
memory and active streaming. Retain 2 ms p95 incremental shadows at 1080p and the
120 FPS / 8.33 ms overall target. Do not infer hardware performance or motion quality
from software Vulkan. Historical `shadows_5f_*` CSVs and `rendering_5f_*` images
describe the removed implementation, not current CSM behaviour.

### Executed checks

- Full Debug and Release builds and routine suites passed (28/28 each). After the
  final minimal shadow vertex-interface change, all five affected Release tests
  passed: shadows CPU/GPU, scene GPU, terrain GPU and terrain neighbourhood.
- All 57 shader modules in each build passed `spirv-val --target-env vulkan1.3`;
  final Debug/Release shader binaries are identical. GPU tests and probes used
  llvmpipe with Vulkan validation and reported no validation messages.
- CPU regressions exercise rotation-independent extents, overlaps, snapping at
  negative and billion-chunk coordinates, rebasing, zenith continuity and visible
  terrain selection remaining unchanged when offscreen casters are collected.
- GPU regressions cover compact/direct draws, hardware/manual PCF, masked and
  mirrored casters, transparent receivers/noncasters, offscreen shadows under
  Hi-Z, moving light, removal, resize, unsupported D32 and bounded-storage failures.
  Receivers around all four near-cascade boundaries and at 4 km retain shadows.
- An owned steep-terrain fixture changes its triangle count by more than 2x across
  LOD selections. Overhead lighting produces zero false-shadow pixels out of
  5,717 / 5,717 / 5,714 evaluated pixels; oblique lighting retains 419 genuinely
  shadowed pixels. Streamed terrain and teleport/missing-coverage checks also pass.

### Reproducible probes and limitations

The probe recreates owned fixtures for Off and CSM, warms coverage, and uses fixed
camera/sun inputs. It records CPU preparation, per-cascade raster, full-frame GPU,
memory and coverage data. `--flight` moves the camera; `--moving` moves the sun.
Captures intentionally involve readback and must not be used as a clean benchmark.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  cmake-build-release/shadows_probe /tmp/csm-mountains \
  --quick --scene mountains --yaw 3.14 --frames 12
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  cmake-build-release/shadows_probe /tmp/csm-motion \
  --quick --scene streaming --flight --moving --frames 24 --capture-every 8
```

Both runs completed with settled coverage, no caster overflow and no incomplete
cascades. Quick mode uses 320x180 output, 256-square maps and reduced terrain grids;
these runs verify integration, not production resolution or native motion quality.

| Software mountain probe | Off | CSM |
| --- | ---: | ---: |
| Full-frame GPU median / p95 (ms) | 11.27 / 12.71 | 94.76 / 100.66 |
| CPU preparation p95 (ms) | 0 | 0.50 |
| Shadow raster p95 (ms) | 0 | 87.28 |

CSM is substantially more expensive than Off on this software workload; the paired
incremental GPU median/p95 is 83.23/89.39 ms. This small sample establishes neither
RTX performance nor an improvement over the removed virtual implementation.
The CSM run submits 3,233 cascade caster instances / 905,664 triangles and reserves
30.29 MiB of shadow resources plus 2.98 MiB of extra terrain at these quick settings.
The short motion run crosses a terrain tile boundary while changing the sun; it
does not substitute for a long native mountain flight.

Saved evidence: [mountain summary](measurements/shadows_csm_mountains_llvmpipe.csv),
[mountain frame trace](measurements/shadows_csm_mountains_frames_llvmpipe.csv),
[motion summary](measurements/shadows_csm_motion_llvmpipe.csv),
[motion frame trace](measurements/shadows_csm_motion_frames_llvmpipe.csv),
[mountains with shadows off](images/rendering_csm_mountains_off.png) and
[mountains with CSM](images/rendering_csm_mountains.png).

The NVIDIA driver was unavailable (`nvidia-smi` could not communicate with it).
Run the probe without `--quick` or capture readbacks on the RTX 3060, at 1080p and
native resolution, with representative terrain view distances, camera flight and
moving sun. Review cascade transitions, far silhouettes, LOD changes and low-sun
coverage without TAA, and record p95 incremental GPU cost before accepting the gates.

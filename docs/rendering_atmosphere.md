# Sky, atmosphere and distant coverage — slice 5H

Status: implemented; verification results are recorded below. Target-GPU and live
flight acceptance remain separate gates. Local fog, shafts, clouds, stars, space
rendering and automatic exposure remain outside this slice.

## Responsibilities, controls and units

`VulkanAtmosphere` owns bounded lookup images, GPU environment lighting, descriptors
and queries. The renderer owns pass ordering and transitions of scene HDR/depth.
CPU settings and reference integration have no Vulkan dependency. Atmosphere is Off
for library callers and Physical/Normal in the demo; Analytic is a comparison and
capability fallback. Existing diagnostic shading bypasses atmosphere.

`SceneSettings::atmosphere` exposes mode, quality, aerial perspective, physical
profiles, altitude datum, an environment multiplier, optional boundary haze and
transport diagnostics. Nonfinite/out-of-range settings are rejected. The demo has
clear (aerosol density 1) and hazy (4) presets, quality/transport controls and GPU
cost/memory/readiness displays. `--atmosphere off|analytic|physical` selects the
sky mode; `--no-horizon` disables distant coverage and visual curvature at startup.
`multipleScattering=false` is an owned reference comparison, normally left enabled.

World distances are metres, scattering coefficients inverse metres, and sun input
is top-of-atmosphere illuminance in Physical mode. Integration converts to kilometres
to retain useful FP32 precision around the 6360 km virtual planet. Its centre is
below the camera's horizontal position at a shared altitude datum. Flat library
callers retain flat terrain. The [continuous-horizon mode](rendering_horizon.md)
adds a matching visual curvature transform without changing procedural identity. CPU altitude subtraction
uses doubles. Camera/surface evaluation at or below the datum uses an atmosphere
origin one metre above its bottom boundary; density below the datum is ground density.
This local virtual-planet approximation is intended for ground and low-flight views.

The default atmosphere is 100 km deep, with 8 km Rayleigh and 1.2 km aerosol scale
heights, RGB Rayleigh coefficients (5.802, 13.558, 33.1) × 10^-6 /m, aerosol
scattering/absorption 3.996/0.444 × 10^-6 /m, HG asymmetry 0.8, and a triangular ozone
layer centred at 25 km with 15 km half-width. Ground albedo is 0.1 and solar angular
radius 0.004675 radians. These are RGB physical-model approximations, not spectral
weather simulation. The selected output transform and manual exposure retain their
existing meaning.

## Transport and lighting

The frame sequence is atmosphere products, opaque HDR/motion, opaque aerial
composite, sky, sorted transparency, TAA, bloom, output and UI. Transport is
`T * surface + scattering`. The compute composite reads/writes only its own HDR
pixel, reconstructs position from jittered reversed-Z depth, and skips background.
HDR gains storage-image usage; no extra full-resolution colour image is allocated.

A camera-centred 64×32 latitude/longitude dome is generated from vertex IDs and
renders only at depth zero with EQUAL comparison and no depth writes. Its direction
is independent of world translation. Physical sky radiance includes a separately
evaluated solar disk. The background is not passed through opaque transport again.
Transparent fragments, including unlit/emissive materials, evaluate transport at
their own depth before premultiplication. Alpha remains coverage rather than being
multiplied by atmospheric transmission.

Surface sunlight samples the same atmosphere's sun transmittance at surface altitude
and then applies existing CSM visibility. GPU diffuse irradiance and GGX-filtered
specular environment lighting use the same solar/profile state. The explicit solar
disk is excluded from IBL, avoiding duplicate direct light. The existing BRDF LUT
is reused. Physical mode uses a dimensionless environment multiplier (default 1);
legacy environment nits/rotation retain their meaning in Off/Analytic modes.

LUT scattering and environment values are normalized to unit solar illuminance.
Sun RGB/intensity and pre-exposure are applied on consumption, exactly once. Both
sky and transparent emission reach TAA/bloom; UI remains outside all these stages.

Boundary haze defaults to zero. Its additional extinction rises smoothly from 65%
of the outer square radius, decays with a 2 km height scale, and is integrated
alongside physical extinction. Its source uses the analytic daylight sky colour:
this is an artistic source approximation, not an additional physical aerosol
multiple-scattering solution. A matching double-precision mixed-medium reference
checks it. It is included for sky rays as well as finite surface rays. Future 5I
media must combine sources/extinction over overlapping segments rather than apply
a second full-strength fog pass. Nonoverlapping segments compose as
`(T_near*T_far, S_near + T_near*S_far)`.

## Products, invalidation and GPU ownership

| Product | Normal | High | Representation |
| --- | --- | --- | --- |
| Transmittance | 256×64 | Same | RGBA16F, RGB transmission |
| Multiple scattering | 32×32 | Same | RGBA16F, isotropic closure |
| Directional sky view | 192×108 | 384×216 | RGBA16F, normalized radiance |
| Aerial transport | Two 32³ volumes | Two 64³ volumes | Separate RGB transmission/scattering |
| Environment | 64² cube, seven mips | Same | RGBA16F, normalized GGX radiance |
| Diffuse irradiance | Nine float4 coefficients | Same | GPU SH buffer |

Transmittance uses the spherical distance/altitude mapping; sky latitude concentrates
samples near the virtual horizon and longitude interpolation explicitly wraps.
Aerial depth is quadratic view depth through the existing 64 km far plane, with an
exact zero-distance identity slice. Ray lengths derive from view depth, including
oblique rays. Integration stops at the virtual atmosphere/ground boundary.

Fixed bounds are 128 integration steps for transmittance, 64 directions × 24 steps
for the isotropic multiple-scattering closure, 40 steps for sky rays, 32 steps for
aerial voxels, 64 GGX samples per filtered cube texel and 1024 directions for SH.
The closure estimates repeated isotropic scattering with a bounded geometric series;
it is not a path-traced reference. All integration arithmetic is FP32.

Each existing frame slot owns its products, 368-byte uniform record, SH buffer and
13 compute descriptor sets. The atmosphere image/buffer budget defaults to 64 MiB;
requirements are queried before allocation. Driver-private pipeline/descriptor/query
storage is excluded. Normal image/buffer allocation on llvmpipe is 2,191,360 bytes;
minimal descriptor-valid fallback resources use 6,656 bytes even below the minimum
budget. Unsupported physical formats/depth sampling or insufficient budget selects
the complete Analytic path and reports a reason; allocation errors propagate.

Extinction changes rebuild transmittance and downstream products. Ground albedo
changes rebuild multiple scattering and downstream products. Datum, phase-function
and MS-comparison changes rebuild sky/environment but do not rebuild extinction.
Sky also depends on altitude, sun and optional boundary haze. Aerial transport is
regenerated for each active frame (including TAA's changing raster rays). Exposure
and solar intensity do not rebuild normalized products. Environment cube/SH publish
together at a 0.25° sun change, 25 m altitude change, profile change or haze change;
this bounded lag is deliberate. Revisions commit only after queue submission.

Resources are reused after their owning frame fence. Explicit graphics-queue
barriers order lookup dependencies, scene depth/HDR and subsequent fragment reads.
Descriptors update only at resource creation/recreation, including scene-view
invalidation on resize. No async queue, extra ordinary-frame wait or growing
container is introduced. Mode/quality recreation uses the existing explicit idle
boundary. Bindless material capacity reserves 64 descriptors/resources for the
non-material bindings and terrain arrays rather than the former 32.

`readAtmosphereSnapshot()` is a blocking diagnostic returning normalized products
and SH from the last submitted slot. It never supplies runtime rendering input.
Stage timestamp results are collected without waiting and expose availability.

## Temporal processing

Sky motion is previous unjittered UV minus current unjittered UV, based on camera
rotation only. Motion W=2 identifies valid sky; W=1 remains valid geometry and W=0
invalid. Sky history requires previous background depth zero; geometry history
still requires valid matching positive depth. Background centre pixels select their
own motion rather than borrowing a neighbouring geometry vector. Existing sky-less
clear colours do not accumulate history.

Profile/mode/aerial/haze changes reset history. Sun/altitude/intensity motion reduces
reactivity weights for both physical surfaces and sky. Exposure changes retain
history rescaling. No separate sky or volume history is allocated. Transparent
layers retain the existing reduced-history approximation.

## Distant terrain coverage

The original 5H outer ring is superseded by [continuous distant terrain and a
natural horizon](rendering_horizon.md): compact dyadic levels, a protected 128 MiB
demo reservation, render-space curvature and extended aerial sampling. The detailed
view-radius control and near/shadow budgets retain their roles. Curved-horizon
callers preserve the near aerial samples and append 32 far slices; ordinary flat
library callers retain the original volume layout.

The original software measurements below describe the initial 5H implementation;
new horizon verification and costs are recorded in the linked design.

## Verification and reproduction

Owned fixtures are `atmosphere_cpu`, `atmosphere_gpu`, `horizon_gpu` and the extended
hierarchy CPU test. They cover transport composition, vacuum/zero distance,
parameter validation, reference comparisons, product dependencies, opaque/transparent
own-depth agreement, premultiplied blending, sky TAA, rebasing, exposure, High/Normal,
night, diagnostics, tiny resize, fallback budgets/capabilities, horizon seams and
reservations, radius changes and no-allocation CPU demand. Combined GPU cases exercise
Direct/Indirect/Hi-Z, prepass, GTAO, CSM, TAA and bloom under synchronization validation.
Fixtures generate their own mesh/flat-terrain data and do not load production artwork.

Reference transmittance uses 2048 double-precision midpoint steps. Single scattering
uses 2048 view steps and 256 solar steps. Routine comparisons allow 0.01 absolute
RGB-vector transmission error and 8% relative radiance error away from the disk and
horizon singularities; the initial three-ray fixture measured 0.000450885 and
0.207469% respectively. The higher-sample MS closure reference uses 256 directions,
256 integration steps and 128 solar steps, with a 15% relative bound appropriate to
its much coarser 64-direction GPU angular quadrature. This checks the selected
closure, not equivalence to full multiple-scattering path tracing. Mixed haze uses
0.015 transmission-vector and 0.001 normalized scattering-vector absolute tolerances.

GPU tests and probes require configuring with `-DOPENWORLD_GPU_TESTS=ON`; the
CPU atmosphere test remains available with the default GPU-test opt-out.

```sh
cmake --build cmake-build-release --target engine atmosphere_probe atmosphere_gpu_tests horizon_gpu_tests
ctest --test-dir cmake-build-release -R '^(atmosphere_|horizon_gpu|terrain_hierarchy$)' --output-on-failure
cmake-build-release/atmosphere_probe /tmp/5h-1080 --require-validation
cmake-build-release/atmosphere_probe /tmp/5h-flight --streaming --radius 128 --require-validation
# Software correctness/instrumentation only, small scene and 256 m outer coverage:
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/atmosphere_probe /tmp/5h-small --quick --require-validation
```

The probe runs Off/Analytic/physical static, camera turn, moving sun, sunset, haze and
2 km altitude-offset cases. It records 16 warmup frames, per-frame and median/p95 CPU,
whole-frame GPU and LUT/environment/composite costs, availability, memory and C++
allocation calls inside render. Captures include scene, sky, transmittance, multiple
scattering and final aerial slices. `--streaming` adds bounded cold fill and a moving
camera case; fill has an explicit 60-second deadline and reports whether it settled.
Quick mode uses small owned terrain; it does not establish production-scale coverage
quality, generation performance or target-GPU frame time.

The RTX 3060 120 FPS / 8.33 ms total goal remains unchanged, with an initial 1 ms p95
incremental atmosphere target at 1080p. Terrain generation retains separate headroom.
Hardware/windowed review is unavailable in this environment. Full-size clear-air
horizon review and the 2 km flight envelope are not signed off by software tests.

Algorithm reference: [Hillaire's atmosphere implementation](https://github.com/sebh/UnrealEngineSkyAtmosphere).
Applicable Epic Games/MIT attribution is retained in
[third_party/sky_atmosphere](../third_party/sky_atmosphere/README.md).

### Software verification (2026-10-23)

Both full Debug and Release builds and their complete 37-test CTest suites passed.
After descriptor caching/invalidation refinements, five focused Debug cases passed;
after final window-format lifetime cleanup and independent surface/vacuum checks,
atmosphere and resolution GPU tests passed again in both configurations. All 76
SPIR-V modules in each build passed `spirv-val --target-env vulkan1.3`. GPU fixtures
and validation probes reported no Vulkan validation messages. Live output-format
changes still require windowed review.

The higher-sample multiple-scattering comparisons measured 0.231737% and 0.370749%
relative error for the two selected LUT samples. The opaque fixture additionally
compares final pre-exposed surface radiance against independent ray integration
(2% plus 0.0001 absolute tolerance) at 1 km and 8 km. GPU vacuum transport preserves
surface radiance within 0.0001 RGB-vector absolute error.

Release llvmpipe (LLVM 23.1.3), 320×180, TAA and bloom enabled, 16 warmup plus 16
measured frames per mode, **validation disabled**, recorded:

| Mode | Whole-frame GPU median / p95 (ms) | LUT median (ms) | IBL median (ms) | Composite median (ms) | C++ allocation calls |
| --- | --- | --- | --- | --- | --- |
| Off | 2.544 / 2.715 | 0 | 0 | 0 | 0 |
| Analytic | 2.871 / 3.007 | 0 | 0 | 0 | 0 |
| Physical, stationary | 4.399 / 4.685 | 1.191 | 0.002 | 0.120 | 0 |
| Physical, rotating camera | 4.465 / 4.737 | 1.186 | 0.003 | 0.117 | 0 |
| Moving sun | 5.915 / 6.202 | 1.954 | 0.783 | 0.120 | 0 |

These are software instrumentation measurements of a sky-only scene, **not** RTX
3060 timings or a representative populated-scene budget. Sky drawing is included
in whole-frame GPU time rather than a separate timestamp. All eight probe modes
recorded zero C++ new/new[] calls inside render; this excludes malloc and does not
establish allocation behaviour for every streaming workload. Validation-enabled
probes include layer/driver allocations and are retained separately. The reduced
streaming validation probe settled its 256 m outer coverage in 6.285 seconds.

Raw [performance timings](measurements/rendering_5h/performance_timings.csv),
[per-frame samples](measurements/rendering_5h/performance_frames.csv),
[metadata](measurements/rendering_5h/performance_metadata.txt),
[validation timings](measurements/rendering_5h/validation_timings.csv) and
[small streaming timings](measurements/rendering_5h/streaming_timings.csv) are retained.
Captures include [clear sky](measurements/rendering_5h/physical_static.png),
[sunset](measurements/rendering_5h/sunset.png),
[sky LUT](measurements/rendering_5h/physical_static_sky.png),
[transmittance](measurements/rendering_5h/physical_static_transmittance.png),
[aerial transmission](measurements/rendering_5h/physical_static_aerial_T.png),
[aerial scattering](measurements/rendering_5h/physical_static_aerial_S.png) and
[small terrain coverage](measurements/rendering_5h/streaming_small.png).

A second validation-enabled Release probe used production-size 256-cell chunks,
32-cell patches, a 16 km detailed radius and the 32 km outer coverage cap at
320×180. It completed all modes without validation messages, but did **not** settle
within its 60.008-second cold-fill deadline. The measured frames still had 86–92
pending coverage requests and zero of 20 requested horizon tiles ready. Its
552,967,596-byte terrain reservation stayed bounded; this run establishes neither
full-scale coverage quality nor steady-state streaming performance. Existing
near-coverage priority delays the outer ring while near coverage is still filling.
The validation-enabled allocation counts include layer/driver work and cannot be
compared with the validation-disabled sky-only allocation results above. Retained
[32 km timings](measurements/rendering_5h/streaming_32km_timings.csv),
[frame readiness](measurements/rendering_5h/streaming_32km_frames.csv) and
[configuration](measurements/rendering_5h/streaming_32km_metadata.txt) make this
remaining acceptance limit explicit.

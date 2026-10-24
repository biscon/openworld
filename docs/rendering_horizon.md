# Continuous distant terrain and a natural horizon

Status: implemented. This design implements the approved continuation
of slice 5H; hardware acceptance is separate from automated correctness checks.

## Ownership and representation

The existing streamer owns a protected near hierarchy, shadow reservation and a
separate bounded distant reservation (demo: 128 MiB). Additional dyadic levels
extend visible coverage without changing the detailed view radius or shadow level.
The startup horizon distance is selected conservatively for terrain elevations
bounded by twice the supported height scale and flight up to 2 km above terrain,
with 10% margin and a dyadic ceiling, capped at 1,048,576 m. Device or memory
limitations reduce complete level sets and are reported explicitly.

Distant tiles omit the finest stored height level (128 stored cells per axis for
the demo’s logical 256-cell tile), preserving their logical tile
footprint and filtered coarser samples. They do not request fine normal products.
Parent activation, quadrant suppression and morphing remain shared with near
terrain. The full/compact join and compact tiles have a minimum mesh LOD of one,
so a child’s collapsed grid matches its compact parent. Near finer levels retain
their original representation. Generation, cache ownership, frame work limits and fence retirement stay
in the existing streamer. All request, draw and history capacities are allocated
at startup. Ancestor-first coverage bootstraps the distant roots before dependent
near tiles activate; coverage precedes optional fine-normal refinement. This
preserves watertight parent fallback, but cold-fill latency is an acceptance metric.

Compact GPU buffers use per-reservation offsets/strides; coarse normal images
remain shared. Cache representation 10 serializes only the retained height levels
and metadata. Workers reconstruct the omitted finest scratch grid from triangles
for shared payload validation; frame processing never allocates that scratch.
Generation similarly expands scratch after computing retained levels, with separate
workgroup timing estimates for cheap expansion/bounds stages. Existing cache schema
remains readable, but changed generation shader fingerprints cause a one-time miss.

## Interfaces and defaults

`RendererOptions::curvedHorizon` is opt-in for library users and enabled in the demo.
`TerrainStreamingConfig::horizonBytes` defaults to zero for library users and
128 MiB in the demo; `horizonMaximumDistance` caps the startup hierarchy at
1,048,576 m. The production allocation uses conservative elevations [-4000,4000] m
and a 2 km flight allowance; frame diagnostics evaluate the current height scale,
atmosphere radius/datum and camera altitude. `horizonLimited` reports insufficient
range or an altitude outside the supported envelope. Requested/effective distance,
level count, residency and storage are exposed in `TerrainStreamStats`.

`Camera::renderFar` and `curvature` carry the rendering parameters through existing
renderer subsystems; navigation and `worldPosition` ignore them. The render far plane
is at least 64 km, or 1.5 times the configured horizon cap when curvature is enabled.
`--no-horizon` selects the demo’s legacy flat comparison.

## Render-space contract

Navigation, procedural generation and geographic material coordinates remain flat.
A camera-local spherical mapping curves rendering using the atmosphere radius and
datum. Positions, normals, shadow geometry, visibility bounds, lights and previous
frame positions use this same mapping. The stable vertical expression uses
`-2*sin(angle/2)^2` rather than subtracting nearly equal planet radii. Conservative
CPU bounds include the nonlinear displacement over the entire original box.

The demo enables curvature independently of sky mode. The projection far distance
covers the distant square diagonal; local-light clusters retain their 64 km range.
Temporal history records current and previous mapping parameters. Distant temporal
depth is stored internally in kilometres when curvature is active. Public post
snapshots continue returning metres. Geometry carries both flat geographic position
and the interpolated rendered triangle position: lighting/transport and motion use
the latter, avoiding false motion from curving interpolated flat positions again.
GTAO depth conversion uses the effective far plane. Shadow clipping keeps its own
detailed footprint, independent of the visible outer boundary.

## Curvature precision correction

The native walking-speed review showed wobbling wireframe diagonals with
LOD frozen and TAA disabled. The old GPU mapping changes from a series to
`sin(a)/a` at `a=1e-6`, or 6.36 m at the default radius. This is a numerical
hazard: [Vulkan specifies an absolute error for sine](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#spirvenv-precision-operation), not small-angle relative
accuracy, and division by `a` magnifies that error. The flat vertex and UV checks
do not exercise this final mapping. The user confirmed on RTX 3060 after rebuilding that this correction resolves
the wobble. Remaining CSM edge/filter quality is a separate issue.

The correction replaces both the position and tangent-frame mapping for `a<=0.5` radians with
shared Horner polynomials for `sin(a)/a` and `(1-cos(a))/a²`. Compute vertical
drop from `r*a*cosc(a)`; derive sine/cosine for the frame from those same values.
The first omitted terms bound analytic errors below 2.5e-11 and 2.1e-12 over
this interval, before float rounding. Larger angles retain the trig path without
division by tiny values. This covers the default horizon, preserves geographic
UVs and CPU double-precision bounds, and adds no resources or per-frame work on
the CPU. All colour/depth/shadow/history users share the helper.

GPU regression diagnostics reuse idle terrain scratch through an explicit
blocking fixture, bounded by that scratch capacity. Compare post-curvature
positions and transformed normals against double-precision references across
the origin, the old 6.36 m threshold, walking increments, the polynomial join,
and supported planet radii. Repeat with sine results quantized to 1e-7 to model
bounded absolute error on an otherwise accurate software device; this is not
an emulation of a specific NVIDIA instruction. Check continuity and orientation,
not only a forward/reverse image match. Run SPIR-V and Vulkan validation and
retain native walking review as separate acceptance.

Debug and Release precision, horizon integration and terrain temporal tests pass
with software Vulkan validation. The precision regression fails with the old
formula: the controlled sine perturbation displaces near vertices by up to
0.312531 m at the default radius. The corrected near path is independent of that
perturbation; maximum position error against the double reference is 6.974e-6 m
within 256 m. Both builds' 77 SPIR-V modules validate. These establish the
numerical correction. Subsequent RTX 3060 visual review confirms the wobble fix;
native performance has not been measured. See the
[measurement record](measurements/curvature_precision/README.md).

## Atmosphere and materials

Preserve the existing aerial samples through 64 km and append 32 logarithmic far
samples when curved-horizon rendering is enabled: Normal 32×32×64, High 64×64×96.
Legacy flat callers retain their original cubic volumes. Generation and consumption share the depth
mapping. Surface sunlight uses radial altitude in the curved atmosphere frame.
Opaque and transparent surfaces retain the same transport composition; optional
boundary haze stays zero by default.

`AtmosphereSnapshot` exposes separate volume width and depth.

Blend full terrain materials to broad colours, coarse normals and macro variation
between the detailed radius and 1.5 times that radius. Beyond it, omit micro normal,
height blending and cliff projection costs. Curvature never changes texture mapping.

## Verification and acceptance

Owned CPU fixtures check bounds, mapping, depth distribution, demand/capacity,
retirement and allocations. GPU fixtures check terrain seams, silhouettes, depth,
shadows, Direct/Indirect/Hi-Z and temporal rebasing under synchronization validation.
Independent atmosphere integration checks near/far transport and the lookup join.

After coverage settles, clear-air ground-to-2-km views must show continuous terrain
through the horizon with no exposed straight coverage edge. Cold starts and
teleports may temporarily have missing coverage. Record those delays separately.
Release RTX 3060/1080p target: <=1 ms additional steady-state GPU p95, maintaining
the existing 8.33 ms frame goal and generation allowance. Software checks do not
substitute for native performance or flight review.

## Recorded verification — 2026-10-23

Debug and Release builds completed without compiler warnings. The full suites
passed 37/37 in both configurations (Debug 676.87 s, Release 587.87 s). The full
Debug run preceded the final integration refinements. The final Debug run passed
all six affected GPU fixtures (197.99 s), and focused CPU checks passed in both
builds after the data-level diagnostics fix. All 76 SPIR-V modules in each build passed
`spirv-val --target-env vulkan1.3`.

GPU checks used llvmpipe (LLVM 23.1.3), eight worker threads and Vulkan validation.
The extended aerial reference fixture measured maximum transmittance vector error
0.000754158 and relative in-scattering error 0.000717427 (0.072%). It covers both
sides of the 64 km sampling join and far samples, plus opaque/transparent transport
agreement, 100 km temporal depth and stationary motion, rebasing, AO and shadows.
The non-flat horizon fixture checks compact joins with Direct/Indirect/Hi-Z,
movement/radius reuse, and fallback that preserves near capacity. CPU checks cover
compact cache round trips/corruption, conservative curved bounds, allocation-free
demand and distant data levels beyond the per-tile mesh LOD count.

### Production-sized software probe

The owned 256-cell terrain fixture used an 8.192 km detailed radius, six distant
levels and 1,048,576 m outer coverage at 320×180. All 141 requested distant tiles
were ready in the sampled frames. Distant GPU storage was 70.39 MiB within its
128 MiB reservation; total terrain storage was 561.82 MiB. Physical/Normal
atmosphere storage was 3.09 MiB. Validation reported no errors.

The stream was **not fully settled** at the 180.038 s fill deadline: 27 near
coverage requests remained in the first sampled frames and 19 at the end. These
are cold-generation measurements with the disk cache disabled, not steady-state
performance results. Physical/static GPU median/p95 were 40.80/44.38 ms on
llvmpipe. Allocation counts include validation/driver activity and cannot establish
allocation-free streaming. The separate non-validation sky-only probe recorded
zero C++ `new`/`new[]` calls in all sampled render modes; it excludes `malloc` and
does not establish streaming allocation behaviour.

Inspected captures at camera elevations 500 m and 2500 m show continuous distant
terrain with no exposed coverage edge, with boundary haze zero. They use the owned
palette fixture and small output resolution, so they do not sign off full-resolution
materials, every camera direction or the entire flight envelope.

Records: [production metadata](measurements/rendering_horizon/production_metadata.txt),
[timings](measurements/rendering_horizon/production_timings.csv),
[frame readiness](measurements/rendering_horizon/production_frames.csv),
[sky-only metadata](measurements/rendering_horizon/sky_metadata.txt) and
[sky-only timings](measurements/rendering_horizon/sky_timings.csv).
Captures: [clear air](measurements/rendering_horizon/production_physical_static.png),
[higher altitude](measurements/rendering_horizon/production_altitude_2km.png) and
[atmosphere off](measurements/rendering_horizon/production_off.png).

An RTX 3060 is not exposed in this environment. The 1080p/120 FPS target, <=1 ms
additional GPU p95 and full ground-to-2-km flight review remain unverified.

## Reproduction

Build with `BUILD_TESTING=ON` and `OPENWORLD_GPU_TESTS=ON`. GPU tests require a
Vulkan 1.3 implementation and validation layers. For llvmpipe, explicitly select
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json` to avoid loading unavailable
NVIDIA libraries in the container.

```sh
ctest --test-dir cmake-build-release --output-on-failure
cmake-build-release/atmosphere_probe /tmp/horizon --streaming --width 320 --height 180 --frames 8 --radius 64 --altitude 500 --fill-seconds 180 --require-validation
cmake-build-release/atmosphere_probe /tmp/horizon-small --quick --streaming --require-validation
cmake-build-release/atmosphere_probe /tmp/horizon-sky --quick --width 320 --height 180 --frames 16
```

The probe also accepts `--horizon-distance` and `--flat-horizon` for controlled
comparisons, and records the actual range, storage, readiness and fill deadline.
A timed-out fill is recorded as unsettled rather than counted as successful coverage.

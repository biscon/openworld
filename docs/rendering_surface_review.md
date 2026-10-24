# Stable terrain surfaces, shadow filtering and review views

Status: the initial correction passed software checks but failed native near-ground
review. Its hierarchical geometry transition is superseded by
[stationary ground LOD](terrain_stationary_lod.md); the shadow-filtering, visibility
accounting and bookmark work below remains in use. The old diagnosis and measurements
are retained as history, not evidence of acceptance of the new correction.

## Surface contract

Replace the horizontal odd-vertex collapse with a fixed geographic XZ grid whose
heights approach the parent triangle surface. The parent target in curved mode is
the barycentric interpolation of its three rendered vertices, not the curvature
of an interpolated flat height. Thus a fully transitioned fine patch tessellates
the same rendered parent triangles. Colour, depth, shadows, wireframe and temporal
replay share this calculation. Normal/material coordinates remain geographic.
Use per-vertex distance and residency weights. The geographic grid stays fixed for
the entire partial transition. At the exact parent endpoint, redundant odd vertices
coalesce to their even anchor **only when that anchor has also reached the endpoint**.
This changes no parent surface and avoids subpixel raster cracks at T-junctions.
Coalescing earlier would produce coincident vertices with different heights/normals;
leaving all endpoint vertices separate exposed a one-pixel crack in the existing
hierarchy regression. There is no gradual horizontal squeeze across the morph band.
Selection, filtered heights, topology, cache payloads and protected near/distant/shadow
reservations retain their ownership. Legacy flat streaming samples parent vertices
from its single raw height grid; hierarchical streaming uses its filtered levels.
The vertex-probe build explicitly depends on the production vertex source.

Per-patch metadata already encloses the fine and parent height endpoints. Use it
instead of inflating every patch to its whole tile/parent height range. Curved
bounds contain all endpoint vertices and their convex combinations. Unknown bounds
remain visible. Frozen diagnostic LOD retains its observer and committed range;
the fixed startup draw capacities still apply and exhaustion stays explicit.

## Shadow sampling

The existing four bilinear taps share references and underestimate their receiver
plane footprint. Replace them with the equivalent separable weights over at most
nine unique depth texels, correcting each comparison at its actual texel centre.
Finite plane corrections are not capped to four world texels. Singular/nonfinite
derivatives use the ordinary finite depth/normal bias without plane correction.
Keep five 2048-square maps, 0.75/0.25 texel defaults and bounded resources. The old
manual-filter override remains source compatible; both capabilities use the same
explicit comparisons. Cleared reversed-Z depth (zero) is always unoccluded, even
when a grazing receiver extrapolates beyond the depth interval. The grazing-plane
fixture caught this case. Existing contact/real-occluder tests remain in place.

## Visibility accounting

Keep conservative current-frame Hi-Z and its full depth prepass. Per-frame batch
counters retain depth and opaque-colour triangle totals, split
between terrain and meshes. Read only after the existing frame fence; tag completed
counts with their originating mode. Direct/fallback paths account for actual CPU
submissions. All storage is reserved at startup, with 64-bit CPU totals. CPU terrain
selection and per-LOD triangle totals are labelled separately from completed GPU
opaque-colour submissions and shadow raster instances. No synchronous frame readbacks or
previous-frame occlusion decisions are introduced. Each batch uses three `uint4`
records: colour draw/rejection counts, depth triangles, and colour triangles. Each
triangle record has low/high uint pairs for terrain and meshes, so no GPU int64
feature is required. Counters cost 48 bytes per reserved candidate per frame,
32 bytes more than before (4 MiB additional at 65,536 capacity and two frames).
Transparent meshes remain on their separate sorted draw path. Hi-Z rejects whole
patches, not individual hidden triangles. Broad or partially visible patch bounds
can still prevent rejection; lower colour counts do not remove depth-prepass work.

## Saved review views

A small versioned JSON value owns the camera, terrain settings/origin and scene settings, frozen
LOD observer/range and reproducibility-critical startup settings. Explicit Save/Load
buttons use a file in the application's user-settings directory; `--review-view`
allows the demo and probes to replay a supplied file. Save replaces atomically;
load parses and validates a temporary value before applying it. Runtime startup
mismatches leave the current scene untouched and report a CLI replay instruction.
Startup replay configures the renderer before construction. Loading resets temporal
history. JSON/file work occurs only on explicit actions, never steady-state frames.
This is a diagnostic bookmark, not a scene/resource serialization framework.
`review_view` is a separate C++ library using the existing nlohmann JSON dependency;
only demo/probe actions parse files. `ReviewView` owns settings, while the renderer
exposes a small `TerrainLodReviewState` value (observer, scale, frozen focal length).
A file is limited to 1 MiB, uses schema version 1 and validates finite values, enums,
terrain settings and supported resource ranges before mutation. Failed saves leave
an existing file intact; a same-directory temporary file is renamed on success.
There is no automatic load or save; `--save-review-view PATH` explicitly saves the final view on exit.

The default file is `review-view.json` under `SDL_GetPrefPath("OpenWorld", "OpenWorld")`
(typically `~/.local/share/OpenWorld/OpenWorld/` on Linux). The UI reports its path.
A supplied `--review-view` path is also used by Save/Load. Replay takes precedence
over renderer/scene CLI defaults; process-local window, validation, shader/cache
paths and timing options remain caller-owned. Extent/render height are recorded;
headless replay uses them, while live loading retains the current window extent.
Terrain/material startup mismatches require restart. Material directory is a reference,
not an asset bundle. Generated lighting-demo meshes/lights, streaming progress and
GPU temporal history are intentionally outside this terrain-review bookmark.

```sh
cmake-build-release/engine --review-view /path/review-view.json
cmake-build-release/shadows_probe /tmp/shadow-review --review-view /path/review-view.json --flight-speed 1
cmake-build-release/visibility_probe /tmp/visibility-review --review-view /path/review-view.json --flight-speed 5 --require-validation
```

Probe replay preserves the saved startup settings, including dimensions even with
`--quick`; quick still shortens the measurement. The shadow probe compares Off/CSM;
the visibility probe compares Direct, Indirect, prepass and Hi-Z on the same saved
camera path. `--flight-speed` is metres/second (shadow probe moves X at that speed
and Z at 0.4 times that speed; visibility replay moves along negative world Z).

## Verification

Use owned plane/ridge fixtures across morph intervals, LOD and data-level joins,
curvature, rebasing and residency transitions. Check parent-plane coincidence,
non-inverted projected triangles, bounded endpoints and shared edges. Compare
slow 1/5 m/s flights, frozen observers, wireframe, plain and textured shading with
TAA/shadows off and on. Test shadow slope/subtexel sweeps against planar references,
real occluders, contacts and cascade overlaps. Ridge-hidden terrain must actually
reduce colour triangles while matching unculled output. Exercise direct/indirect
fallbacks, counters, no allocations and review-file failure/round-trip behaviour.
Run Debug/Release checks, SPIR-V and Vulkan validation. Preserve the 1080p/120 FPS
and shadow performance targets; software Vulkan does not establish native acceptance.

### Recorded results

#### Follow-up: native review rejected the transition quality

The subsequent native screenshots still show a camera-centred changing wireframe
band and apparent texture motion at 2 m/s, with shadows disabled. A saved view at
chunk (9,15), local (61.958796,619.603861,116.658758), yaw 2.309998, pitch -0.205,
default terrain and range scale 1.517789 supplied a reproducible investigation.
An isolated software replay preserved the 256-cell/0.5-m grid and frozen range,
reduced outer coverage to 512 m, and disabled shadows, TAA and curvature. Residency
settled before measurements. Production GPU vertex readback agreed with the
current partial-morph formula within 0.000041 m in the sampled level-zero tiles.
This confirms the formula is being executed; it does not establish visual quality.

In the selected transition vertices, fine-to-parent height differences reach
approximately 0.171 m at absolute level zero and 0.702 m at level one. Vertical
morphing therefore remains camera-dependent even though partial-transition XZ is
fixed. Exact-endpoint coalescing also still changes wireframe topology. Neither
condition was excluded by the previous checks for connectivity and fixed partial XZ.

The quality metadata currently measures `abs(current-reference)` but omits
`abs(parent-reference)` despite including both in culling bounds. Consequently its
pixel-error diagnostic does not bound the surface throughout the morph. The shared
range scale and twice-baseline geometry cap impose further limits. A follow-up
design must account for the actual transitioning surface and grazing-view motion,
and validate the saved view before claiming the moving band is fixed. No further
production shader change was made during this diagnosis. The CSM issue remains a
separate native-review item; this replay used shadows off.

#### Initial automated verification

Debug and Release suites passed all 38 tests (20 GPU tests) with llvmpipe and Vulkan
validation. A final five-test Debug rerun covered the flat-stream compatibility
fix, LOD, patch bounds, review files and CLI. All 76 generated SPIR-V modules in
each build passed `spirv-val`. The geometry regressions check geographic positions,
parent endpoints, shared edges, normal coherence and non-inverted triangles;
existing hierarchy captures also check for sky cracks. Grazing shadow-plane sweeps
and existing real-occluder/contact checks passed. The visibility fixture submits
2,048 terrain triangles to depth and zero to colour behind an opaque wall, with
the same resulting HDR image as the unculled reference.

An explicit save/restart/replay produced byte-identical captures. Saved-view slow
flights at 1 and 5 m/s and frozen top-down wireframe replay completed with validation.
Those plain-terrain smoke captures do not establish textured mountainous appearance
or temporal quality on the user's camera path. Native inspection remains necessary.

[Measurement records](measurements/rendering_surface_review/README.md) contain
the owned replay settings and short before/after software probes. CSM median scene
GPU time was 3.702 versus 3.713 ms; this is not a native GPU cost estimate. The small
visibility flight rejected no patches before or after, so it establishes no culling
speedup. The tighter bounds enable rejection without changing the conservative
whole-patch rule. The current-frame depth prepass and global LOD triangle budget
remain; neither a large reduction in selected triangles nor RTX 3060 frame-time
acceptance has been demonstrated by these software runs.

# Stationary ground terrain LOD

Status: implemented; verification results are recorded below. Supersedes the radial
hierarchical geometry morph and shared quality-range controller; fixed/flat diagnostic rendering remains
the reference path. Native acceptance is separate from software checks.

## Ownership and representation

The hierarchical selector owns a bounded, persistent partition in integer world
sample coordinates. Resident tiles provide immutable height pyramids and metadata;
they do not define the rendered geometry transition. Each selected quadrant is an
independent half-patch grid. Reusable 16 edge-stitch variants remove redundant odd
edge indices. Interior vertices never move with the observer. Shared edges and
corners use the coarsest incident absolute height level; edge/corner neighbours are
balanced to one level. Curved positions use those same canonical vertices in all
passes. No radial collapse, height morph, or residency height fade is applied to
stationary draws. Missing data retains coarse coverage and is reported.

Selection occurs before frustum culling, with world-keyed split hysteresis. Distance
provides a baseline, a 64 m horizontal protected region retains finest available
geometry, and projected replacement error refines above 1 pixel/coarsens below
0.5 pixel. Optional error-driven quality also uses the user approximation target.
Metadata bounds both representations and stores current/parent reference errors
and their displacement for both channels; cache representation changes with layout.
Triangle budget defaults to 2,097,152 (`--terrain-triangles`). Protection and
balancing, then replacement safety, precede baseline and optional refinement. Impossible budgets or unavailable fine data
report protected-ground shortfalls; complete coarse coverage is retained. Root
coverage may itself exceed a deliberately undersized allowance, and no further
splits are admitted in that case. The allowance bounds refinement rather than
silently dropping coverage. Storage is reserved from the smaller of the budget and
maximum possible resident patch count, subject to draw capacity. Constructor checks
reject insufficient root storage and configured CPU/GPU budgets. Geometry selection
and neighbour closure use preallocated storage, integer-key lookup and bounded priority work, never all-pairs scans.

The streamer retains its existing coverage prefetch ring and motion-predicted,
protected-neighbour normal demand. Selection never waits for a fine-normal job.
A child publishes only when its existing readiness and neighbour dependencies are
complete. Readiness latches: cancelling partial preparation cannot publish a child,
and returning demand cannot hide a child that is still awaiting retirement. Resident slot references and parent pins remain fence protected.

Shading normals have an independent geographic sampling field. Three adjacent
normal levels per provider cover independent geometry refinement; normal availability
may blend, but that blend cannot move the surface. The colour, depth, shadow,
wireframe and temporal paths consume the same draw records and topology. Stitch
changes participate in temporal identity, rejecting incompatible history.

The selector exposes immutable patch keys for review save/load and diagnostics for
budget, transition error, refinement and protected coverage. Existing v1 review
files remain loadable; new bookmarks retain frozen leaf selection. GPU buffers and
resident slots keep existing fence retirement. New CPU/index storage is allocated
at startup and included in managed memory accounting.

## Verification and acceptance

### CPU preparation cost

Native review after the wobble correction reports lower FPS even with shadows off
and substantially fewer visible triangles. Selection operates on the complete
partition before culling, so visible triangle count does not bound its CPU cost.
An isolated 14-level, 4,096-patch workload measured about 3.2 ms for Release
selection and 0.7 ms for 4,095 temporal records on the available CPU. These are
synthetic CPU measurements, not attribution of the complete native frame time.

The correction keeps topology, refinement thresholds, the protected region and
triangle allowance unchanged. Neighbour/history coverage searches start at the
query patch's level, then check nearby levels before searching the remainder.
Every possible level is still checked on a miss; no assumption about balanced
startup residency is needed. The disjoint partition permits at most one matching
leaf, so lookup order cannot change the result. Once even one split cannot fit,
reject it before collecting neighbour closures. The complete closure check still
applies when a split might fit. Exhausted-budget attempts are counted as budget
limits without first searching for an additional residency limitation. No new
selection storage is needed.

Terrain temporal history retains submitted/pending record ownership and full-key
identity checks. It sorts a preallocated array of 32-bit indices into the previous
records instead of moving 432-byte records during sorting. This adds at most four
bytes per configured history slot (256 KiB at 65,536 slots), with no frame-time
allocation or GPU layout change. Reset, abandoned preparation and bounded overflow
must retain their existing behaviour.

Validate with owned mixed-level selection/temporal regressions, allocation checks,
unchanged selected-patch fingerprints, a separate CPU benchmark at 0.5M/2M triangle
budgets, and terrain temporal/horizon GPU integration under Vulkan validation.
The saved mountain view is additional review evidence. Measure native total frame
time before attributing the remaining cost to geometry, shading or TAA resolve.

The lighting window now exposes streamed-terrain CPU selection, draw preparation
and submission finalization times. These exclude fence/acquisition/presentation
waits. Selection includes resident weight updates and the complete partition;
preparation includes batching, temporal correspondence and visibility records;
finalization includes submitted-history indexing and residency publication.
They describe current CPU work, alongside the existing completed GPU timers.

Implemented and measured with `terrain_cpu_probe` (owned workload, outside CTest).
In the 2M-budget workload, Release median selection drops from 3.06 to 1.24 ms and
4,095-record temporal history from 0.65 to 0.49 ms. All selected patch and stitch
fingerprints match across 480 moving frames. See the
[measurement record](measurements/terrain_cpu_preparation/README.md) for settings
and limitations. Native Release FPS acceptance remains pending.

Owned plane, slope, corrugation and ridge tests verify static world positions,
topology coverage/winding for every stitch combination, edge/corner balance,
selection hysteresis, bounded work, allocation-free selection and budget failures.
GPU checks include signed/large coordinates, curvature, residency changes, fixed
geographic surface queries, shared vertices and crack-free captures. Test both
quality-toggle states and TAA/shadows on/off.

The saved native camera at chunk (9,15), local (61.958796,619.603861,116.658758),
yaw 2.309998/pitch -0.205, default terrain, supplies a separate visual review.
Replay 2/6/30 m/s forward/reverse ground paths, chunk boundaries, freeze/unfreeze,
plain/textured/wireframe and cold/warm residency. No circular moving band or
travelling ridges is the acceptance criterion; matching a shader formula is not.
Run Debug/Release, SPIR-V and Vulkan validation. Record CPU/GPU time, allocations,
memory, triangle counts and budget limits; the RTX 3060 1080p/120 FPS target is not
established by software Vulkan.

## Replay tool

`terrain_ground_probe OUTPUT REVIEW [HEIGHT [DISTANCE [FPS]]]` loads an explicit
bookmark, renders plain/textured/wireframe comparisons with additional error
refinement off/on, checks a frozen bookmark round trip, then follows the sampled
finest ground at 2/6/30 m/s for four seconds forward and four back. Defaults are
1080 pixels, the saved coverage distance and 30 Hz; FPS 0 selects static captures
only. A reduced coverage run omits
the distant bank and explicitly records its resolution/distance. Terrain samples
and captures are outside timed render calls; software GPU/CPU timings are diagnostic,
not native performance claims. The tool owns its cache under OUTPUT; the supplied
bookmark may reference the built terrain palette. Routine tests use owned data.

The demo now enables opaque depth prepass at startup; its checkbox and
`--no-depth-prepass` retain the comparison path. Hi-Z/GTAO still require depth.

## Recorded verification

Debug and Release builds succeeded. All 38 registered test targets passed across
full-suite runs and focused reruns; both sets of 76 shader modules passed SPIR-V
validation. The final streaming publication correction passed its CPU regression
in both builds, the Debug hierarchy GPU suite, and the Release ground replay.

The saved camera was inspected in plain, textured and wireframe views at 540 and
1080 pixels. Final 2/6/30 m/s forward/reverse paths in both quality modes returned
pixel-identical images, with no missing protected ground, allocation growth or
Vulkan validation errors. These are reduced-coverage software checks, not native
acceptance. The triangle budget still limits some replacement changes: maximum
estimate 1.352 px during the 540-pixel paths and 2.604 px at the fixed 1080-pixel
view. Small distant LOD pops remain possible under that budget; continuous camera-
driven geometry deformation is removed. Full settings, captures, per-frame
measurements and limitations are in the [review record](measurements/terrain_stationary_lod/README.md).

Subsequent native review still found moving wireframe diagonals and apparent
texture swimming with frozen LOD and TAA disabled. Flat-position and round-trip
checks do not establish stability of the final curved surface. The follow-up
[curvature precision correction](rendering_horizon.md#curvature-precision-correction)
tests that final transform under bounded absolute trig error. Subsequent RTX 3060
review confirms that the curvature correction resolves the wobble and apparent
texture swimming. Full-scale performance and remaining shadow quality remain
separate acceptance items.

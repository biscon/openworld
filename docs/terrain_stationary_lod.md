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
tests that final transform under bounded absolute trig error. Native acceptance
of the combined result remains open.

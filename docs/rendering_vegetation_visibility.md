# 5J-V4: engine integration, visibility and range

Status: implemented and verified in the ordinary engine with native Vulkan validation.
[Measurements and remaining tuning gates](measurements/vegetation_5j_v4/README.md)
record default/max-range cost, moving CPU limits and the existing GTAO test failure. This is the engine
integration slice of [the vegetation roadmap](rendering_vegetation.md). V5 adds
interaction and habitat variety; V6 performs final tuning and extended acceptance.

## Engine contract

The normal hierarchical terrain engine enables vegetation at startup. Rendering /
Vegetation opens a dedicated settings window with enable, habitat, density, wind,
render distance and completed diagnostics. Distance is 20–200 m, initially 80 m,
and controls actual visibility rather than merely clamping the old asset limits.
Close population density is preserved. Explicit review bookmarks save these settings;
ordinary UI persistence continues to save window state only. No new viewer is added.

## Residency and ownership

Keep 4 m cells, 128 candidate identities per cell, canonical suitability and rendered
triangle attachment. Runtime demand is max(render distance, caster distance) +16 m;
retention adds another 16 m. Neither radius nor quality changes population identity. The engine preallocates
12,288 slots and allows 256 MiB of actual GPU allocations including both frame
attachments, selection lists/scratch, assets, staging and retirement. Smaller owned
fixtures retain explicit capacities. Expansion schedules outward work; contraction
changes rendering immediately and retires cells only after submitted readers finish.
Near cells and attachment repairs have priority; distant work defers under pressure.
The 32-cell update cap and shared terrain generation allowance remain in force.

Terrain surface lookup must cover the maximum demand envelope, and required canonical
providers must be requested within existing bounded terrain storage. Missing or
overflowing surface queries hide pending cells rather than producing guessed roots.
Streaming does not upload placeholder instance populations. Large readbacks are
diagnostic-only. Steady-state buffers and CPU containers never grow.

## Rendering

Cell rejection precedes individual instance reads. Count/prefix/scatter builds bounded
asset/LOD/material lists with deterministic truncation. Camera and CSM lists are
independent, including their fade distances; grass never enters casting work. Bounds include scale, wind, slope and
curvature. Near-plane or uncertain Hi-Z tests retain candidates.

Near density and existing thinning are retained; extra thinning starts beyond 80 m.
Projected size supplements species LOD distances, with identity-staggered transitions.
Density and final fade use matching stable masks in colour, depth and shadow passes.
Shading, fine flutter and shadows have independent limits. Previous submitted wind,
camera and settings determine motion; incompatible/new geometry rejects history locally.

Stable habitat/exclusion inputs are shared with terrain coverage; material appearance
does not depend on rendered populations or camera distance. Existing grass/soil assets
supply the ground exposed by fading vegetation.

Optional vegetation Hi-Z defaults off and borrows the frame-owned solid-depth pyramid.
Pass ordering is updates/casters, CSM, solid depth, shared Hi-Z, vegetation camera
selection, vegetation depth, GTAO, colour/motion, atmosphere/volumetrics, temporal post.
There is no vegetation self-occlusion dependency or second pyramid. Without vegetation
Hi-Z, camera selection precedes the combined depth pass. Disabled vegetation adds no
depth-pass requirement. Resource barriers and descriptor lifetimes follow frame fences.

## Verification and acceptance

Owned CPU tests cover live range changes, identity preservation, capacity, retirement,
invalid settings, bookmarks and UI registration. Owned GPU tests cover longer ranges,
near density, masks/LOD, conservative culling, offscreen shadows, Hi-Z/GTAO, temporal
history, stitched attachment, teleport/return, overflow and both material bindings.
Build Debug/Release and validate affected shaders and Vulkan synchronization.

Review the ordinary engine at ground/grazing/low-aerial views, with motion, terrain
refinement, wind, low sun, TAA and optional GTAO. Extend existing measurement tooling
for off/on pairs at 80/200 m, one occlusion comparison and a bounded cold/warm/teleport
route. Report median/p95 CPU/GPU and frame time, per-pass costs, allocations and memory.
Targets remain about 1.5 ms incremental GPU and .25 ms CPU at default range; 200 m is
reported separately. Do not trade away nearby density to pass a benchmark. Hardware
and live appearance checks are required for acceptance; unavailable checks are pending.

## Implementation details

Camera workgroups are compacted near-first from ready residency cells into frame-owned
uploads. A group first rejects its cell, then classifies candidates once. A frame-owned
choice buffer stores the chosen first bin and part count. Group/bin histograms are
scanned by one lane per bin, followed by bounded indirect command construction and
stable group/lane scatter. Prefix/choice scratch is reused serially between views with
read/write barriers. This removes repeated per-instance classification and makes
capacity truncation deterministic. Sparse caster indices remain immutable and exclude
grass. Both frames own all mutable work, descriptors and attachments.

The borrowed Hi-Z image descriptor is finalized before caster commands first bind the
vegetation descriptor set. The image is built later from solid depth; occlusion uses
the exact raster projection while frustum culling uses the conservative projection.
Uncertain/near-plane bounds stay visible. The no-Hi-Z shader variant requires no image.

The surface index covers 111×111 geographic cells, preserving the 16-patch-per-cell
limit and explicit overflow deferral. Canonical terrain demand appends bounded
near-first data-only requests after normal rendered dependencies; it neither forces
fine rendering nor grows terrain pools. Terrain budgets may still defer those requests.

The library format remains version 2. The live distance overrides authored fade-end
hints, while authored LOD and caster limits remain independent. Explicit review saves
use version 3; v1/v2 bookmarks acquire the 80 m enabled default. CLI vegetation options
override a loaded startup bookmark. Fixed single-chunk inspection stays supported.

Shared habitat probabilities and exclusion evaluation retain the V3 population rules.
Terrain rebalances the existing grass/soil layers by expected habitat coverage, leaving
rock/gravel intact. It does not repaint a camera-centred ring or use surviving instances.
This is an appearance approximation, not per-blade terrain shading or a new biome system.

The terrain surface index owns a preallocated snapshot of patch records. Its partition
signature and per-cell patch ordering are independent of render traversal order, so moving-camera priority changes
do not invalidate otherwise unchanged attachment caches. Provider generation, epoch,
geometry and stitched topology still invalidate them. Snapshot storage is charged to
the terrain host allocation; buckets never borrow indices into a reordered render list.

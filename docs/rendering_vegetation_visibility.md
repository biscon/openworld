# 5J-V4: engine integration, visibility and range

Status: implemented and verified in the ordinary engine with native Vulkan validation.
[Measurements and remaining tuning gates](measurements/vegetation_5j_v4/README.md)
record default/max-range cost, moving CPU limits and the existing GTAO test failure. This is the engine
integration slice of [the vegetation roadmap](rendering_vegetation.md). V5 adds
interaction and habitat variety. [V6 documentation closeout](rendering_vegetation.md#5j-v6--final-tuning-and-extended-acceptance) is complete; final tuning and extended acceptance are deferred to the combined vegetation/tree scene.

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
12,288 slots and allows 256 MiB of actual GPU allocations including current/previous
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
Plants and legacy grass use matching stable masks in colour, depth and shadow passes.
New grass libraries use the geometric fades described in [grass stability](#grass-lod-and-shading-stability).
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
grass. Both frames own mutable selection work and descriptors. Shared attachments
use [continuous publication](rendering_vegetation_streaming.md#continuous-attachment-publication)
with explicit queue barriers and previous-repair snapshots.

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

## Grass LOD and shading stability

Status: implemented; native verification recorded below. This replaces the grass LOD and stochastic fade described above for newly built assets; existing version-2 libraries retain their previous behavior.

Near grass stays unchanged through 15 m. The offline builder creates nested blade prefixes (default 24/8/4, generally N/ceil(N/3)/ceil(N/6)) with 3/1/1 segments. Coarse blades expand to approximately preserve summed surface area, capped at six times authored width. Single-draw morphs retire blades by shrinking their width and widen/straighten survivors to match the next mesh. Bands are 18–28 and 38–55 adjusted metres, staggered by a stable per-instance ±2 m; bin changes occur only after the morph completes. Scale and unjittered focal length adjust distance beyond the protected 15 m zone. Forced LOD inspection shows settled meshes.

An optional grass profile owns counts, transition bands and shading controls. Each participating vertex has a separate static 64-byte morph record: current centre/blade ID, next centre/width ratio, next normal, and next deformation weights. Nonparticipating vertices receive neutral records in the GPU stream. Library format 3 stores these fields, with format 2 still readable. The importer hashes the metadata. The existing geometry allocation and staging budget include the second stream; there are no per-instance additions, new frame allocations, extra draws or fragment terrain fetches. Bounds enclose the widened meshes and all interpolated targets; existing wind/slope envelopes still apply.

Grass density/range fades use blade width instead of fragment noise. Candidate identities, thinning and render range remain unchanged. Camera depth and colour share the geometry. Motion evaluates the previous submitted distance, focal length, coverage, wind and geometry; newly appearing blades and incompatible LODs reject history. Grass remains excluded from shadow casting.

From 15–40 adjusted metres, grass lighting normals blend at most 40% toward the attached terrain macro-normal, roughness approaches max(authored, 0.95), and normal-map detail fades out. Diffuse, specular and transmission use the filtered normal; shadow bias retains the original surface normal. Other plants and wind animation remain unchanged. This targets moving geometric and lighting aliasing but cannot guarantee pixel stability for unresolved geometry.

Validation: owned CPU checks for nested geometry, endpoint correspondence, envelopes, malformed metadata and version-2 compatibility; existing native GPU coverage extended across morph/fade boundaries and submitted motion. One engine-view before/after comparison with SMAA (wind and camera movement), brief FXAA/TAA sanity checks, and one short 1080p timing sample. No standalone viewer or broad benchmark matrix.

Verification (2026-10-24): Debug and Release engine builds and asset imports pass. Owned library, population, streaming, legacy vegetation, foliage and imported-texture integration checks pass on the RTX 3060 with Vulkan synchronization validation; 42 affected SPIR-V modules validate. CPU fixtures cover 8/24/25/64 blades, morph endpoint/deformation correspondence, intermediate envelopes and v2 read/re-save. The GPU fixture checks both LOD bands, opaque depth/colour agreement, geometric range fading, submitted motion and history rejection. At a fully morphed endpoint, subdivided and unsplit triangles agree within two raster edge pixels (one observed at the first boundary).

A single 1080p/80 m engine-view sample (SMAA, 32 warmup + 120 measured frames) recorded grass depth/colour GPU medians of 0.369/0.860 ms before and 0.574/1.245 ms after: approximately +0.59 ms for the denser representation and morph stream. Vegetation memory increased from 151.955 to 152.100 MiB, with zero steady-state render-thread allocation calls. Full-frame medians were 6.47/6.23 ms; this short sample does not establish a total-frame speedup. Matching pan, stationary-wind and forward-movement sequences plus brief FXAA/TAA captures are in `/tmp/ow-grass-before` and `/tmp/ow-grass-after`. Captures confirm fuller distant coverage; raw temporal pixel differences also include actual movement and changed coverage, so no shimmer-reduction percentage is claimed. The user subsequently confirmed stable stationary TAA and preferred it over the spatial modes; TAA is restored as the engine startup default.

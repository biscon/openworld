# 5J-V3: streamed population and terrain attachment

Status: implemented. V1/V2 remain supported. This design implements
the V3 slice of [the vegetation roadmap](rendering_vegetation.md).

## Ownership and identity

A Vulkan-independent residency manager owns 4 m geographic cells, generation-checked
slots, bounded demand and retirement. Each of at most 4096 slots reserves 128 GPU
candidates (126 grass, one fern, one shrub). Integer cell coordinates, world seed,
species and candidate index determine candidates; GPU slot numbers do not. Shader
32-bit identities are variation hashes, not unique world keys. Population revisions
include terrain generation, habitat and exclusion settings. Render density and LOD
do not change population. Demand is a 96 m circle with 112 m retention; nearest
cells win capacity pressure. Pending and retired cells count against the same pool.

GPU generation writes compact 32-byte records in deterministic slots. Scale zero
marks rejected candidates. Habitat presets use the saved grass/fern/shrub assets;
versioned weights and a bounded world-positioned clearing input are replaceable by
future habitat/mask providers. Canonical level-zero heights determine elevation and
slope suitability. Missing canonical inputs defer generation. Rendered surface
changes only update attachment, never population acceptance. There is no disk cache.

## Terrain boundary

The terrain stream exposes read-only nearby stationary patch records, before camera
culling, plus canonical provider ranges. Records identify height-buffer offsets,
geometry level, stitched edges/corners, geographic origins and provider revisions.
Queries retain all readers through the existing terrain submission serial/lastUse
retirement mechanism. No height readback participates in frame processing. Attachments use the flat
stationary triangle before the existing curvature transform; this is a local ground
vegetation path, not a new distant curved-surface attachment system.

Attachment evaluates the actual containing triangle, using shared stationary vertex
and stitched topology helpers. Root height and surface orientation live separately
from population, in two frame-owned 16-byte attachment buffers. Footprint samples
reject departures above 2 cm for grass/ferns or 5 cm for shrubs. Missing inputs hide
the cell. Attachment changes invalidate temporal correspondence locally. Cached
attachment signatures include provider generation/revision, topology and comparison
mode; normal texture changes alone do not move roots. A bounded CPU surface cache (~4.3 MiB
at 4096 slots) reuses records while the provider/patch partition is unchanged. A
51×51 cell index admits at most 16 overlapping patches per cell, including a 1.5 m
footprint margin; unsupported/overflowing queries defer explicitly. The controlled
exclusion input is a world-positioned circular clearing, not an authored mask editor.

## Scheduling, storage and rendering

All arrays, uploads, work lists and descriptors are preallocated. Cell records and update jobs are prepared in cached CPU memory, then
copied once into mapped GPU uploads; frame processing never reads those uploads back.
CPU work is per
cell; selection/drawing reuse V2, including sparse caster work and grass exclusion.
Population slots are rewritten only after their last submitted reader completes.
Attachment buffers are writable only after their frame fence. Teleports discard
queued demand, not submitted resources. Failed/stale queries defer work.

Terrain publication/selection precedes vegetation updates, then selection, CSM,
depth and colour. Compute barriers cover population/attachment writes and subsequent
selection/vertex reads. An attachment repair reservation reduces terrain generation's
existing 3.6 ms working allowance; new population uses the remainder. Vegetation is
limited to 32 cell updates and an estimated 0.5 ms per frame, with completed timing
feedback. Updates deferred by the budget remain hidden instead of floating. Actual
allocation requirements, including staging, attachments and diagnostics, must fit
128 MiB. There is no new frame wait, per-plant CPU submission or steady-state growth.

The ordinary demo loads assets at startup but starts vegetation disabled. Settings
expose enable/preset/density and residency/work diagnostics. Legacy fixed/flat terrain
and immutable fixture paths remain separate supported inputs; V3 streamed attachment
requires stationary hierarchical terrain. V4 owns new visibility/LOD and terrain
coverage blending; V5 owns richer habitats and interaction.

## Verification

Owned CPU fixtures cover signed/large coordinates, order independence, quality versus
population identity, capacity, stale handles, cancellation and retirement. Small GPU
fixtures compare roots against actual indexed triangles, including stitched edges
and corners, provider changes, unavailable inputs and local history rejection. Test
both material binding paths and frames in flight with synchronization validation.

Build Debug/Release and validate affected shaders. Run focused GPU correctness once,
then only rerun affected failures. Native checks are one 1080p off/on pair (120 warmup,
600 measured frames each) and one bounded cold/warm/refinement/teleport route. No
default per-frame CSV or full V2 matrix. Planned GPU execution is capped at five
minutes; unavailable or unfinished checks remain explicit. Targets remain 1.5 ms
incremental GPU, 0.25 ms CPU and 128 MiB, not claims of achieved performance.

## Usage and measured verification (2026-10-24)

Build/run `cmake-build-release/engine --vegetation`, or enable Vegetation under
Rendering Settings. Default startup stays off. Habitat and density controls operate
without renderer reconstruction. Existing terrain review bookmarks preserve the
current vegetation settings when loaded; vegetation settings are not serialized by
the legacy bookmark format in this slice. The saved V2 library is loaded at startup.

`VegetationOptions::streaming` selects the streamed path alongside `library` and is
mutually exclusive with `population`/`fixture`. Capacities are startup-only. Streamed
terrain must be hierarchical at 0.5 m spacing with an extent divisible by 4 m;
unsupported configurations fail before Vulkan startup. The terrain generator's existing
noise-lattice coordinate limit still depends on the chosen terrain wavelengths. Runtime habitat/exclusion
values are validated and included in population revisions even if a caller forgets
to increment its explicit revision. `diagnostics` opts into bounded instance/root
readback storage; ordinary application frames never read instances back.

[The compact native report](measurements/vegetation_5j_v3/README.md) records the short
paired GPU check, the CPU upload/cache correction, and remaining limits. The final
stationary CPU sample is 0.095 ms median / 0.127 ms p95 with zero render-thread C++
allocations, zero population/attachment updates and 72.62 MiB charged GPU storage.
The paired native GPU delta before the CPU-only cache correction is 1.205 ms.
These are controlled streamed-meadow results, not full-scene or reference-art acceptance.

Small owned tests check signed/large GPU identities, actual sloping indexed triangles,
forced stitched LOD/coarsening and recovery, local history rejection, density
independence, clearing exclusion, teleport/return, disable/re-enable, fixed/bindless
materials, and CPU retirement/pressure/stale handles. Debug and Release builds and
focused CPU checks pass. Native Vulkan validation results are recorded in the report;
software Vulkan and a long stress matrix were deliberately not rerun.

Footprint conformity uses four directional samples and a local base-plane shear;
it is not per-vertex ground conforming geometry. Steep/unsupported footprints are
suppressed with their candidate identity retained. V4 still owns production visibility,
terrain coverage/fades and longer species ranges; V5 owns habitat art/interaction;
V6 retains full-scene quality, moving-frame CPU spikes and longer stress acceptance.

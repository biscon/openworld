# Hierarchical terrain residency

Milestone 4 extends the milestone 3 streamer. The application enables hierarchical mode; the flat streamer and fixed-neighbourhood renderer remain diagnostic baselines. See [streaming](terrain_streaming.md) for cache workers, mailbox ownership, transfer slicing and fence retirement, which remain in use.

The [ridge LOD correction](terrain_lod_quality.md) supersedes the original point-sampled height representation and fixed-only distance ranges described below: filtered height levels, endpoint-preserving height morphing and bounded error-driven range extension are now enabled.

## Identity and representations

`TerrainTileKey` contains signed X/Z coordinates and an unsigned level. Level zero uses the startup grid (normally 256 cells, 128 m extent and 0.5 m spacing). Each higher level doubles spacing and extent, retaining the cell count. Parent coordinates use mathematical floor division, including negative coordinates. Camera coordinates and the view slider remain in base chunks. At the default footprint, radius 64 is 8.192 km outward, and radius 128 is 16.384 km. Startup `maximumDistance` allows smaller owned test hierarchies; the application cap is 16.384 km.

Data tiles are separate from shared 32-cell rendering patches. A higher-level data tile can contain several geometry LODs. The application retains level-zero 0.25 m normal samples and the established erosion/crease settings. A tile's identity does not depend on its current visibility, fade or selected rendering LOD.

Hierarchical coverage contains base/eroded heights, conservative sampled-surface bounds and one coarse normal level. Fine-normal products contain the remaining normal levels. Coverage now stores a compact filtered height pyramid and sampled error metadata. Level zero evaluates the original world; coarser absolute levels use a bounded 5x5 footprint over the same full-octave world function. It does not generate descendants or a dense fine-normal pyramid. See the ridge LOD design for layout and filtering details.

Canonical sampling addresses quarter-metre world coordinates using a signed 64-bit integer represented by two 32-bit words. Shader noise-domain conversion multiplies the coordinate by the exact binary frequency using three integer limbs, then extracts lattice and fraction. This avoids shaderInt64/float64 requirements and makes samples at the same absolute filtered level use the same arithmetic regardless of tile origin or data representation. The existing supported noise-lattice range is still checked; world coordinates are not unlimited. The representation uses 24 fractional bits in noise space. GPU fixtures compare matching absolute-level heights exactly; finest-level generation retains the established world.

Normal level zero retains central differences at 0.25 m. Higher absolute normal levels use central differences at half their output spacing, followed by a normalized 3×3 [1,2,1] tent. This is a bounded kernel, not recursive filtering down from every underlying 0.25 m sample. Each 8×8 output group shares its height stencil; filtered normals use 21×21 original height samples; the geometry-height blur is deliberately excluded to retain sub-mesh lighting detail. Halos and world alignment make the same absolute normal level agree across different data-tile levels. Cache representation and the tile level are part of the header identity; incompatible entries regenerate rather than migrate. The filtered representation is now 7 following the [ridge LOD correction](terrain_lod_quality.md); startup diagnostic unfiltered pyramids use representation 8.

## Demand, ownership and pressure

`TerrainStreamer` owns preallocated slots, request storage and an open-addressed tile lookup. The top level is fixed at startup from maximum radius. Camera-local tree traversal requests coarse ancestors first and bounds requests to nearby rings at each level. Refinement covers tiles within one tile-width of the camera's base-chunk centre; the next ring is prefetch. Traversal intersects the requested square, rather than enumerating all base chunks within it.

Capacity reserves 48 slots per data level plus 16 replacement slots. Startup uses actual Vulkan buffer/image requirements to select the maximum supported radius and normal-detail capacity, reserving at least four fine-normal slots when the layout has detail. Pool exhaustion defers requests; a budget that cannot fit coarse coverage reduces the supported radius. Radius changes do not resize pools. GPU/host/transfer/disk defaults remain 512/256/8/8192 MiB respectively (transfer is per frame). Explicit GPU allocations exclude render targets and opaque driver overhead; host accounting retains the exclusions described in the streaming document.

A resident parent supplies coverage until children publish. Each active child suppresses exactly one parent quadrant. No parent/child surfaces are drawn over one another. Handles and epochs reject obsolete work; frame submission serials protect storage reuse. Normal refinement follows the [detail-demand policy](terrain_detail_demand.md): horizontal distance to tile bounds, protected level-zero neighbours, bounded motion prediction and retention preference. Request revisions and latched cancellation stop obsolete work without invalidating coverage. Fully covered ancestors do not acquire unnecessary fine normals. Coverage and normal generation are interleaved so the horizon backlog cannot indefinitely starve near detail.

Critically, active children pin their **parent neighbourhood**, not only their own parent. Every parent participating in a shared edge/corner must remain resident and fully refined until all dependent children have collapsed. This dependency also applies while children fade out. Without it, an adjacent parent can begin collapsing under a still-active fine edge. A GPU vertex replay and pixel-capture regression cover this failure.

Refinement demand must also include that parent neighbourhood **before** a child activates. Build the dependency closure from fine to coarse levels, promoting required parents from prefetch to refinement and adding missing parents even outside the visible square. Each level requires at most the neighbouring parent tiles, and uses the existing bounded request storage. Activation still proceeds coarse to fine; retirement still retains dependencies until children collapse. Merely retaining already-active dependencies leaves some camera positions permanently stuck at coarse LOD despite all requested data being resident. Regression tests must verify near-camera refinement after movement across positive, negative and distant coordinates, including small view radii, as well as edge continuity during transitions.

## Selection and transitions

Hierarchical selection uses horizontal distance bands doubling from a baseline of 64 m. Sampled approximation errors can expand the shared ranges by up to two times, subject to a twice-baseline triangle allowance. Sampled height bounds, expanded by the parent envelope during transitions, control frustum culling and conservative pixel-error projection. The existing quadrant topology and coherent even-vertex anchors are retained. Data-level neighbourhood readiness and the distance-band spacing keep neighbouring selected geometry within one level; reduced fixtures verify this with delayed products and partial parent masks.

Height availability approaches its target over 0.25 seconds. Only the finest geometry in a data tile needs an additional collapse toward its parent grid; coarser geometry already samples that grid. Shared 4×4 availability controls use identical edge/corner minima. Intermediate geometry blends current and parent triangle heights while collapsing toward even-grid anchors, so different data representations evaluate the same morphed edge. Lookup endpoints are exact, and rendering adds the integer camera-chunk-relative tile origin before subtracting camera-local position once.

Normals transition between the tile and available parent representation using the same spatial availability, independently of fine-normal arrival. Fine normals retain their own short residency fade and shared boundary weights. Parent normal sampling uses the matching absolute normal level when available. Level-colour diagnostics show absolute geometry levels (palette repeats after eight colours).

Turning LOD off in hierarchical mode draws the finest **resident** grid; it does not request all base tiles at 16 km. This diagnostic can submit many triangles. Frozen LOD retains the distance observer while streaming demand and visibility continue following the camera.

The renderer uses reversed-Z with D32, a 0.5 m near plane and a 64 km far plane. Depth clears to zero and tests greater. Camera-relative positioning remains in use.

## GPU work and diagnostics

Cold-generated products are copied directly into resident GPU pools in bounded slices. Only bounds needed by CPU selection are read back before coverage publication. Optional cache export happens afterwards; unavailable write mailboxes skip persistence rather than blocking visibility. Cache hits continue through bounded staged uploads. All publication waits for the existing owning frame fence; normal frame processing adds no queue-idle or device-idle waits.

The UI reports requested/effective kilometres, per-level residents, pending work, capacity deferrals, managed memory, transfer volume and GPU generation/terrain timings.

The cache has two CPU worker threads, but procedural generation executes as compute commands on the rendering GPU queue, scheduled once per frame by the render thread. The former eight-dispatch ceiling with 16×16 normal regions required at least 183 frames per default fine-normal product, irrespective of GPU speed. The replacement schedules workgroups against a soft 4 ms GPU budget and combines adjacent workgroups into rectangular dispatches. Independent compute-queue execution is not required for this change.

The scheduler uses a 4 ms soft GPU target with 10% headroom, and shares its 3.6 ms estimated allowance across successive kernel/LOD stages of the active tile. The increased target prioritizes faster tile completion while allowing more generation work on the rendering queue per frame. Startup growth, hard caps and fence ownership remain unchanged. UI budget values come from the same constants as the scheduler. A short stage tail must not discard the rest of the frame's allowance. It preserves the single scratch owner, stage dependency barriers, cancellation and fence-protected publication, stopping when that job's compute finishes.

Per-kernel costs come only from completed frame timestamps. Conservative estimates include the existing four-group startup and at-most-twofold growth per observation. A frame may issue one group to guarantee initial progress even when expensive; later stages cannot repeat this exception. Hard limits are 4,096 groups, 128 dispatches and 32 timed stage segments across the whole frame. Two frame slots own fixed segment arrays and 66-query timestamp ranges (terrain draw and 32 segment pairs); they are recycled only after their frame fences. Aggregate segment groups/time by kernel before updating each estimate once per completed frame. Fractional cost estimates preserve usable time between kernels even when group counts round down. Missing timing does not increase the allowance. The first and last segment timestamps also measure overall generation, avoiding a redundant pair on ordinary single-stage frames. Only pairs used by the current submission are reset. No new frame allocations or waits are introduced.

Diagnostics distinguish current recorded groups/stages/estimated milliseconds and stop reason from completed GPU time and its associated groups/stages. CPU tests exercise mixed costs, carryover, minimum progress, missing timing and hard limits. Existing numerical, cancellation, publication and synchronization GPU fixtures exercise multiple stages in a submission. A cold-cache full-view generation probe (`terrain_generation_probe --fill RIDGE DETAIL`) at radius 64, 256 cells, camera chunk (0,2), with explicit ridge/detail (0.50,0.65) and (0.60,0.72), compares the old and new schedulers on the same device and target. It looks away from terrain at 320×200 to exclude software drawing cost while requesting the complete view footprint, and records coverage/detail convergence, frames, generation time and frame costs; software Vulkan results cannot establish RTX 3060 performance.

## Verification and profiling

### 4 ms generation target (2026-10-17)

Debug and Release engine builds and generation-budget CPU tests pass. The rebuilt Release hierarchy and detail cancellation GPU tests both pass with Vulkan synchronization validation (111.18 seconds total).

The default soft target is now 4 ms (3.6 ms estimated allowance). A Release full-fill probe at ridge/detail .50/.65, radius 64 and 256 cells, using eight llvmpipe workers and Vulkan validation, completed in **4,046 frames / 11.410 seconds**, versus **21,123 frames / 29.217 seconds** at the previous 1 ms target. Both runs generated 171 coverage tiles, 22 detail tiles and exactly 421,305 workgroups. Dispatches fell from 31,453 to 9,388. Tracked GPU/managed host storage remained 443.537 / 44.7535 MiB; no validation errors were reported.

Completed generation time totaled 8,669.8 ms. Render-call p50/p95 were 2.922 / 4.846 ms; peak completed generation was 10.527 ms, so the target remains soft rather than a hard frame-time guarantee. The probe looks away from the terrain to isolate generation. These software-driver measurements establish faster cold fill in this workload, not RTX 3060 frame times.

### Shared stage budget verification at the former 1 ms target (2026-10-17)

Release, llvmpipe with eight workers, Vulkan synchronization validation, disk cache disabled, unchanged 0.9 ms estimated allowance under the 1 ms soft target. Both profiles completed with 171 coverage tiles, 22/22 selected detail tiles, exactly 421,305 workgroups, 443.537 MiB tracked GPU / 44.7535 MiB managed host storage and zero validation errors. The comparison executable retains the former one-stage scheduler; settings and workload are explicit in both versions.

| Ridge/detail | Scheduler | Complete frames | Wall seconds | Completed generation ms | Frame p50 / p95 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| .50/.65 | Previous | 21,951 | 29.829 | 17,713.0 | 1.356 / 2.019 |
| .50/.65 | Shared stages | 21,123 | 29.217 | 17,533.5 | 1.359 / 2.041 |
| .60/.72 | Previous | 21,949 | 31.298 | 17,709.5 | 1.373 / 2.195 |
| .60/.72 | Shared stages | 21,212 | 30.942 | 17,716.4 | 1.388 / 2.205 |

Debug and Release builds pass. The complete 20-test Release suite passed. After the fractional-cost refinement, all five focused CPU checks passed in both configurations. After the final timestamp cleanup, the generation-budget, hierarchy GPU, flat streaming GPU and detail cancellation GPU regressions passed again (four checks, 143.47 seconds). The hierarchy regression explicitly requires dependent stages in a single frame while checking the shared estimate and hard limits. GPU checks use Vulkan synchronization validation and report no errors. Physical GPU device nodes are unavailable in this environment, so RTX 3060 and live UI/flight verification remain unexecuted.

These final comparisons ran sequentially without concurrent builds or GPU tests. Completion required 3.8% and 3.4% fewer frames respectively; single-run wall times improved by only 2.1% and 1.1%, so this is a modest scheduling improvement. The new scheduler recorded multiple stages in 742 / 746 frames, with at most five stages per frame. Both versions had 795 frames without compute, including publication and final settling. Completed generation peaks were 2.222 / 2.525 ms for the first profile and 2.053 / 2.260 ms for the second; the target remains soft, and this does not establish an improvement in worst-case spikes. Maximum render-call times were 384–394 ms, including software-driver startup. There is no claim of an RTX 3060 frame-rate improvement from these uncapped software runs.

This removes an avoidable stage boundary throttle, but does **not** explain the entire reported slowdown. At that former target, remaining generation was dominated by the time budget: those runs spend about 0.86–0.87 ms per frame that records compute, close to the 0.9 ms allowance. Even ideal scheduling needs roughly 19,500 frames for the measured GPU work at that allowance. Publication frames are unchanged. Terrain kernels, filter sampling counts and cache representation are unchanged by this scheduler correction; raising the time target would be a separate frame-time tradeoff. Initial probes rendering the terrain were interrupted because software drawing dominated elapsed time; they are not included in this comparison.

Routine fixtures own their generated data, temporary caches and captures. Tests cover signed hierarchy addressing, bounded/no-allocation demand, pool sizing, delayed refinement, parent-neighbourhood retention, epochs and retirement, non-overlapping balanced patch selection, canonical shared heights/normals, cache reuse and identity changes, sliced direct publication, rapid edits/teleports and memory pressure. GPU checks use synchronization validation. Close-up captures use a distinct background colour to detect exposed sky, and GPU vertex replay checks shared positions while refinement withdraws. The fixed-generation/LOD/streaming regressions remain enabled.

The separate Release probe accepts output directory, cells, radius and maximum distance:

```sh
./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-1km 256 8
./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-8km 256 64
./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-16km 256 128
# Small correctness/review workload, not a production performance result:
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-small 32 8 128
```

Use a fresh output directory for a cold run. The probe runs cold/warm starts, flight, radius changes and teleports, saves captures and per-frame CSV traces, and reports initial-fill latency, a separate 100 m/s flight, frame percentiles, terrain GPU time, generation/cache counts, transfers and fixed memory use. The target is 120 FPS at 1280×800 on RTX 3060. This environment has no NVIDIA device nodes; software Vulkan establishes correctness, not target-hardware performance. Startup, teleports and uncached terrain can show incomplete coverage until coarse ancestors arrive. Materials, haze, shadows, vegetation and collision remain outside this milestone.

### Verification results

- Debug and Release: all twelve CTest cases passed, including the generation-budget controller, hierarchy GPU integration and the older generation, LOD and streaming regressions. Vulkan synchronization validation reported zero errors.
- Streaming and hierarchy CPU tests passed ThreadSanitizer without race reports.
- The hierarchy fixture checks exact shared height/normal samples at negative and million-chunk coordinates, warm-cache generation count zero, and a 0.01 m comparison with the previous terrain sampler. Its 32 KiB transfer limit exercises partial publication. A separate 4 MiB GPU pool forces detail pressure.
- Close-up movement crosses base-chunk boundaries and reverses direction while old refinements withdraw. GPU vertex replay compares shared clip positions within 1e-5, and captures contain no sky leaks through the terrain. CPU fixtures independently check non-overlap and adjacent geometry levels under delayed data.
- The refinement-demand regression reproduces the former permanent LOD 3 fallback at camera chunk `(2, -6)`. After the dependency fix, CPU fixtures verify fully refined near-camera tiles and borders, selected 0.5 m geometry, and bounded storage across signed/root/million-chunk coordinates and radii 0–128. GPU integration checks that the moved camera receives level-zero normals and geometry, with connected vertices and no sky holes. The fix changes demand only; existing terrain caches remain compatible.
- A full-size Release run at radius 128 (16.384 km outward) completed with 191 coverage tiles and 34 normal-detail residents at the initial view. Explicit terrain allocations stayed at approximately **442.75 MiB GPU / 44.47 MiB managed host** throughout radius changes and teleports. Warm startup generated **zero** products; later movement can generate additional products not yet cached.

On llvmpipe (LLVM 23.1.1), a cached 100 m/s flight measured approximately 18 ms median and 23 ms p99 CPU frame time. These initial milestone measurements preceded the refinement-demand correction above; they do not establish performance with the corrected near-camera detail. These validation-enabled software figures do **not** meet or predict the RTX 3060's 120 FPS target. First drawing use also produced a roughly 143–146 ms software-driver spike, including on warm-cache startup; compute warm-up does not eliminate first-use graphics compilation. No terrain-specific blocking wait was added to ordinary frame processing. Target-GPU timing, interactive swapchain behaviour and Windows/macOS remain unverified.

The outermost partial patches are additionally clipped in shading to the requested square. This removes coarse patch tails beyond the view boundary; the finite terrain edge is still visible without the atmospheric treatment planned for later milestones.

### Generation throttle follow-up

`terrain_generation_probe` isolates cold generation at the default 256-cell size, with no disk cache and the camera looking away to exclude terrain drawing cost. It stops after three detail products publish. On an otherwise idle llvmpipe device, the previous scheduler needed 758 frames / 1.05 seconds / 4,851 dispatches; the workgroup scheduler needed 309 frames / 0.48 seconds / 679 dispatches. The first detail product arrived at frame 134 instead of 258. These uncapped software runs establish reduced scheduling overhead and frame-count latency, not RTX 3060 flight performance or 120 FPS.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_generation_probe
```

### Detail cancellation follow-up

The [detail-demand design](terrain_detail_demand.md) records the revised ownership and verification results. `readTerrainDetailJob()` returns a value snapshot of the active request revision, phase, progress, cancellation latch and submission/completion serials. Residency snapshots include detail demand and residency. Optional export interruption means cache reuse is tested against skipped exports rather than assuming every generated product was persisted.

An additional probe mode takes a fifth argument specifying seconds per flight leg. It runs separate cold-cache 100 m/s and 400 m/s flights, reverses abruptly, then stops until demand settles. Use a fresh output directory. Unlike the original flight, it does not settle before movement. CSVs report nearby level-zero detail readiness, frame times, obsolete-work counters, coverage/detail residency, pending jobs and generation count; snapshot work happens outside the timed render call. An optional sixth argument changes the settling deadline from 60 seconds (maximum 600), for diagnosing slow progress. Passing with an extended deadline does not satisfy the default deadline.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-cold-flight 256 64 8192 3
```

The trajectory uses elapsed wall time, so frame counts and sampled camera positions differ with rendering throughput. Readiness includes missing coverage in its denominator. These are end-to-end cold-start measurements, not isolated normal generation benchmarks.

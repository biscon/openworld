# Terrain streaming

Status: implemented (milestone 3). Longer viewing distances, finer residency tiers and target-GPU profiling remain milestone 4.

The application follows the camera with a square radius of 0–8, default 2, plus one ring of coverage prefetch. Radius 2 means 25 renderable chunks and 49 coverage requests; radius 8 means 289 renderable chunks and 361 coverage requests. Frustum culling determines the submitted patches. Fixed neighbourhood rendering remains available to diagnostic callers and `--single-chunk` retains fixed inspection.

## Ownership and interfaces

- `TerrainStreamer` owns bounded CPU residency records, generation-checked `TerrainHandle`s, priorities and world epochs. A fixed 19×19 spatial lookup provides constant-time neighbour access. Residency records move through requested, reading, generation, working, resident and retiring states.
- `TerrainCache` owns two filesystem workers and two reusable payload buffers. Atomic mailbox states transfer ownership between the render thread and workers. Completed payloads are immutable until their lease is released. Frame-side operations do not acquire a mutex or wait for capacity; workers sleep on condition variables with a short timed recheck to avoid lost notifications.
- `VulkanTerrainStream` owns GPU pools, generation progress, transfer slices, timing queries and publication/retirement. Only the render thread calls Vulkan. Workers never access mapped GPU storage or GPU resources.
- `TerrainStreamingConfig` supplies startup budgets/cache settings. `SceneSettings::viewRadius` supplies live demand. `Renderer::setTerrainSettings()` changes world identity; `terrainStreamingStats()` reports residency/work/budgets. Coordinate-based height/normal readbacks are explicitly blocking diagnostics, not runtime interfaces.

Existing fixed-neighbourhood APIs remain available for deterministic generation/LOD tests. Streaming uses an independent graphics layout with compact heights and pooled normal images, sharing the established compute algorithms and patch topology. No general task graph, allocator framework or rendering HAL was introduced.

Mailbox lease operations have one caller: the render thread. Only that thread changes `Idle` to `Leased` or releases a completed lease; workers consume `Queued` and publish `Complete`. Acquiring an idle box therefore uses an atomic load/store, without a competing-consumer compare-and-swap. Publishing `Queued` and `Complete` retains release/acquire synchronization for payload access. This also avoids GCC 16's spurious key-assignment overflow warning after inlining the compare-and-swap acquisition path. The existing two-mailbox exhaustion, delayed-worker and payload round-trip tests cover this ownership contract.

## Products and visual behaviour

**Coverage** contains float32 base/eroded height pairs at 0.5 m, conservative leaf bounds and the coarsest RGBA16F normal level. **Detail** contains all remaining padded normal levels. The grid, sampling and filter halos are identical to the fixed generator. Geometry always samples full heights, keeping geometry LOD independent of normal residency and future collision data.

Coverage becomes eligible for drawing only after all transfers and metadata complete. Missing terrain remains absent until coverage arrives; fine-normal loading never removes existing coverage. Cold generation currently evaluates the complete normal pyramid, even for a distant chunk, so filtered levels remain exactly consistent. Coverage and detail are cached separately to avoid loading fine normals unnecessarily on later visits.

Fine normal availability fades over 0.25 seconds. Each chunk supplies a 4×4 set of weights: interior values use its own fade, edge values use the minimum of both neighbours, and corners use the minimum of all four participants. Smooth interpolation across one patch of border width makes adjacent chunks agree throughout refinement and eviction. Missing neighbours use zero availability. The vertex-normal diagnostic interpolates normals sampled at vertices from the available representation; it is not the old stored `TerrainSample` normal buffer.

Only complete coverage enters CPU selection. Streaming therefore does not use the fixed-neighbourhood global full-detail fallback. A shared LOD range grows smoothly when incoming bounds require it; affected chunks wait until that range is safe before drawing. Eviction never lowers the range within a world epoch. Freeze LOD retains the distance observer while streaming demand and visibility continue following the live camera.

## Scheduling and budgets

Visible coverage has first priority, followed by nearby normal refinement and then speculative coverage in the outer ring. Fine demand excludes regions that only need the root LOD. Existing detail receives a one-chunk distance preference to reduce churn. Two detail slots are kept as replacement headroom when capacity permits; coarse coverage reserves four replacement slots. Detail demand now follows the [revision and cancellation policy](terrain_detail_demand.md). Queued reads can be cancelled; running I/O keeps its mailbox until completion. Request revisions reject stale detail even while its coverage handle and world epoch remain valid.

Defaults are **512 MiB GPU terrain storage**, **256 MiB host working storage**, **8 MiB combined transfer allowance per frame**, and **8 GiB disk cache**. Startup computes capacities from footprint, normal representation, device limits and budgets. Before allocating pool memory, it creates temporary unbound buffers/images with the exact pool descriptions, queries Vulkan memory requirements, then destroys those temporary objects. Raw texel counts are insufficient because driver tiling and layer padding can substantially increase storage. Coverage gets the largest radius plus prefetch and four replacement slots fitting half the available pool budget; the remaining budget funds fine-normal slots. Fine capacity is selected by binary search using requirements for whole image arrays, rather than assuming a linear per-layer cost. Generation scratch and a safety reserve are excluded first. The default 128 m footprint supports radius 8 on the tested software driver; actual capacities depend on the device. Larger footprints, driver padding or smaller budgets can reduce the supported radius, which the UI reports separately from the requested radius.

Pools, descriptor sets, topology, CPU arrays, cache buffers and transfer buffers are allocated at startup. Radius changes do not resize them. The host figure covers managed buffers/CPU collections, with an allowance for cache metadata; thread stacks, C++ runtime and opaque driver allocations are outside it. Mapped streaming buffers are charged using actual allocation sizes, including alignment padding. GPU figures cover explicit terrain allocations and exclude render targets and opaque driver/descriptor overhead. Final allocation totals are checked before compute warm-up; budget failures report required and configured bytes for both GPU and host storage.

A small compute warm-up runs during startup, before interactive frames, to avoid deferred driver compilation on the first cache miss. Generation uses one shared scratch set. The original one-to-eight dispatch controller has been replaced for both flat and hierarchical streaming by the [per-kernel workgroup budget](terrain_hierarchy.md#gpu-work-and-diagnostics), retaining a **soft 1 ms** GPU target. Adjacent workgroups share dispatches, and timing feedback uses the actual work submitted. An individual group or driver first-use work can exceed this target. Without usable timestamps, the conservative four-group allowance remains.

Generated products pass through bounded readback/upload buffers, sharing the publication path with cache loads. This is an intentional first implementation tradeoff: it adds a transfer round trip on cold generation. Avoiding that copy and generating cheaper distant products are profiling opportunities for milestone 4. Tests reduce the transfer allowance to 64 KiB to exercise sliced products without production-sized fixtures.

## Synchronization and failure behaviour

Each frame's existing fence protects its mapped upload/readback storage. Transfers are sliced at buffer/image-row boundaries. CPU copies a readback slice only after the fence; upload payload leases can be released once their bytes have been copied into frame-owned staging. A product is published only after its final transfer submission completes.

Slot generations reject stale coverage results; separate detail revisions reject obsolete refinements. Current camera demand is evaluated before detail publication. Last-use submission serials protect coverage and fine-normal reuse. Generation scratch is retained until outstanding work has retired, including cancellation during settings changes or teleports. Changing world settings creates a new epoch and removes old-world terrain from drawing; radius changes retain compatible data and cache identity.

There are no terrain-specific queue-idle waits, synchronous readbacks, future waits or worker joins during normal streaming. The renderer still waits for its normal frame fences/presentation; bounded GPU work does not remove those existing dependencies. Resize, explicit diagnostics and shutdown may wait. Shutdown cancels queued cache work and joins workers before destroying their buffers; an already-running filesystem operation may finish during shutdown.

Read errors, incompatible/corrupt records and missing entries fall back to generation. Write errors do not prevent publication; diagnostics are rate-limited and writes back off. A full mailbox or resident pool defers work instead of blocking or growing memory.

## Cache format and maintenance

The per-user cache directory defaults to the XDG cache directory on Linux, LocalAppData on Windows and Library/Caches on macOS. `--cache-dir` overrides it; `--no-cache` disables persistence while retaining procedural streaming.

Files contain a 128-byte explicit little-endian identity header, an eight-byte payload checksum, an eight-byte header checksum and an exact-sized payload. Identity includes format/generator representation versions, product type, grid dimensions/spacing, normal subdivisions, patch size/levels, signed chunk coordinates, seed, all generation settings, generation-shader fingerprint and GPU/driver fingerprint. The latter includes vendor/device/driver information and pipeline-cache UUID. This deliberately avoids assuming cross-device bitwise equivalence.

A 64-bit FNV-1a hash indexes files into two-character shard directories; full header comparison validates identity. The checksums detect accidental corruption, not adversarial tampering. Coverage validation also checks finite heights, ordered conservative bounds and finite normal components. Payloads use float32 height/bound values and RGBA16F normals, with explicit dimensions rather than native C++ structure dumps. The current GPU payload codec requires a little-endian host.

Writes use unique temporary files and atomic rename. Background maintenance incrementally scans at most 128 entries per step and retains at most 1,024 oldest candidates, then removes stale engine cache entries until the budget is met. Maintenance continues when workers are idle; the disk limit is asynchronous and can temporarily be exceeded between scans. Read access refreshes an entry's modification time for eviction. Cache files are disposable; persistent player edits must use separate storage.

## Verification and measured limits

Routine tests use owned generated fixtures and temporary directories. They cover:

- Pool sizing with synthetic padded image-array requirements, coverage radius fallback, layer limits, exact budget boundaries and insufficient budgets; GPU integration checks actual allocation totals against the configured limits.
- Cache round trips, identity/version changes, malformed/truncated/corrupt data, invalid bounds, disk failures and background pruning.
- Both I/O workers deliberately blocked while 1,000 maximum-radius frame-side demand updates continue; mailbox exhaustion/polling and repeated demand updates allocate no frame-side memory.
- Negative/distant coordinates, nearest-first demand, radius limits, epoch changes, stale handles and delayed retirement. Shared normal weights agree at every chunk edge/corner.
- Small-grid GPU cache/generation comparisons for both height variants and every filtered normal level, including actual uploaded GPU images. Near-ground rendering agrees with the established fixed generator.
- A 64 KiB transfer allowance, rapid teleports/settings/radius changes with work pending, and a 6 MiB residency budget that forces mixed normal detail and slot eviction. Existing height data survives detail pressure.
- Vulkan synchronization validation and the existing generation, neighbourhood and geometry-LOD regressions. Streaming CPU tests also passed ThreadSanitizer. Final Debug and Release CTest runs passed all nine cases, with zero Vulkan validation errors.

Separate Release probes on llvmpipe (LLVM 23.1.1), before the driver-aware pool-sizing fix, produced:

| Initial region | Resident coverage | Fine-normal residents | Cold generations | Warm-start generations | Terrain GPU / managed host |
| --- | ---: | ---: | ---: | ---: | --- |
| 128 m chunks, radius 2 | 49 | 23 | 49 | 0 | 450.45 / 44.27 MiB |
| 32 m chunks, radius 8 | 361 | 0 (elevated root-LOD view) | 361 | 0 | 139.82 / 32.79 MiB |

Both probes crossed boundaries, reversed direction, changed radius and teleported while work was pending. Explicit terrain allocations remained fixed, and validation reported zero errors. The smaller-footprint probe verifies maximum-radius residency, not first-person detail at that altitude. Enlarging demand or teleporting into unseen regions can legitimately generate more chunks later in a warm run.

After the pool-sizing fix, both Debug streaming tests pass, including synthetic allocation-padding regressions and the existing GPU cache/residency tests. A default 128 m Debug startup with caching disabled renders eight offscreen frames at 444.86 MiB of explicit terrain GPU allocations with zero validation errors on llvmpipe. This checks startup and budget handling, not completion of terrain generation; the small-grid integration test checks completed streaming. The user subsequently confirmed NVIDIA startup works.

The initial software probes exposed roughly 300 ms spikes at the first generation dispatch. A startup compute warm-up removed those spikes in the repeated warm-cache flight; that run measured roughly 5 ms median, 11 ms p99 and 76 ms maximum. These software measurements do not establish target-GPU frame-time performance or a strict 1 ms GPU guarantee. NVIDIA device nodes are unavailable here. Interactive SDL/swapchain behaviour and Windows/macOS builds remain unverified.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_stream_probe /tmp/openworld-stream-review 256 2
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_stream_probe /tmp/openworld-stream-radius8 64 8
```

Use a fresh output directory for an actually cold cache. The probe saves captures and reports cold/warm generation counts, transfer volume, frame-time percentiles and fixed memory use. No xdotool automation is used.

## Milestone 4 extension

The application now uses [hierarchical data tiles and residency transitions](terrain_hierarchy.md). The flat coverage/detail policy documented above remains the milestone 3 diagnostic baseline. Cache worker ownership, checksums, failure handling, transfer budgets and fence retirement are shared. The hierarchy adds tile level and representation identity, canonical sampling, parent-neighbourhood retention and direct GPU publication before optional cache export.

## Detail-demand follow-up

[Detail demand and cancellation](terrain_detail_demand.md) adds protected nearby requests, bounded motion prediction, preemption, revision-matched cache reads and latched GPU cancellation. Scratch and unpublished slots retire against their last recorded fences without extending those fences. Optional exports may be interrupted by urgent protected detail, so a later warm run can regenerate products whose export was skipped. Cache formats and shader outputs are unchanged.

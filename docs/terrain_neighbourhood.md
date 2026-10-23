# Fixed terrain neighbourhood

Status: implemented (milestone 2a).

The demo draws a fixed 3×3 neighbourhood centred on a chosen integer chunk coordinate. Default chunks are 128 m wide, with 256 cells at 0.5 m and normals at 0.25 m. Footprint is selected at startup. Camera movement does not change residency. Milestone 2b now adds [quadtree LOD and filtered normal levels](terrain_lod.md); this document records the 2a baseline. Streaming remains future work.

## Requests and ownership

A `TerrainBatchRequest` contains shared settings, up to nine coordinates, and an active count. Validate the whole request before replacing pending state, including duplicate coordinates, capacity and all noise-domain bounds. Fixed slots track submitted requests independently. CPU request storage never grows during frame processing. A single-chunk convenience request preserves diagnostic/tool use.

Render patches reference resident chunk slots; initially each chunk has one full-grid patch. Topology belongs to the renderer, not the generated chunk. Slots are private fixed indices, not externally retained resource handles; no recycling registry is required yet.

## GPU layout and synchronization

One terrain owner holds shared compute pipelines, index topology, sampler, and a dense float2 height scratch buffer. Mesh samples occupy aligned slices of one buffer. One RGBA16F array holds two normal layers per slot (base and eroded), with a two-layer view per slot. Preallocated descriptors and aligned mapped parameter slices cover each slot and both frames in flight.

Generate dirty slots sequentially before drawing on the existing graphics/compute queue. Barriers protect scratch write-after-read reuse, previous draw reads before output overwrites, compute dependencies, and compute-to-draw visibility. Normal transitions address only the slot's two layers. Build shared indices once per changed grid size. Commit applied requests, counters and image initialization only after successful submission. Frame fences protect mapped parameters; normal frames never wait for device idle.

Default terrain payload is approximately 58 MiB, plus allocation alignment and driver overhead. Capacity is exactly the startup footprint in the demo; the core retains bounded grid resizing for tests. All nine outputs retain both terrain variants. Diagnostics may allocate and wait for idle, but ordinary requests and draws reuse storage.

## Demo and verification

The demo exposes startup size/cells, centre X/Z, and a single-chunk mode. Its UI displays footprint, extent, counts and allocated bytes, and has a draw-only chunk-border overlay. Reset frames the fixed neighbourhood. Generation settings apply to all chunks on edit release.

Small owned CPU/GPU fixtures cover transactional requests, ordering, negative/distant coordinates, shared height and dense-normal edges/corners, repeated updates with frames in flight, slot mapping, and tiled versus single-patch rendering. Same-device generation order comparisons are exact; boundary comparisons retain existing numerical tolerances (0.002 m height and 0.002 normal-vector distance), with separate capture inspection for visible cracks/seams. Cross-device bitwise identity is not promised. Debug/Release validation and one default-sized offscreen capture complement routine small tests. No xdotool workflow is needed.

## Verification results

- Debug and Release: five CTest cases passed, including the dedicated neighbourhood GPU suite, with zero Vulkan validation errors on lavapipe.
- Default eight-frame offscreen run: nine chunks, 1,179,648 triangles, nine chunk generations, 58.518 MiB of terrain allocations, zero validation errors. The overview capture was inspected for visible seams.
- Routine neighbourhood tests use 16-cell chunks at 0.5 m, two normal subdivisions, and 24-cell capacity. They compare all edges/corners, normal texels, order independence, and a 3×3 region against one 48-cell patch. Repeated edits exercise topology changes and inactive-slot reactivation without intermediate waits.
- Device: llvmpipe (LLVM 23.1.1). NVIDIA device nodes are unavailable in this environment; hardware performance and interactive swapchain behaviour were not measured. No xdotool automation was used.

Diagnostics expose snapshots and dense normal layers by slot; inactive or pending slots are rejected. Reported allocated bytes include Vulkan memory requirements for terrain buffers and the normal image, not temporary diagnostic buffers after they are freed, pipeline/descriptor driver overhead, or render targets.

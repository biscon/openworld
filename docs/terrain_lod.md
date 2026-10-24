# Terrain LOD

Status: implemented (milestone 2b). Milestone 3 adds [camera-driven streaming and tiered normal residency](terrain_streaming.md). [Milestone 4](terrain_hierarchy.md) extends residency across hierarchical tiles; their current geometry uses [stationary ground LOD](terrain_stationary_lod.md). The fixed-neighbourhood behaviour below remains the diagnostic baseline.

## Layout and selection

Generation chunks retain dense, camera-independent heights. Rendering uses a quadtree per chunk and shared 32×32-cell grids (smaller for small chunks). Chunk dimensions are powers of two. Each node doubles the sampling step; leaves preserve 0.5 m spacing. A draw contains a chunk slot, grid origin, sampling step, level and a quadrant mask. Sixteen shared index ranges cover the quadrant combinations without overlapping selected children.

`TerrainLod` owns contiguous, capacity-bounded node, bounds and draw arrays. `TerrainLodConfig` selects patch cells and minimum range; `TerrainPatchDraw` addresses chunk slots and node quadrants; `TerrainRenderStats` exposes the latest selection. GPU generation reduces union bounds for base and eroded heights at each leaf; CPU parents union these bounds and cache each slot’s required distance range; ordinary selection does not rescan all bounds. Selection uses camera-relative 3D point/AABB distances and Vulkan's zero-to-one frustum. Distances use a separate observer when LOD selection is frozen; visibility still follows the live camera.

Ranges double per level, starting at 64 m or a larger value required by the terrain bounds. Morphing runs over the final quarter of each range. The base range is at least 2.01 times the largest node diagonal divided by its level's sampling step. This keeps a selected fine node entirely inside the next level's unmorphed range. Vertices retain geographic XZ through the final-quarter morph and blend vertically to the parent triangle surface. Curved rendering interpolates the parent's curved vertices. Redundant vertices coalesce only at the exact completed endpoint, when their shared anchor also reaches it, to avoid raster T-junction cracks; the transition no longer horizontally compresses the grid. Patch diagonals match the parent triangulation. See the [surface correction](rendering_surface_review.md). Coarsest nodes never morph. No skirts or topology-specific edge patches are required. This is inspired by [CDLOD](https://github.com/fstrugar/CDLOD), with an independent implementation.

## GPU data and lifetime

Dense height/sample storage remains resident. New resources are shared patch topology, a small leaf-bounds buffer, two mapped bounds readbacks, per-frame view uniforms, and filtered normal levels. Generation copies only bounds to the current frame's readback; the CPU consumes them after the existing frame fence. Slot revisions and grid/request identity reject obsolete results. Coalesced grid edits rebuild the CPU hierarchy only when a frame begins. Reactivated slots with unchanged samples can reduce/copy bounds without regenerating heights or normals. No additional queue/device wait occurs on the frame path.

Until **all** active chunks have current bounds, the entire neighbourhood uses unmorphed finest patches without culling. A global fallback avoids transient cracks between chunks with and without metadata. This is deliberately more conservative than per-chunk fallback. Capacity is fixed at construction; grid changes reuse storage. No steady-state selection allocation is permitted.

## Normal representation

Level zero retains 0.25 m sampling. Each next level filters normalized normals with a separable 3×3 [1,2,1] tent, then normalizes. Explicit RGBA16F array images preserve the endpoint texel at every level; ordinary texture mip dimensions would drop that endpoint. The active width is `(cells * normalSubdivisions >> level) + 1`.

Each normal level has a halo `2^(maxLevel-level)-1`, where maxLevel is fixed by allocated capacity; height generation adds one further sample for central differences. The recursive halo makes filtering independent of resident neighbours and prevents clamped chunk-edge seams. A draw binds the current and next normal levels and blends them over the same distance interval, evaluated at the final morphed position so coincident vertices cannot retain different shading blends. Images and descriptors are allocated once. Compute writes, filtering reads, sampling and readback have explicit barriers.

## Diagnostics and testing

The demo supports LOD enable/disable, level colours, frozen LOD observer, live frustum culling, and actual triangle wireframe through index pulling and barycentrics. Draw statistics report selected/culled nodes, triangles, per-level counts and ranges. Existing normal/base/border comparisons remain draw-only. Low-level diagnostics can disable the LOD representation to retain arbitrary-grid sampling fixtures.

Small owned tests cover coverage, masks, winding, bounds, distance transitions, frustum rejection, negative/distant coordinates, allocation-free repeated selection, matching mixed-resolution vertices/corners, filtered normals and cross-chunk halos. Vulkan tests exercise delayed/stale bounds, in-flight regeneration, camera paths and draw-only controls. Debug and Release regression suites plus a default-size offscreen run verify integration. Software Vulkan results establish correctness, not hardware performance. Readbacks return active normal texels without halos. `readTerrainPatches()` copies the last CPU selection for diagnostics; it does not run on the frame path. `readTerrainIndices()` returns the concatenated quadrant topology in LOD mode, or dense generated topology in the legacy diagnostic mode. `normalsForFixture()` retains the non-LOD diagnostic layout. The app enables LOD resources by default; core diagnostic callers opt in with `RendererOptions::lod.enabled`.


## Verification results

- Debug and Release: all seven CTest cases passed on llvmpipe (LLVM 23.1.1), with Vulkan synchronization validation enabled and zero errors. Existing lighting/terrain tests retain their tolerances.
- CPU fixtures check all 16 index masks, exact selected-area coverage, triangle winding throughout morphs, shared geometric edges and normal blend levels, corners, flat/sloped/curved terrain, negative and million-chunk origins, visibility, fallback, bounded reconfiguration and 1,000 allocation-free selections.
- GPU fixtures independently filter a continuous terrain reference and compare all allocated normal levels, including chunk edges and distant origins (normal-vector tolerance 0.002). They verify GPU union bounds against dense base/eroded samples, reversed generation order, delayed/stale feedback, 12 consecutive in-flight grid/settings edits, and slot reactivation.
- Moving-camera flat-fixture captures are pixel-identical with LOD on/off and culling on/off; projected interior pixels contain no sky holes. A curved fixture crosses an actual selection threshold with a fixed viewing camera and checks continuity with a sub-millimetre observer change. Frustum culling preserves its capture. These are reduced deterministic correctness fixtures, not a universal visual-quality guarantee.
- The optional `terrain_lod_probe` runs the default nine 128 m chunks through an overview, ground-level chunk-boundary crossing, and elevated flight. It saves full-detail, LOD, level-colour and actual-wireframe captures for inspection. No UI automation was used.

Representative default-scene measurements (same view and culling enabled in each comparison):

| View | LOD triangles | Full-detail triangles | Selected LOD patches |
| --- | ---: | ---: | ---: |
| Overview | 52,224 | 1,148,928 | 26 |
| Ground at a chunk edge | 134,656 | 393,216 | 71 |
| Far end of flight | 18,432 | 1,175,552 | 9 |

The default bounds raise the first range to 91.06 m. Terrain allocations remain **71.48 MiB**, compared with milestone 2a's 58.52 MiB. Extra normal levels and padding increase residency; this milestone reduces submitted geometry, not resident world data. The flight performs exactly nine chunk generations and no terrain allocation growth. Counts include submitted degenerate morph triangles. Terrain allocation figures exclude render targets and opaque driver/pipeline/descriptor overhead.

Run the separate review workload with:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-debug/terrain_lod_probe /tmp/openworld-lod-review
```

NVIDIA device nodes are unavailable in this environment. Hardware frame-time/FPS gains and interactive swapchain behaviour are unverified. Windows and macOS are also unverified. Remaining scope includes residency/streaming, longer viewing distances, and profiling on the target GPU. Changing generation settings can briefly restore full detail while bounds arrive; intentional debug mode changes need not transition smoothly.

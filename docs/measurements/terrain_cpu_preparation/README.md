# Terrain CPU preparation, 2026-10-24

Status: CPU improvement measured; native RTX 3060 total-frame acceptance pending.
The user reports a Release-build slowdown after the wobble correction even with
CSM off, including views with fewer visible triangles. Selection visits the whole
terrain partition before culling, so visible triangle count alone does not explain
its cost. This measurement isolates preparation; it does not establish the entire
cause of the native FPS change.

## Workload and method

- AMD Ryzen 9 7900, GCC 16.2.1, Release `-O3 -DNDEBUG`, project LTO.
- `terrain_cpu_probe 480`, 16 warm-up frames followed by 480 measured frames.
- Owned synthetic terrain metadata, 14 dyadic levels, 256-cell tiles at 0.5 m,
  32-cell patch configuration, 1.2 px additional refinement. Camera advances
  0.016 m each frame. No user assets, cache or saved view are needed.
- Two triangle allowances: 524,288 and 2,097,152. Complete partitions reach 1,024
  and 4,096 leaves; visible counts finish at 280 and 1,090.
- Separate temporal-history workloads: 1,024 and 4,095 distinct full keys, shuffled
  with a fixed seed and a rotating submission order. Each includes begin, prior
  lookup, append and submission indexing.
- Baseline is `4dab9ad` selector/history code, compiled against the same benchmark
  source and unchanged supporting library. Corrected and baseline binaries ran
  sequentially without concurrent builds or GPU workloads.

The correction searches coverage from the query level outward, retains a complete
search on misses, rejects impossible splits before collecting neighbour closures,
and sorts compact indices instead of complete temporal records. Geometry quality,
triangle allowance, stitching, residency policy and shaders are unchanged.

## Results

Times are milliseconds. Raw data: [before](before.csv), [after](after.csv).

| Stage / workload | Before median | After median | Before p95 | After p95 |
| --- | ---: | ---: | ---: | ---: |
| Selection, 524,288 triangles | 1.583 | 0.198 | 1.655 | 0.258 |
| Selection, 2,097,152 triangles | 3.058 | 1.239 | 3.245 | 1.404 |
| History, 1,024 records | 0.103 | 0.071 | 0.115 | 0.083 |
| History, 4,095 records | 0.653 | 0.492 | 0.687 | 0.530 |

Selected-patch fingerprints match between versions for both selection workloads
over all measured moving frames. They include every complete-partition and visible
draw's slot, coordinates, step, level, cells, edge and corner stitch masks.
Temporal workloads retain every expected prior-frame correspondence. A single
synthetic run is not a native FPS prediction or a GPU shading measurement.

## Verification and follow-up

Debug and Release `terrain_lod` and `temporal_cpu` tests pass, including bounded
storage, no frame allocations, mixed-level selection and temporal records through
reordering, removal, overflow, reset and abandoned preparation. In both builds,
`horizon_gpu`, `temporal_terrain_gpu` and `temporal_gpu` pass under required Vulkan
validation on llvmpipe. Software Vulkan establishes integration correctness, not
RTX performance.

A replay of the saved mountain camera was attempted at 960×540 with 4 km
coverage, CSM off and an isolated cache. It was stopped during software terrain
generation; no settled-view timing or image acceptance is claimed from that run.
The user's saved review file was left unchanged.

The lighting window exposes `Terrain CPU` selection, draw preparation and
finalization beside the completed GPU timers. These measure current CPU work and
exclude fence, acquisition and presentation waits. Use the user's saved mountain
view in the native Release executable to evaluate total frame time and remaining
shading/TAA costs. Do not infer that native 120 FPS has been restored from these
isolated CPU improvements.

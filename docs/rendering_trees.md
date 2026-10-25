# European forests (5J-T)

Status: implemented. Small owned Vulkan fixtures and rendered asset reviews are
covered below. Native RTX 3060 performance, final reference-quality acceptance
and extended 8 km stress remain unverified; they are not marked passed.

## Ownership and boundaries

Trees are a separate library, population, streamer and Vulkan renderer alongside
ground vegetation. Runtime consumes immutable offline assets; only placement and
terrain attachment are generated at runtime. Use C++17, Vulkan and HLSL with the
existing CMake workflow. Share lighting, world coordinates, curvature, texture
import conventions and submitted-frame history. Do not extend grass's eight-asset
packing, 4 m cells or short-range policies to forests.

The initial library contains European beech, pedunculate oak, silver birch and
Scots pine: two young, two woodland and two spreading/irregular forms per species.
Summer foliage, wind, lighting, shadows and understory influence are included.
Collision, cutting, seasons and persistent edits are excluded.

## Offline assets

Tracked recipes, seeds, imagegen source artwork and prompts live in
`assets/trees/source`. Build outputs live in each CMake build tree. A deterministic
builder produces species-specific branch skeletons, tapered trunk/branch meshes,
root flares, shaped leaf/twig clusters and three mesh LODs. Broadleaf crowns use
space-colonization-inspired branching; pine has apical dominance and whorls.
LOD targets are roughly 32k/8k/2k triangles for mature trees. Crown gaps and species
silhouettes matter more than identical triangle counts.

A headless Vulkan baker captures the generated geometry from a full-sphere
12x12 octahedral direction lattice. Each direction has a 128-pixel tile. Capture
unlit albedo/coverage, object-space normals/authored ambient visibility/leaf classification,
and depth. Use tile-isolated, padded coverage-preserving mip chains; stop before
tiles merge. BC7 compression has an explicit RGBA fallback; fallback colour/normal tiles use
64 pixels to retain the memory ceiling, while R16 depth stays full resolution.
Source artwork is image-generated. Bark bump comes from luminance gradients;
roughness and leaf ambient visibility are authored approximations, not measured
material properties or a ray-traced AO bake. Builds track every
recipe/source/tool/shader dependency and publish a completion stamp last.

## Placement and residency

Use integer 64 m cell keys, up to 128 candidates per cell, compact 32-byte
instances and separate root-attachment history. Candidate identities, XZ positions,
species and shapes depend on seed/geography, never camera LOD or generation order.
Jittered strata and deterministic neighboring-candidate competition bound spacing
work. Broad woodland masks cover about half to two-thirds of suitable land;
coherent stands, mixed edges and smaller clearings prevent random species soup.
Beech favors sheltered/moist sites, oak suitable lowlands, birch open pioneer
stands and pine exposed/poorer uplands while remaining possible in lowlands.
Use geographic moisture/exposure proxies, not a claimed soil/climate simulation.

Suitability uses fixed world-coordinate geographic evaluation. Root heights use
the currently rendered terrain triangles, including stitched edges. Do not demand
fine terrain across kilometer ranges. Preserve published attachment while bounded
repairs queue; retain previous heights for temporal motion. Missing surfaces defer
new trees. Cell height enclosures retain the union of old/new provider bounds
through repairs so a retained root cannot be culled by a smaller new enclosure.
Trees remain upright and use root flares on slopes.

Terrain attachment jobs query the existing ground partition hash by each candidate root,
deduplicate at most 128 supporting patches, and retain their provider slots through
submission. This avoids scanning the full terrain partition for every repair and
keeps fine ground tessellation from overflowing a 64 m cell job.

The default is 4 km visibility, 32,768 resident cell slots and a 768 MiB actual
GPU buffer/image allocation ceiling including assets, staging, frame buffers
and retirement. This accounts for actual `VkMemoryRequirements`, not driver-opaque
pipeline/descriptor allocations or shared renderer resources.
The optional 8 km stress profile uses 65,536 slots and 1.5 GiB. Demand includes
offscreen casters. Near cells and root repairs outrank distant work. Cap updates
at 32 distinct cells/frame within the existing shared terrain-generation allowance.
Reserve up to 0.75 ms for trees before terrain dispatch, then subtract their
estimated cost from the allowance passed to grass. As with terrain, one expensive
cell can exceed the soft estimate to prevent permanent starvation. Trees and
grass share this allowance rule; a regression check covers an expensive single
cell, exhausted budgets and reserved repairs.
Exhaustion defers work and reports incomplete coverage; no frame-time growth or
silent acceptance of list overflow. Generation-checked slots retire only after
all submitted readers finish. Startup allocations account for driver requirements.

## Rendering and interfaces

Expose TreeLibrary/TreeAssetId, TreeCellHandle, TreeSettings, TreeStreamConfig,
TreeStats and VulkanTrees. Use contiguous storage and bounded GPU
count/prefix/scatter selection: cells first, then individual trees. Camera and
shadow lists are independent. Draw indexed indirect instances grouped by
asset/LOD/material, sharing trunk/leaf lists where possible. Fixed bindings and
bindless devices must both work. Conservative Hi-Z uses opaque scene depth, not
solid canopy boxes.

Camera LOD thresholds start at 320/120/64 projected crown pixels, with 15%
transition bands and a 1.5% projected-size deadband (0.5 pixel minimum). Retain tree positions through the full range;
fade only at the outer boundary. Impostors blend three adjacent octahedral views,
correct parallax from depth, reconstruct surface positions and write fragment
depth. Light them dynamically using shared PBR/foliage lighting. Shadow impostors
face the light, not the camera, and reconstruct light-space depth. Existing CSM
resolution limits distant leaf detail; broad canopy shading remains meaningful.

Shared alpha rules serve depth/color/shadows. Leaf gaps create dappled sun and
two-sided transmission supplies leaf backlighting. Ambient visibility affects
indirect lighting only. Wind anchors roots, bends branches and flutters leaves;
fine motion fades with distance and broad sway matches impostors. Current and
previous successful submissions share curvature, attachment and wind evaluation.
Reject invalid motion correspondence at incompatible LOD/publication changes.

A 128x128 near canopy map at 4 m spacing biases existing grass/fern/shrub
candidates independently of physical CSM lighting. Grass thins to roughly one
third beneath dense crowns; other species use their authored understory/meadow
affinity ratio. This adjusts retained candidates, not a soil simulation or a new
population recipe. Add Trees controls and review bookmark settings to the
existing engine UI. Keep main focused on orchestration. A tree probe provides
specimen, under-canopy, edge and panorama checks.

## Delivery and tests

T1 rendering, T2 24-asset generation, T3 streaming/attachment and T4 understory
integration are implemented. Combined native performance and final visual
acceptance remain open; the code completion does not close those gates.

Owned CPU/build fixtures cover deterministic geometry and LOD reduction, bounds,
serialization errors, incremental rebuilding, missing output recovery, failed
publication, octahedral directions/weights, extreme coordinates and slot retirement.
Small Vulkan fixtures cover sparse baked silhouettes, mesh/impostor pixel coverage,
light-facing shadow depth, octahedral axes/poles, TAA/wind, camera rebasing, terrain
attachment to indexed stitched triangles, cross-cell spacing, coherent stands,
teleport/return identity and terrain edits. The streamed fixture also enables
existing grass and checks Vulkan validation. No correctness test uses game artwork.

See [verification and review captures](measurements/trees_5j/README.md) for commands,
results and limitations. Native targets remain 1080p RTX 3060, combined p95
16.67 ms, incremental trees <=4 ms GPU / <=0.5 ms CPU and zero warm render-thread
allocations. Software Vulkan timings are not native performance evidence.

## Runtime layout and lifetime

`tree_library` owns versioned immutable asset data. `tree_generation`, `tree_baker`
and `tree_import` are offline tools. `trees` owns world keys, settings and the
bounded CPU residency table. `VulkanTrees` owns GPU resources; its startup/assets,
frame orchestration and shared internal layout are separate files.

Each 64 m resident cell has 128 fixed lanes. A lane uses 32 bytes for identity,
local XZ/scale, yaw, variation and packed asset/cell index; a separate 16-byte
attachment holds current/previous height and publication serial/validity. One
32-bit value stores camera LOD hysteresis. Default population/attachments/history
use 128/64/16 MiB. Camera and five cascade lists each hold at most 1,048,576 entries;
the two frame slots together reserve 48 MiB for these lists. Counts/prefix/scatter
share selections between wood and leaves. Transition entries can count twice.
Overflow is capped and reported, never an out-of-bounds write or silent success.

Host cell/job/uniform buffers and selection scratch are allocated once. The two
frame slots are reused only after their renderer fences complete. Resident slots
carry generations and last-submitted serials; displaced slots retire before reuse.
Startup texture/geometry uploads may block, while ordinary frame updates do not
wait. Readback of stats/timestamps is delayed; `readTreeSnapshot()` is an explicit
blocking diagnostic, bounded to 64 cells, outside normal frame processing.

## Build and review

```sh
cmake --build cmake-build-release --target engine tree_probe
cmake --build cmake-build-debug --target trees_cpu_tests trees_gpu_tests trees_stream_gpu_tests tree_import
ctest --test-dir cmake-build-debug -R '^(trees_.*|tree_asset_build)$' --output-on-failure
```

The normal hierarchical engine loads trees and enables 4 km forests. `--no-trees`
is the comparison switch; `--tree-distance 8000` selects the larger startup
capacity/budget. The Trees window exposes range, coverage, density, treeline,
wind, opaque occlusion, forced LOD and statistics. Review bookmark version 6 saves
these settings and capacities; older bookmarks receive defaults.

`tree_probe` exports `view.ppm`, `frames.csv` and `summary.txt`. Scenes are
`specimen`, `canopy`, `edge`, `panorama`, and terrain-backed `stream`. Use `--asset
0..23`, `--lod 0..3`, `--distance`, `--yaw`, `--sun`, `--vegetation`, `--rgba`,
`--taa-off`, `--validation` and `--no-trees` for comparisons. `--sequence` supports
`stationary`, `walk`, `travel` and `teleport`. The normal measurement uses 120 warmup
and 600 measured frames; `--quick` uses small resolution, 512px cascades and a
small panorama. `--span 128` restores the 8.2 km-wide fixture, while `--capacity
32768` tests the normal allocation profile. Explicit frame/size options override
quick defaults. CSV allocations count render-thread `new`, including calls made
by the Vulkan driver/validation layer on that thread; validation must be disabled
for warm allocation measurements.


## Research

- [Runions et al., space colonization](https://algorithmicbotany.org/papers/colonization.egwnp2007.html)
- [Brucks, octahedral impostors](https://shaderbits.com/blog/octahedral-impostors)
- [Epic, impostor baking](https://dev.epicgames.com/documentation/en-us/unreal-engine/impostor-baker-plugin-in-unreal-engine)
- [NVIDIA, tree lighting and shadows](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-4-next-generation-speedtree-rendering)
- EUFORGEN: [beech](https://www.euforgen.org/species/fagus-sylvatica),
  [oak](https://www.euforgen.org/species/quercus-robur),
  [birch](https://www.euforgen.org/species/betula-pendula),
  [pine](https://www.euforgen.org/species/pinus-sylvestris).

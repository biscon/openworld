# Terrain ridge LOD quality

Status: implemented; validation/review results below. This correction precedes material slice 5C.

The hierarchical renderer previously point-sampled all erosion frequencies on sparse grids. Narrow ridges alias into alternating peaks and depressions. Normal filtering cannot repair their silhouettes. The correction preserves the level-zero world and uses filtered height representations plus bounded screen-error-driven extension of the shared LOD ranges.

## Representation and ownership

Each coverage tile owns compact endpoint-preserving base/eroded height grids for its selectable geometry levels and one parent target level. Absolute level, canonical world coordinate and generator settings completely determine a height: level zero is unchanged; higher levels use a separable 5x5 binomial stencil [1,4,6,4,1], spacing half the geometry spacing. An 8x8 output group shares a 19x19 source stencil: 361 terrain evaluations instead of 1,600 independent evaluations. Shading normals use a separate shared source stencil and normal-resolution filtering of the original height field; geometry filtering is not applied to them. This finite stencil is an approximation, not an exact band limit.

Matching absolute levels agree across tile representations, including negative and distant coordinates. A parent target is stored in the child's own height pyramid, so geometry does not require a new cross-slot descriptor or asynchronous parent lookup. Existing parent-neighbourhood pinning remains necessary for residency transitions.

Coverage metadata records bounds and base/eroded triangle approximation errors per patch and geometry level. Reference probes include half-spacing samples of the original terrain; metadata retains the largest child error. These estimates are not mathematical bounds on unseen procedural frequencies. Culling bounds include both filtered transition endpoints and child bounds. Heights, metadata and normals use sliced generation, fence publication, cache workers and retirement. Separate timing estimators cover raw heights, filtered heights and error reduction. Expensive compute pipelines are warmed during startup.

`TerrainPayloadLayout` describes compact height-level offsets, leaf bounds and level metadata. Filtered hierarchical representation 7 replaces 6. Diagnostic unfiltered representation 8 uses the same payload layout; fixed/flat representation remains 3. Shader identities also participate in cache keys. Loading validates sizes, finite values, bounds and nonnegative error estimates. Pool sizing counts the larger coverage allocation before choosing capacities. CPU error arrays are preallocated and included in managed host accounting. No frame-time allocation or synchronous GPU readback is introduced.

## Error-driven shared ranges

Default projected-error target is 2 pixels, with a 1-pixel coarsening threshold. Projection uses viewport focal scale and the closest camera-space depth of the patch bounds, clamped to the near plane. Near-plane intersections can yield deliberately large reported errors. The frozen observer retains its viewport focal scale and requested range scale; hard geometry-budget enforcement still takes precedence.

A conservative implementation adjustment preserves one shared distance/morph field. The maximum visible sampled error requests a range scale between 1 and 2. The scale changes at four units per second (a full 1-to-2 adjustment takes 0.25 seconds). Up to eight bounded trial selections cap submitted geometry at twice the baseline distance-selected triangle count. Budget contraction, camera jumps and diagnostic switches may clamp the scale immediately. Existing coverage demand, balanced topology and parent-neighbourhood dependencies remain authoritative. Refinement can only use resident data; missing finer coverage stays on its parent surface.

This is **not independent per-patch screen-error refinement**. The initially proposed per-patch split/merge state machine, ranked extra residency requests and new shared-edge control buffers are not part of this correction. Keeping a shared scale retains the existing nesting/anchor continuity proof and bounded storage. It spends some additional triangles on nearby lower-error terrain and cannot guarantee a two-pixel silhouette under all settings or budgets. Those limitations are reported rather than hidden. Local refinement remains a possible follow-up if hardware profiling justifies the added topology and residency machinery.

`SceneSettings` adds `terrainLodQuality`, `terrainLodPixelError` and `terrainLodErrorView`. Rendering diagnostics expose the target, comparison switch, error colours, maximum sampled pixel error, over-target patch count, applied range scale and budget limiting. Disabling error-driven LOD retains filtered height levels. The startup-only `TerrainStreamingConfig::unfilteredHeightReference` is for review tools and uses a separate cache identity.

## Connected filtered transitions

Filtered representations differ at coincident vertices. The existing coherent even-vertex horizontal collapse now also blends height from the current level to its parent triangle surface, including even vertices. Exact endpoint interpolation avoids float rounding differences at tile boundaries. The same absolute-level samples make these targets agree across data representations. Normals retain sub-mesh relief through independent normal-resolution filtering and existing distance/residency fades. Parent/child coverage remains non-overlapping; bounds cover both endpoints. Height availability retains its existing 0.25-second residency transition.

## Tests and review

`sampleTerrainHeightLevel` is a blocking diagnostic that reuses reserved non-streamed scratch, invalidates slot zero, and executes the actual height-generation kernel. Its optional analytic ridge variant is compiled separately and has no production runtime branch. `readStreamedTileHeights(tile, geometryLevel)` can inspect matching absolute levels across resident tiles.

Owned straight and curved Gaussian ridge fixtures at several sampling-grid offsets verify the GPU footprint against an independent analytic reference. At 4/8-metre spacing they reduce crest-height standard deviation by **85–93%** compared with unfiltered point sampling. This measures the repeating sawtooth component; it is not an 85–93% reduction in total silhouette error relative to the original sharp crest. Filtering lowers and broadens unresolved narrow peaks. Absolute two-pixel silhouette accuracy is not established by this test.

Level-zero procedural base and eroded heights remain exactly equal to the existing geography probe. The procedural fixture uses seed 1, region 16384, mountain 2048, height 1200, erosion wavelength 1133, strength .674 and rounding .270. Other checks cover flat preservation, filter addressing, malformed payload rejection, pixel-error projection, allocation-free selection and same-absolute-level parent/child equality. Existing hierarchy GPU vertex replay and no-sky captures check mixed edges/corners during movement, reversal and residency withdrawal.

Build/run the focused review:

```sh
cmake --build cmake-build-release --target terrain_lod_quality_probe terrain_lod_quality_gpu_tests
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/terrain_lod_quality_gpu_tests
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/terrain_lod_quality_probe /tmp/terrain-lod-quality
```

The probe saves unfiltered/filtered views with identical terrain settings and cameras, plus text records of coordinates, angles, settings, triangles, error, memory and settling time. It uses a reduced footprint and software Vulkan correctness review; it does not establish RTX 3060 performance or production 8/16-km cold-flight behaviour. Hardware performance and the user's exact camera view remain live-review checks.

The reduced procedural review at world camera (-2544, 800, 11024), pitch -0.65, saves [unfiltered](images/terrain_lod_unfiltered.png) and [filtered](images/terrain_lod_filtered.png) captures. Far ridge outlines are smoother; both runs completed with zero Vulkan validation errors. These captures use identical constant shading to expose geometry. Their roughly 109 MiB allocations are equal because the diagnostic reference deliberately uses the same pyramid layout. This small-grid view is residency-limited and did not gain extra triangles from shared-range extension; it demonstrates filtering, not independent adaptive refinement. Settling times were recorded while correctness work also ran and are not performance comparisons.

### Verification results (2026-10-17)

- Debug and Release engine builds succeeded. Release passed all 20 tests in 197.82 seconds on llvmpipe (LLVM 23.1.1), including all 11 GPU tests with Vulkan validation enabled.
- Debug passed all tests across the initial run and focused reruns. The hierarchy test exceeded its 120-second timeout while a separate GPU capture workload was running; run alone, it passed in 91.51 seconds. The quality regression passed in 3.23 seconds. No Vulkan validation errors were reported. The final payload-fixture adjustment was checked in both configurations.
- Default 256-cell pool startup at 8,192 metres retained the full requested radius: 468,457,900 GPU bytes, 46,736,976 managed host bytes, 352 coverage slots and 31 detail slots. At 16,384 metres: 465,082,540 GPU bytes, 46,927,440 managed host bytes, 400 coverage slots and 24 detail slots. Both fit configured budgets with zero validation errors. These are capacity checks, not full-radius flight benchmarks.
- An isolated Release `terrain_generation_probe` at chunk (0,0), disk cache disabled, reached its first three detail tiles at 0.66, 1.34 and 1.96 seconds, with at most four dispatches per frame and zero validation errors. This reduced-footprint software-GPU measurement has no matched pre-change baseline and does not establish a generation-speed improvement.

RTX 3060 frame times, cold streaming over the full 8/16-km footprint, and the original screenshot camera remain unverified in this environment. The larger payload changes pool composition and requires regeneration of older hierarchical cache entries.


## Shading detail correction (implemented)

The first ridge correction also blurred heights before generating shading normals,
then filtered those normals again. It removed useful lighting detail, especially
in fallback normals. Normal demand still used the baseline range even when
geometry ranges expanded. The correction keeps geometry heights and morphs intact.

Shading normals instead differentiate the original procedural terrain at their
own sampling resolution. Absolute level zero retains its central differences;
higher absolute levels retain the 3x3 [1,2,1] normal filter, without the geometry
height stencil. Shared source samples, canonical coordinates and existing halos
preserve cross-tile agreement. No new resident images or runtime shader options
are needed. With the default two normal subdivisions, a filtered workgroup now
evaluates 441 height samples instead of 841 and omits the extra height convolution.
This reduces generation work; it is not a measured frame-rate claim. The generation
shader hash invalidates coverage and detail caches.

A shared range helper defines the baseline 64 m, maximum scale 2, and scale slew
4/second. Normal demand uses the previous committed geometry scale plus at most
one frame's expansion (clamped frame time <=0.1 s), with no allowance when frozen
and baseline scale when quality is disabled. Streaming preparation receives scene
settings. Existing prediction, protected-near ordering, slot limits, cancellation
and fence retirement remain authoritative. Diagnostics report eligible, selected
and ready-selected detail tiles; eligibility is not a promise of available slots.

Regression coverage compares the production normal algorithm against an
independent analytic corrugated/ridge reference, including normal variation,
checks expanded-range demand and freeze/disable behaviour, retains geometry and
cache tests, and reviews fixed views while the LOD observer moves at production
256-cell grid size. Software correctness and target-GPU performance are reported
separately. This correction does not add local adaptive geometry or enlarge pools.


The analytic test uses `sampleTerrainNormalFixture`, a blocking non-streamed
probe executing the production normal arithmetic with a compile-time owned
corrugated/ridge surface and buffer output. It checks each direction within 0.002
and slope energy within 1% of an independent CPU reference, across absolute
levels and both terrain channels. Packed image samples still receive the existing
hierarchy cross-tile checks. Diagnostic pipelines are lazy and owned by the terrain
renderer; scratch is reused and slot zero invalidated after inspection.

`setTerrainLodObserver` sets the existing diagnostic observer for a subsequent
`freezeLod` draw, allowing a fixed view to show geometry transitions without view
motion. It does not move the streaming camera. The visual probe uses periodic
terrain frames during warm-up and avoids repeatedly rasterizing terrain while
products are pending; the final sixty warm-up frames draw the actual scene.
These settle times are tool-workload timings, not interactive frame performance.

Reproduce production-grid review and startup capacity checks separately from tests:

```sh
cmake-build-release/terrain_lod_quality_probe /tmp/detail-default --detail cmake-build-release/shaders 256 0
cmake-build-release/terrain_lod_quality_probe /tmp/detail-strong --detail cmake-build-release/shaders 256 1
cmake-build-release/terrain_lod_quality_probe /tmp/detail-capacity --capacity
```

Set `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json` to reproduce the software
Vulkan run. Setting 0 uses the latest screenshot's 1024/.22/.30 erosion controls;
setting 1 uses the earlier 1133/.674/.270 controls. Each records camera, settings,
geometry counts, residency, forward/reverse observer captures, live-flight captures
and recovered views. Compare the same command with a saved pre-correction shader
directory to isolate the previous normal-generation algorithm.

The default view at chunk (10,7), local (64,620.4702,64), pitch -0.45, yaw 0 is saved
[before](images/terrain_normals_before.png) and [with restored shading detail](images/terrain_normals_restored.png).
The height and terrain-vertex SPIR-V binaries are byte-identical between these
versions. Geometry counts match at each captured observer position; the settled
shared range scales differ slightly (1.5204 versus 1.5192) because the budget search
retains frame-history-dependent rounding. This is not an exact pixel-isolated
normal comparison. The analytic normal fixture supplies the numerical comparison.


### Shading correction verification (2026-10-17)

- Debug and Release builds completed without compiler warnings. The full Release
  suite passed all 20 tests (11 GPU cases) in 218.26 seconds with llvmpipe and
  Vulkan validation. Debug CPU checks passed all nine cases. The final focused
  Debug run passed LOD, streaming, hierarchy GPU and quality GPU cases in 111.88
  seconds (hierarchy 89.59 seconds; quality 18.26 seconds), with zero validation
  errors.
- Normal-reference tests preserve slope energy within 1% across the tested levels;
  both base/eroded directions satisfy the 0.002 vector tolerance. Existing crest
  variation tests retain their at-least-80% reduction requirement. Coverage and
  detail cache fixtures both reject changed shader identities.
- The CPU demand fixture includes a tile outside the baseline normal band that
  becomes eligible in the expanded band, plus freeze/disabled-quality cases. The
  GPU fixture verifies the actual streaming range reaches 128 m from 64 m, restores
  the baseline when disabled, and settles all selected normal products. In that
  stationary reduced view all active tiles were already eligible at baseline, so
  it does not establish additional visible refinement from range expansion.
- Default and stronger-erosion production-grid reviews both completed with zero
  validation errors, fixed 469,345,708-byte terrain allocation and 41/41 selected
  normal products ready. Default observer reversal reproduced all four repeated
  lit/normal views pixel-for-pixel. In the stronger preset, the geometry cap reduced
  frozen scale from 1.7050 to 1.6747 during the first advance; subsequent repeated
  positions matched pixel-for-pixel. Hard budget enforcement intentionally takes
  precedence over the frozen scale. The [stronger preset capture](images/terrain_normals_strong.png)
  records the retained erosion detail.
- Isolated review warm-up took 53.41 seconds with restored normals versus 69.08
  seconds using the saved previous shader directory; forward/reverse-flight
  recovery took 5.58 versus 8.45 seconds. These single software-driver tool runs
  use accelerated warm-up and slightly different settled range scales; they do
  not establish an interactive frame-time improvement. Strong-preset warm-up and
  recovery were 71.63 and 11.16 seconds.
- Startup capacity checks at 8,192/16,384 metres retained the full requested radius
  and exactly the previous allocations: GPU 468,457,900/465,082,540 bytes, managed
  host 46,736,976/46,927,440 bytes, coverage slots 352/400, detail slots 31/24.
  Both checks reported zero validation errors.

- An isolated Release generation probe at chunk (0,0), cache disabled, reached
  the first three detail tiles at 0.666, 1.311 and 1.949 seconds, with at most four
  dispatches/frame and zero validation errors. These are current-run measurements,
  not evidence of a hardware performance gain.

RTX 3060 frame times and the exact screenshot camera remain target-hardware
checks. No NVIDIA/render device is exposed in this environment.

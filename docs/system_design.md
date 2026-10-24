# System design

This document is the overview and index for engine system designs. Development priorities and working conventions are defined in [AGENTS.md](../AGENTS.md); feature order and demo scope are recorded in [milestones.md](milestones.md).

## Design priorities

- Fast, reusable systems with small interfaces and explicit ownership.
- Algorithms and data layouts suited to the workload, with attention to locality and bounded resource use.
- Preallocated, reusable storage for steady-state frame work; deliberate policies for capacity growth and exhaustion.
- Typed handles for long-lived managed resource identities, with safe slot reuse where applicable.
- Explicit CPU/GPU lifetimes and synchronization.
- Practical modularity without speculative frameworks or design patterns that compromise performance.

Milestone 4 adds [hierarchical terrain residency](terrain_hierarchy.md): coarse data over larger areas, near-camera refinement and 8–16 km views.

## System design index

- [Vegetation representation experiment](rendering_vegetation_experiment.md) — implemented 5J-V1 automated comparison with saved fixtures, compact indirect instancing, LOD, wind, CSM and temporal motion. Debug/Release, software Vulkan and native RTX 3060 synchronization validation pass; native performance results are recorded, final asset/scene acceptance remains open.
- [Vegetation and ground clutter roadmap](rendering_vegetation.md) — agreed architecture; V1 experiment implemented, V2–V6 proposed: baked grass tufts and mixed mesh/card plants, bounded GPU population and instancing, independent LOD/density/shadow budgets, shared wind and temporary player bending. Slices 5J-V1–V6 start with a representation/performance experiment; trees remain a separate future design.

- [Engine UI and layout persistence](engine_ui.md) — implemented: menu-driven settings and diagnostics, dedicated UI modules, and UI-only JSON state saved on clean exit. Debug/Release and software Vulkan checks pass; native desktop review is blocked by an X11 drawable error.

- [Local froxel volumetrics](rendering_volumetrics.md) — implemented slice 5I: bounded local media, mixed atmospheric transport and independent scattering history. The [stability correction](rendering_volumetrics.md#stability-correction) addresses reported opacity flicker, depth bands and ambiguous preset controls; target-GPU acceptance remains pending.
  The implemented [shaft filtering correction](rendering_volumetrics.md#shaft-filtering-correction) addresses the remaining repeating slat-shadow pattern with bounded spatial integration, preserving stable mist opacity; native acceptance remains pending.

- [Terrain CPU preparation cost](terrain_stationary_lod.md#cpu-preparation-cost) — implemented and CPU-benchmarked: retain the stationary mesh and quality budgets while reducing neighbour lookup work and sorting compact temporal indices instead of whole surface records. Native frame-time acceptance remains required.

- [Curvature precision correction](rendering_horizon.md#curvature-precision-correction) — implemented; native wobble correction confirmed: stable small-angle position and tangent mapping, with GPU regression coverage for bounded trigonometric error. RTX 3060 review confirms the moving circular band is resolved; performance acceptance remains separate.

- [Stationary ground terrain LOD](terrain_stationary_lod.md) — implemented; native acceptance pending: local selection, immutable patch interiors, stitched boundaries and explicit transition/resource limits, replacing the unresolved radial morph.

- [Stable terrain surfaces and rendering review](rendering_surface_review.md) — initial correction and software validation; the remaining moving band is addressed by the stationary ground LOD design above. Includes per-texel CSM filtering, conservative patch bounds, completed triangle counters and saved review views; the follow-up diagnosis records the remaining transition-quality gap.

- [Continuous distant terrain and natural horizon](rendering_horizon.md) — implemented: bounded compact distant levels, visual curvature and extended atmospheric transport; target-GPU/flight acceptance remains pending.

- [Sky, atmosphere and distant coverage](rendering_atmosphere.md) — implemented slice 5H: physical transport, sky/environment lighting and bounded coarse horizon coverage. Owned software verification is recorded in the design; target-GPU and full-scale flight acceptance remain pending.

- [Temporal antialiasing and HDR bloom](rendering_temporal_post.md) — implemented slice 5G: submitted-frame geometry history, native scene-resolution TAA and bounded pre-exposed bloom. Verification is recorded in the design; target-GPU/live review remains pending. Auto-exposure is deferred.

- [Local terrain asset builds](rendering_materials.md#local-asset-builds) — implemented: CMake imports tracked source artwork into each build tree; generated DDS files are removed from the source tree without rewriting history. Debug/Release, incremental-build fixtures and software Vulkan material checks pass.

- [Scene resolution and presentation](rendering_resolution.md) — implemented: configurable scene height (demo default 1080) independent of native output resolution, native ImGui and F10 borderless fullscreen. Debug/Release and software Vulkan checks pass; live windowed and target-GPU verification remain pending.

- [Cascaded sun shadows](rendering_shadows.md) — implemented slice 5F: four stable near cascades and one far mountain cascade, shared rendered terrain surfaces, bounded offscreen residency and ordinary depth rendering. The virtual implementation and dense clipmap fallback are removed. Debug/Release and software Vulkan checks pass; target-GPU acceptance remains required.

- [Ground Truth Ambient Occlusion](rendering_gtao.md) — slice 5E.5 implemented (Debug/Release and software Vulkan verified; target-GPU/live review pending): spatial GTAO, independent AO depth filtering and half-resolution reconstruction for indirect diffuse lighting.
  A performance correction makes AO opt-in (default Off), reuses a bounded compute-group filter tile,
  factors repeated edge weights, and skips filtering/reconstruction where visibility is already white.
  Contact shading, fade boundaries and diagnostics are preserved; no new frame resources or CPU waits.

- [GPU drawing and conservative visibility](rendering_gpu_visibility.md) — slice 5E implemented (Debug/Release and software Vulkan verified; target-GPU/live review pending): bounded indirect commands, current-frame opaque depth and opt-in reversed-Z Hi-Z.

- [Clustered lighting and ordinary meshes](rendering_clustered_lighting.md) — slice 5D implemented (Debug/Release and software Vulkan verified; target-GPU/live review pending): bounded local lights, reusable mesh submission, alpha modes and replacement of the material preview with owned fixtures.

- [Terrain blending and repetition control](rendering_terrain_blending.md) — slice 5C implemented (software Vulkan verification; target-GPU/live flight pending): height-aware composition, cliff projection, dedicated macro artwork. A performance revision removes stochastic sampling, separates macro filtering, and adds an optional terrain depth prepass; see the linked design for status and tradeoffs.

- [Terrain ridge LOD quality](terrain_lod_quality.md) — filtered geometry and independent normal filtering remain in use. Its historical shared-range and morph controller is superseded by stationary ground LOD with local refinement and stitched boundaries.

- [Terrain materials](rendering_materials.md) — slice 5B implemented (software verification; target-GPU/live flight pending): offline image-generated PBR palettes, bounded bindless binding and stable world mapping.
  The coverage continuity correction uses a material-only smooth ridge-feedback gradient; generated heights and cache identities remain unchanged. See the material design for its regression strategy.

- [HDR and shared PBR](rendering_hdr_pbr.md) — milestone 5A implemented (software Vulkan verified; target-GPU/windowed checks pending): linear HDR, sun/environment lighting, output transforms and owned material fixtures (the interactive preview is replaced by 5D mesh submission).

- [Rendering roadmap](rendering_roadmap.md) — proposed milestone 5 sequence: HDR/PBR, bindless terrain/object materials with 8×/16× anisotropic filtering, clustered forward lighting and transparency, GPU Hi-Z/indirect drawing and GTAO, stable cascaded sun shadows, TAA/HDR bloom, atmosphere/volumetrics, and later GI evaluation. This is a roadmap, not implemented renderer behaviour.

- [Regional terrain geography](terrain_geography.md) — first geography milestone: distinct plains, foothills, mountain ranges and valleys; separate collapsible generation controls (implemented; target-GPU profiling pending).

  Adjustable ridge strength and base detail are implemented: independently controlled ridge/detail contributions
  (defaults 0.50 / 0.65) and matching
  erosion/material slope fields, using the existing generation resources.

  [Ridge stability and rolling plains](terrain_ridge_stability.md) records the smoother erosion-driving field, plains relief, and regression evidence.

- [Hierarchical terrain residency](terrain_hierarchy.md) — milestone 4: dyadic data tiles, parent fallback and transitions, direct GPU publication, longer viewing distances and profiling.
  Generation uses a bounded per-kernel workgroup scheduler driven by completed GPU timings. Its generation target is 4 ms per frame, with 10% headroom for a 3.6 ms estimated allowance. It shares that allowance across successive stages of the active tile, with fixed timestamp storage and unchanged fence-safe publication; see the hierarchy design for limits and verification.

- [Terrain streaming](terrain_streaming.md) — milestone 3: threaded disposable cache, bounded residency, incremental GPU work and camera-driven view radius (implemented).
  Startup pool sizing uses driver-reported allocation requirements, including image padding, before committing GPU memory; capacities adapt to the configured budget.
  [Detail demand and cancellation](terrain_detail_demand.md) specifies bounded motion prediction, request revisions, preemption and fence-safe cancellation (implemented).

- [Terrain LOD](terrain_lod.md) — implemented milestone 2b: quadtree patches, continuous morphing, asynchronous bounds, filtered normal levels and diagnostics.

- [Fixed terrain neighbourhood](terrain_neighbourhood.md) — implemented milestone 2a: batch requests, shared GPU storage, fixed 3×3 rendering, and verification.

- [Terrain generation and inspection](terrain_generation.md) — terrain algorithms and milestone 1 verification; CPU value types, compute passes, cached rendering, fixed 0.5 m demo spacing, startup footprint, filtered per-pixel terrain normals with preallocated GPU storage, camera, tests, and dense normal generation.
- [Terrain sampling investigation](terrain_sampling.md) — measured crease artifacts, correction, visual evidence, diagnostic tools, and resource costs.
- [Planned chunk generation and LOD architecture](#planned-chunk-generation-and-lod-architecture) — LayerProcGen-inspired direction; fixed neighbourhood and LOD implemented; streaming implemented; broader dependency scheduling remains deferred.

When a design grows beyond a concise entry, move its details into a dedicated document under `docs/` and retain a summary and link here.

## Planned chunk generation and LOD architecture

Status: milestones 2a, 2b and 3 are implemented; milestone 4 is implemented with hierarchical data tiles and residency transitions; target-GPU performance verification remains pending. A broader dependency scheduler remains deferred until real consumers require it.

Borrow LayerProcGen's spatial dependency concepts in a small C++ implementation suited to our GPU terrain generation. Start with explicit terrain stages; introduce a general dependency scheduler only when multiple real consumers justify it. A wholesale framework port is not planned. Generation dependencies and rendering LOD have separate responsibilities.

### Layers and spatial dependencies

- **Separate stage inputs and outputs.** A stage consumes completed, read-only inputs and produces its own output. Do not mutate shared inputs while neighbouring chunks depend on them. The initial pipeline is procedural heights followed by normals and render data; regional geography, settlements, and road planning can become upstream inputs later.
- **Request data by world-space bounds plus padding.** A stage declares the surrounding area it needs. Generate all required provider data before running the consumer. Padding must cover the operation's influence distance, including accumulated influence across repeated filters. Our normal-generation halo is the first concrete use of this principle.
- **Preserve determinism across boundaries.** Generation must not depend on request order, which neighbours happen to be resident, or the camera's selected LOD. Use a shared world definition and stable identities; retain the existing distinction between deterministic generation and unproven cross-device bitwise equality.
- **Allow different spatial scales.** Regional planning may cover kilometres while terrain data and render patches cover smaller areas. Dependencies are based on overlap, not matching chunk indices or one-to-one ownership across layers.
- **Generate according to demand.** Streaming requests determine the terrain products currently needed. Broader layers will use these requests to determine their upstream regions. Share overlapping dependencies and retain provider data until its consumers finish. Future terrain, collision, or map consumers may request different extents and representations.

### Chunks, patches, and LOD

| Concept | Responsibility |
| --- | --- |
| Generation/cache chunk | Identifies and stores a region of generated data for a particular layer and representation. |
| Render patch | Draws a portion of terrain at a selected LOD; several patches may cover one data chunk. |
| Generation layer | Defines a kind of world data, its spatial partition, and the inputs needed to produce it. |

Keep these boundaries independent. For example, a 512 m data chunk could contain several smaller render patches so nearby and distant portions can use different geometry detail. The current renderer uses 32×32-cell patches, with smaller patches for small chunks.

Use reusable regular grids with power-of-two LOD steps as the starting direction: 0.5 m mesh spacing near the player, then 1 m, 2 m, 4 m, and coarser spacing with distance. The [implemented LOD design](terrain_lod.md) specifies quadtree selection, continuous morphing, bounds feedback and edge/corner continuity. Camera-dependent representation changes must preserve the underlying world and future collision terrain.

Keep geometry and shading detail separate. Retain the current 0.25 m normal sampling near the player, while establishing suitable filtering and transitions for distant representations. Coarser geometry alone does not solve normal-texture memory or sampling cost. Height consistency and geometry/normal transitions must be designed together.

### Ownership and bounded work

Share grid topology and pipelines, and pool chunk resources with typed handles where slots can be recycled. Separate resident output data from reusable generation scratch storage; bound both residency and concurrent generation. Do not multiply the single-chunk inspection tool's entire allocation by the number of chunks.

Cache identities include seed, generator version, settings, coordinates, and layer/representation where applicable. Camera LOD selection is transient demand, not a change to world identity. Streaming must retain available coarse coverage while refinement is pending and budget generation, uploads, and retirement. Reuse scratch or resident storage only after all relevant CPU and GPU consumers finish.

### Verification

Use small owned fixtures to check neighbouring heights and normals, padding, generation-order independence, negative/distant coordinates, and mixed-resolution edges and corners. Streaming tests cover stale/recycled handles, cache compatibility, delayed refinement and bounded resource exhaustion; a future general dependency scheduler must also test shared-provider lifetimes. Inspect LOD transitions in motion and use Vulkan validation for affected GPU lifetimes. Keep large-world throughput and memory measurements separate from routine correctness tests.

### References

- [LayerProcGen overview](https://runevision.com/tech/layerprocgen/) and [source repository](https://github.com/runevision/LayerProcGen).
- [Layer dependencies](https://runevision.github.io/LayerProcGen/md_LayerDependencies.html) — spatial requests, provider ordering, and demand from consumers.
- [Effect distance and padding](https://runevision.github.io/LayerProcGen/md_EffectDistance.html) — sufficient neighbourhood input for seamless contextual processing.
- [Planning at scale](https://runevision.github.io/LayerProcGen/md_PlanningAtScale.html) — different spatial scales and incremental introduction of planning layers.
- [GPU-based geometry clipmaps](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry) — reference for reusable grids and geometry/normal transitions; not a commitment to this exact LOD layout.

## Testing strategy

Release builds use the toolchain's optimization flags (GCC/Clang `-O3 -DNDEBUG`) and enable CMake interprocedural/link-time optimization when the compiler supports it. `OPENWORLD_RELEASE_IPO=OFF` disables the latter for build diagnostics. Debug remains unaffected; floating-point semantics and CPU architecture portability are preserved. HLSL shaders use DXC `-O3` in both configurations. These CPU build settings do not remove the separate per-frame GPU generation budget described in the terrain designs. Verify effective compile/link commands and run Release CPU/GPU regressions when changing optimization settings.

Follow the [project testing guidance](../AGENTS.md#testing). Each system design should identify its important correctness invariants, failure cases, integration boundaries, and how they will be verified. Coverage should be thorough where it protects our behaviour, without retesting third-party implementations or creating tests that simply mirror our code.

Use small, representative workloads for routine correctness tests. For example, chunk-edge continuity can be checked with a few small neighbouring height grids; cache invalidation can use tiny generated chunks in a temporary directory. Keep the relevant edge conditions intact when reducing computation. Reserve larger workloads for separate stress tests and benchmarks, and document when those are needed.

Fixtures belong to the tests: generate deterministic inputs or maintain small dedicated fixture files alongside the test suite. Tests must not depend on production game assets or existing world data. Any models, materials, textures, or serialized data needed to exercise our integration should be supplied specifically by the test, with outputs isolated from other tests and user data.

Document numerical tolerances, seed choices, GPU or platform requirements, and any checks that cannot run in a given environment. Distinguish correctness results from performance measurements and from checks that remain unverified.

## What to record for a system

Scale the detail to the task. Cover the decisions that help someone implement, review, or extend the system:

- Purpose, scope, and boundaries with neighbouring systems.
- Main abstractions and interfaces, including ownership and lifetime rules.
- Data structures, layout, handles, and invalidation rules.
- Algorithms, expected workload, and relevant complexity or performance tradeoffs.
- Allocation strategy, resource budgets, and behaviour when capacity is exhausted.
- Threading, CPU/GPU synchronization, and deferred resource retirement where relevant.
- Correctness invariants, meaningful test cases, owned fixtures, and reduced workloads for routine tests.
- Required integration or stress checks, validation prerequisites, and any measured performance results.
- Implementation status, known limitations, and unresolved decisions.

Keep entries aligned with the implementation. Distinguish future intentions from behaviour that exists and has been verified.

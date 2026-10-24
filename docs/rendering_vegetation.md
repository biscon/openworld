# Vegetation and ground clutter roadmap

Status: architecture agreed on 2026-10-24. V1 experiment implemented with software
Vulkan verification and native RTX 3060 measurements. Final density/appearance
acceptance remains open; V2–V6 remain proposed. This expands
the vegetation part of [rendering roadmap slice 5J](rendering_roadmap.md#5j--representative-scene-content-and-reflections).
It records the direction, implementation sequence, and acceptance gates. Each slice
needs its own focused implementation plan before coding; exact layouts, capacities,
shader interfaces, and tuning values are deliberately left to those plans.

## Goal and boundaries

Render dense, animated ground vegetation for northern/western European meadows,
moors, and forest understory while retaining budget for trees, water, and other
rendering. Ground-level first/third-person travel drives quality; aerial views
reduce vegetation into matching terrain coverage rather than retaining ground-level
density. The supplied reference images establish a density/appearance direction,
not a requirement to reproduce their apparent card-based technique.

The initial system covers grass, rushes, ferns, flowers, individual plants, heather,
and small shrubs. Its placement and batching must also accommodate later opaque
ground clutter such as small rocks and dead branches. Trees remain a separate
future design with longer visibility and different optimization needs; shared
habitat, asset, and wind inputs must not force trees into grass rendering policies.

Include wind and temporary player bending with recovery. Persistent trampling,
saved damage, harvesting, physical plant simulation, a full biome/climate system,
and a tree renderer are outside this roadmap. Do not introduce a general scene,
dependency-scheduling, or procedural botany framework to deliver ground cover.

## Agreed architecture

### Reusable assets and representations

Use baked tuft meshes containing multiple curved, tapered grass blades. Instances
are tufts, not CPU objects or draw calls for individual blades. Ordinary Vulkan
compute plus indexed instancing is the baseline; mesh shaders are an optional
future optimization, not a device requirement.

| Content | Starting representation |
| --- | --- |
| Grass and rushes | Narrow blade geometry with prebuilt lower-detail variants |
| Ferns and broadleaf plants | Shaped, subdivided leaf/frond cards, with geometric stems where useful |
| Flowers | Small meshes and selective cards |
| Heather and small shrubs | Branch meshes with shaped foliage clusters |
| Later rocks and dead branches | Opaque meshes using the same population and batching infrastructure |

Use cards selectively and trim unnecessary transparent area. Fewer triangles do
not automatically mean lower cost: overlapping masked rectangles can spend more
pixel work than blade geometry. The first slice compares these representations at
similar apparent density before expanding the asset library. Grass impostors are
not a required distance stage; introduce distant cards only if that comparison
establishes a useful quality/performance advantage.

Generate mesh shapes, variants, LODs, and textures offline, save them, and reuse
them. Parameterized curves, frond arrangements, and small branching generators
are sufficient starting tools. An L-system is an optional offline authoring tool
if a concrete plant needs it, not runtime infrastructure.

Image generation may supply source leaf/flower artwork. Follow the existing local
asset-build convention: retain source artwork and recipes, import runtime assets
into the build tree, and rebuild only when inputs change. Runtime startup loads
assets rather than generating their artwork or mesh library again. Prepare
opacity-aware mipmaps with coverage preservation, safe colour edges, atlas padding
where applicable, and coherent normals/material parameters. Validate the resulting
assets rather than assuming generated images are physically consistent PBR maps.

### Population, habitats, and terrain attachment

Separate asset generation from world placement. Deterministically scatter reusable
assets into small, world-aligned vegetation cells as demand changes. Cells are
independent of terrain storage tiles and render patches. Placement runs
incrementally on residency or input changes, not for every plant every frame.

Candidate identity derives from world seed, geographic cell, species, and candidate
index. Population identity also tracks the relevant generator/settings revisions.
Camera movement, loading order, render LOD, and density settings must not reshuffle
the underlying population. Preserve integer world identities and local coordinates
before camera-relative float conversion. Do not assume cross-device bitwise
floating-point reproducibility without verification.

Start with a simple stable mixture of meadow, moor, and understory habitat weights,
plus elevation, slope, and ground suitability. Add broad patches, species clusters,
and individual variation instead of uniformly random coverage. Provide selectable
habitat presets for review. Understory initially means an artistic population type;
it does not claim to derive canopy shade before a tree system exists.

Keep habitat inputs replaceable by a future biome system. Define an exclusion-mask
input for future paths, water, structures, and authored clearings without building
those systems now. Terrain ground appearance and vegetation distribution consume
consistent coverage information, so the final vegetation fade exposes plausible
ground. Exclusions and material coverage must not depend on which plants survived
camera culling or quality thinning.

Stable population and rendered attachment are separate responsibilities. Roots
must agree with the actual terrain triangles, including stitched LOD boundaries;
sampling an unrelated continuous height or using incompatible interpolation can
leave plants floating. Update attachment when the supporting surface changes,
without reseeding the population. Near-ground conformity, plant footprint on
slopes, and coarse-provider transitions are explicit acceptance cases. Missing
terrain inputs defer affected population work rather than guessing unsupported
root heights or blocking a frame.

### Ownership and GPU rendering

```text
terrain + habitat + exclusion inputs
  -> bounded incremental cell placement
  -> persistent compact GPU instance records
  -> GPU cell/instance culling, LOD and density selection
  -> visible lists grouped by asset, LOD and material
  -> indirect instanced drawing
```

| Responsibility | Boundary and owned state |
| --- | --- |
| Asset library/build tools | Immutable mesh/LOD/material assets and deformation metadata; typed resource identities |
| Population/residency | World-keyed cells, input revisions, demand, bounded generation and retirement |
| Terrain provider | Read-only surface data and attachment metadata with explicit reader lifetimes |
| Vegetation renderer | Compact instance pools, per-frame visible/caster lists, indirect commands and pipelines |
| Wind/interaction | Shared wind parameters and a bounded local disturbance field |
| Frame coordinator | Pass ordering, barriers, resource retention and completed-frame diagnostics |

The existing ordinary mesh path prepares substantial CPU records per object; do
not grow that path into one object per tuft. Store compact local position,
orientation, scale, variation, and identity rather than uploading full matrices
per plant. CPU work scales with active cells and bounded work queues; dense
instance selection and command generation remain on the GPU.

Reuse material, lighting, temporal, visibility, and shadow facilities through small
interfaces. Reuse their algorithms/resources where appropriate, not their current
CPU-per-object preparation loops. Vegetation needs GPU-built caster lists rendered
into existing cascaded shadow targets, independently of camera-visible lists.

Optional Hi-Z rejection uses conservative wind-expanded bounds and current
terrain/solid-object depth. Do not use vegetation bounding boxes as solid occluders.
Vegetation depth must be available to downstream depth consumers such as GTAO when
enabled. The implementation plan must order culling, depth, colour, and shadow
passes without introducing a circular dependency or an unnecessary second pyramid.
Hi-Z is an optional measured optimization, not a requirement for open-meadow speed.

Preallocate pools, upload storage, command lists, and scratch at startup or explicit
quality changes. Budget live, pending, and retired allocations together. Retain
terrain providers, assets, descriptors, and instance slots until all submitted
readers finish; use generation checks for recyclable identities. Do not introduce
steady-state allocator traffic, visibility readback decisions, or blocking waits.
Under pressure, defer population or predictably reduce distant decorative density,
with diagnostics; never silently overflow buffers or fall back to enormous numbers
of CPU draw calls. Detailed slices must define capacities and exhaustion behaviour.

### LOD and distance policy

Reduce geometry, population density, shading detail, animation detail, and shadow
work independently. Use projected size as well as distance. Preserve stable blade
and instance subsets across density changes, with modest width compensation where
it preserves coverage without visibly turning blades into paddles. Use stable
thinning and coverage dithering for transitions rather than sorted alpha blending.
Inspect with TAA both enabled and disabled.

These are starting experiment ranges, not final defaults or measured guarantees:

| Content/range | Intended treatment |
| --- | --- |
| Grass within about 15–20 m | Full tuft detail and density |
| Grass to about 40–50 m | Fewer blade segments, blades and instances |
| Grass around 50–80 m | Cheapest geometry and progressive fade into terrain coverage |
| Small plants | Species-dependent LOD/fade, approximately 50–100 m maximum range |
| Shrubs | Size-dependent LOD/fade, potentially approximately 150–200 m maximum range |

Fading grass into terrain is a coverage/colour continuity approximation, not a
promise to preserve individual distant silhouettes. Low aerial and grazing views
must be reviewed so this tradeoff is visible before final quality settings are set.

### Animation, lighting, and shadows

Use shared wind direction/strength, broad moving gusts, and smaller local variation.
Species stiffness and normalized plant height control response; roots remain
anchored. Reduce fine flutter before broad motion at distance. Establish an
explicit maximum deformation for conservative camera and shadow bounds.

Player interaction uses a small GPU field around the player, stamped by a bounded
list of interaction shapes and relaxed over time. This provides bending and a
short-lived disturbance trail without per-plant simulation or persistent saves.
The interaction anchor is independent of the rendering camera; the free-fly demo
needs a controllable interaction proxy. Field scrolling, teleport/reset behaviour,
and previous submitted state belong in the interaction slice's detailed plan.

Add two-sided foliage shading with a restrained thin-leaf transmission
approximation, integrated with the existing sun, environment, local lights, and
atmospheric transport. Avoid an unrelated full subsurface-scattering project.
Colour, depth, and shadow passes must use matching deformation and mask rules.
Motion vectors include previous submitted-frame wind and interaction, not only
instance transforms. New population, changed attachments, and incompatible LODs
reject temporal correspondence locally where it cannot be reconstructed safely.

Shadow work has its own distance and population budget. The agreed production
policy disables individual grass-blade casting by default while retaining reception
of terrain, tree and other object shadows. Grass participates in the existing GTAO
depth/lighting path when AO is enabled. GTAO supplies local ambient occlusion, not
directional sun shadows or guaranteed per-blade contact detail. Shrubs, larger
plants and rocks retain shadow casting with category-specific distance/LOD budgets.
Offscreen nearby casters can still affect visible receivers. Within the casting
range, deformation must match visible geometry. The V1 probe retains its historical
25 m grass-casting default for reproducibility; `--grass-shadow-distance 0` selects
the agreed policy. Production defaults belong to the subsequent implementation.

## Performance and validation gates

Retain the engine objective of RTX 3060, 1080p, 120 FPS / 8.33 ms, with native-output
measurements as well. The user accepts an eventual 60 FPS / 16.67 ms full-feature
scene on this card, but the development target and tight feature budgets remain
unchanged. Existing full-scene performance work remains unresolved; this
roadmap does not assume that the following time is already available. Trees, water,
terrain generation, and other effects need explicit remaining headroom.

| Provisional target | Accounting |
| --- | --- |
| At most approximately 1.5 ms incremental steady-state GPU time | Vegetation culling, attachment work, animation/interaction, depth, colour and shadows; confirm with total-frame on/off comparisons |
| Approximately 0.25 ms steady-state CPU preparation | Population management and submission, with spikes reported separately |
| 128 MiB total vegetation GPU budget | Assets, resident instances, frame lists, scratch, interaction, pending uploads and retirement; count actual allocation requirements |
| Bounded incremental population work | Measure separately and coordinate with terrain generation rather than adding an unrestricted compute allowance |

These are engineering targets to validate, not measured performance claims. Record
paired uncapped Release runs, exact settings, visible density, CPU/GPU median and
p95 frame times, per-pass timings, allocation behaviour, and memory. Include warm
and cold streaming, camera rotation, ground travel, grazing views, low aerial views,
strong wind, sun-angle changes, and shadow/TAA/Hi-Z comparisons. Compare cards and
blade tufts at comparable apparent coverage, not just identical instance counts.

If the target fails, first examine wasted work and reduce distant shadows, fine
animation, and distance coverage before sacrificing nearby density. Present the
measured quality/frame-time tradeoff before changing the overall frame objective.
Software Vulkan validates correctness but cannot sign off RTX 3060 performance;
unavailable target hardware leaves the performance gate explicitly pending.

Every slice adds focused owned fixtures and Vulkan synchronization validation where
available. Routine tests use small deterministic data; production assets and world
saves are not fixtures. Keep large stress/performance runs separate. Coverage must
include population order independence, cell boundaries, negative/large coordinates,
terrain attachment and refinement, masks/LOD transitions, capacity pressure, stale
handles, eviction with frames in flight, wind-expanded bounds, offscreen casters,
interaction recovery, and temporal trails. Test both available material-binding
paths and define bounded feature fallbacks in the relevant implementation plans.

## Implementation slices

Use IDs **5J-V1 through 5J-V6** so these do not imply dependencies on the other 5J
content projects. The sequence is intentional: establish the representation and
cost before broad content production. Tests and profiling accompany each slice;
V6 is the final integration gate, not the first opportunity to find regressions.

### 5J-V1 — Representation and budget experiment

Status: experiment implemented; Debug/Release, software Vulkan and native RTX 3060
synchronization validation pass. Native representation and shadow-policy measurements
are recorded in the [hardware report](measurements/vegetation_5j_v1/rtx3060/README.md).
Final asset coverage, foliage shading and integrated scene acceptance remain open.
Detailed design and findings: [representation experiment](rendering_vegetation_experiment.md).

- Build an owned meadow fixture comparing blade tufts with trimmed grass cards,
  using bounded indirect instancing and representative LOD, wind, and shadow work.
- Include close, grazing, moving, and low aerial views with existing lighting/TAA.
  Temporary benchmark plumbing must not establish a second production renderer.
- Deliver captures and paired CPU/GPU/memory measurements. Confirm the default
  representation and whether a distant card stage earns its complexity. Record
  target-GPU availability and any unresolved performance gate explicitly.

### Handoff after V1 (2026-10-24)

Read the [native hardware report](measurements/vegetation_5j_v1/rtx3060/README.md)
and [experiment design](rendering_vegetation_experiment.md) before planning V2.
The hardware experiment has been run; there is no outstanding request for the user
to run it again. GPU access requires execution outside the sandbox, not a driver fix.
Native synchronization validation passed, including the receive-only/GTAO captures.

- **Settled direction:** blade tufts first; no grass shadow casting. Retain scene
  shadow reception and GTAO compatibility. Shrubs, bigger plants and rocks cast
  with their own limits. Trees remain a separate project. The original casting
  path stays in the experiment solely for reproducible comparisons.
- **Measured cost:** original moving blades with casting 2.749 ms; receiving-only
  1.248 ms, or 1.388 ms with half GTAO (three-repeat range 1.196–1.555 ms). These
  are incremental vegetation costs; GTAO is also enabled in the no-vegetation
  baseline for the latter pair. Do not advertise 1.388 ms as the combined cost of
  the whole GTAO system and grass. Budget remains approximately 1.5 ms.
- **Representation:** tested cards/hybrid have no median win across the five views.
  Keep the comparison harness, but do not add distant cards to production without
  new evidence. This conclusion does not reject cards for broad-leaf plants.
- **Next work:** plan **5J-V2**, then implement only after that plan is agreed. Focus
  on saved reusable assets, representative coverage, foliage shading and animation
  contracts. The plain-ground fixture is dark/sparse and lacks leaf transmission;
  its appearance is not accepted as the reference target. Re-measure after assets
  and coverage improve. Full-scene FPS and production streaming remain unproven.
- **Concrete efficiency opportunity:** receive-only selection still dispatches all
  cascade views (~0.50 ms). Production should exclude grass caster bounds/lists
  and skip their selection work, preserving scene shadow reception. Treat this as
  measured waste to address, not an already measured optimization.
- **Budgets:** keep 128 MiB vegetation GPU storage and ~0.25 ms CPU preparation.
  V1 measured ~69 MiB, ~0.035–0.040 ms preparation and zero C++ allocation calls
  inside validation-disabled steady-state render intervals. Reserve headroom for
  trees/water; eventual 60 FPS full-feature RTX 3060 is acceptable without relaxing
  the tighter development target or these feature budgets.

Implementation entry points: `src/vegetation.*` (fixtures/data),
`src/vulkan_vegetation.*` (bounded GPU experiment), `shaders/vegetation*`,
`tools/vegetation_probe.cpp`, and `tests/vegetation_cpu.cpp` / `vegetation_gpu.cpp`.
The experiment remains disabled in the ordinary application; its API settings and
probe still default to the historical 25 m casting comparison. V2 must implement
the settled production policy explicitly rather than inherit that experiment default.

### 5J-V2 — Asset and foliage rendering foundation

- Build the saved asset/LOD pipeline and a small representative asset set, including
  masked plants; add opacity-aware texture import and deformation metadata.
- Establish reusable compact instancing, foliage shading, shared wind deformation,
  and matching colour/depth/shadow evaluation with submitted-frame motion history.
- Accept when owned fixtures demonstrate reusable assets, correct two-sided/masked
  lighting, stable animation and temporal behaviour, and bounded resource lifetimes.
  Do not expand the full plant library before the V1 tradeoff is understood.

### 5J-V3 — Streamed placement and terrain attachment

- Add stable cell identities, bounded incremental GPU population, residency, and
  terrain reader retention. Define the small terrain/habitat/exclusion input boundary.
- Begin with controlled habitat fixtures/presets; implement terrain conformity and
  attachment updates without camera-dependent population changes.
- Accept seamless deterministic placement, grounded plants through surface/provider
  changes, safe eviction/teleport recovery, and bounded cold/warm streaming behaviour.

### 5J-V4 — Production visibility and LOD

- Productionize GPU cell/instance selection, asset/LOD/material batching, stable
  density reduction, fades, and independent shading/animation/shadow limits.
- Add GPU caster lists for the existing CSM targets and optional shared Hi-Z
  integration. Connect terrain coverage to the final vegetation fade.
- Accept transitions without obvious moving density rings, conservative animated
  culling, correct offscreen shadows, bounded overflow behaviour, and measured
  cost/benefit for optional occlusion. Recheck V1 performance with streamed content.

### 5J-V5 — Interaction and habitat variety

- Add the player-centred bend/recovery field and bounded interaction shapes, with
  correct previous-frame deformation and a controllable demo proxy.
- Populate the simple blended meadow/moor/understory habitats; add representative
  ferns, flowers, heather and shrubs with species-specific placement and response.
- Accept convincing clustered variation, clearings, coordinated wind, temporary
  disturbance/recovery, and correct behaviour when the player and camera separate.
  Validate scrolling and teleports without trails from stale field contents.

### 5J-V6 — Integration and acceptance

- Finalize quality presets, diagnostics, review views, and repeatable performance
  probes. Keep vegetation independently toggleable for paired measurements.
- Review full grass/plant/shrub scenes under streaming, terrain refinement, changing
  sun, wind and interaction; exercise TAA, depth, shadows, atmosphere and optional AO.
- Complete focused Debug/Release correctness and shader checks, Vulkan validation,
  production-scale allocation/memory stress, and target-GPU frame-time acceptance.
  Record measured budgets, remaining headroom, limitations, and any unexecuted gates.

Before each slice, document its concrete interfaces, data layout, ownership,
capacities, failure/fallback behaviour, and focused tests. Link that detailed design
from this roadmap and the [system design index](system_design.md), clearly separating
proposed work from implemented and verified behaviour. Later tree rendering and
additional nonliving clutter content receive their own plans.

## Research basis

These sources inform the choices; their implementations and hardware results are
not benchmarks for this engine.

- [Sucker Punch: Procedural Grass in Ghost of Tsushima, GDC 2021](https://gdcvault.com/play/1027214/) — production precedent for GPU grass blades rather than requiring cards.
- [AMD GPUOpen: Procedural grass rendering](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/) — curved blades, patch organization, fractional density reduction and width compensation; its mesh-shader backend is not a requirement here.
- [Sucker Punch: How stunning visual effects bring Ghost of Tsushima to life](https://blog.playstation.com/2021/01/12/how-stunning-visual-effects-bring-ghost-of-tsushima-to-life/) — shared broad/detail gust patterns across vegetation.
- [Crytek: Vegetation Procedural Animation and Shading in Crysis](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis) — main/detail bending and thin-foliage shading; not a wholesale adoption of its older rendering choices.
- [Khronos: GPU Rendering and Multi-Draw Indirect](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html) — compute-driven draw generation and Vulkan feature fallbacks.
- [Microsoft DirectXTex: ScaleMipMapsAlphaForCoverage](https://github.com/microsoft/DirectXTex/wiki/ScaleMipMapsAlphaForCoverage) — alpha coverage preservation during mip preparation; no DirectX runtime dependency is proposed.
- [Epic: Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage) — experimental instanced-part, voxel and skinning approach; useful future research for trees, beyond this renderer's initial scope.

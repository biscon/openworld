# Terrain and open-world milestones

This document records the agreed development order following the Vulkan hello-world demo. The goal is a procedurally generated world whose extent is independent of the amount of terrain resident in memory.

## Terrain generation approach

Use Rune Skovbo Johansen's erosion filter on top of a procedural base height field. This produces the appearance of erosion through independently evaluated terrain samples, rather than a simulation of droplets transporting sediment. Independent evaluation makes it suitable for generating chunks on demand.

Broad geography—mountain ranges, plains, and coastlines—belongs in the base world generator. Keep the erosion stage configurable and replaceable. Its drainage-like patterns are not a guarantee of connected rivers; actual river networks are a separate future concern.

Derive rendering normals from the final sampled heights rather than relying on the filter's approximate output derivatives. Preserve the upstream license and attribution when porting its implementation, which is released under MPL 2.0.

References:

- [Rune's Fast and Gorgeous Erosion Filter](https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html) — selected approach and implementation references.
- [Hans Theobald Beyer's Implementation of a Method for Hydraulic Erosion](https://www.firespark.de/resources/downloads/implementation%20of%20a%20methode%20for%20hydraulic%20erosion.pdf) — background on simulated droplet erosion; not the selected algorithm.

## Design requirements from the first milestone

- **One continuous world function:** chunks sample the same world-space terrain function. Chunk boundaries are storage boundaries, not generation boundaries. Do not independently normalize chunk heights or assign unrelated noise seeds to adjacent chunks.
- **Large-world coordinates:** use integer chunk coordinates plus local positions and render relative to the camera. Noise evaluation must also preserve precision at large coordinates; shifting the rendering origin alone does not solve generation precision.
- **Independent terrain data:** keep height data separate from its rendering mesh so it can later support collision, persistence, and different levels of detail.
- **Repeatable generation:** identify terrain by world seed, generator version, settings, and coordinates. Generation order must not change the resulting terrain. Cross-device bitwise reproducibility is not assumed without verification.
- **Height-field foundation:** start with a height field. Caves and overhangs cannot be represented directly by it and may be added through separate geometry later.
- **Bounded resource use:** world extent must not require loading or generating the entire world. Streaming will enforce memory and per-frame work budgets.
- **Separate spatial responsibilities:** generation/cache chunks, render patches, and generation layers need not share boundaries or sizes. Adopt spatial dependency concepts from LayerProcGen as described in [system design](system_design.md#planned-chunk-generation-and-lod-architecture).

Milestones 2a (seamless chunks), 2b (basic LOD), and 3 (caching and bounded streaming) are implemented. Milestone 4 extends viewing distance and residency; see the hierarchical design and verification notes.

## Milestone 1: One terrain chunk and a free-fly camera

Status: implemented. See [terrain design and verification](terrain_generation.md).

### Scope

- Generate a procedural base height field and apply Rune's erosion filter.
- Use Vulkan compute and HLSL to generate height data when settings change, rather than regenerating it every frame.
- Keep terrain generation separate from the renderer.
- Render a regular triangle grid with normals derived from the final heights.
- Add a free-fly camera for inspecting terrain at different distances and angles.
- Include simple directional sunlight, ambient illumination, and a plain surface colour to make terrain shape readable.
- Add ImGui controls for the world seed, terrain scale, and erosion strength, plus a before/after erosion toggle.
- Include chunk coordinates in the design even though only one chunk is displayed.
- Use 0.5 m sample spacing over a configurable footprint, defaulting to a 512 m patch. Sample a filtered terrain-normal texture generated at 0.25 m spacing per pixel, with a draw-only vertex-normal comparison for inspection.

### Completion criteria

A configurable terrain chunk can be generated, inspected with the camera, and compared with its uneroded base. Changing generation settings updates the terrain; ordinary camera movement does not regenerate it. Surface normals and simple lighting clearly reveal the terrain's shape.

Disk storage, streaming, terrain LOD, textures, and full PBR lighting are outside this milestone.

## Milestone 2a: A seamless fixed neighbourhood of chunks

Status: implemented. See [neighbourhood design and verification](terrain_neighbourhood.md). The demo defaults to a fixed 3×3 grid of 128 m chunks. Footprint is selected at startup; GPU capacity matches that footprint.

### Scope

- Render a small fixed neighbourhood, initially a 3×3 grid, at 0.5 m mesh spacing. Use smaller chunk footprints if needed to keep this boundary demo manageable; changing footprint must not change the world function.
- Generate each chunk independently from the shared world function.
- Separate chunk data from render patches, share grid topology, and reuse bounded generation scratch storage instead of duplicating the entire single-chunk inspection allocation for each chunk.
- Ensure neighbouring chunks agree on border heights and normals, including any neighbouring samples needed to compute normals.
- Verify that generation order does not affect the result.
- Exercise negative chunk coordinates and locations far from the origin.
- Verify camera-relative positioning across the neighbourhood. Use small owned fixtures for automated border and generation-order checks.

### Completion criteria

The neighbourhood has no geometric cracks or lighting seams at shared edges. Regenerating chunks in a different order produces the same terrain. Negative and distant coordinates behave consistently within the supported coordinate range.

Complete this boundary validation before introducing mixed-resolution rendering, disk storage, or streaming.

## Milestone 2b: Basic terrain LOD

Status: implemented. See [LOD layout, transition rules, and verification](terrain_lod.md). The fixed neighbourhood now uses quadtree patches with smooth geometry/normal transitions and frustum culling.

### Scope

- Use reusable 32×32-cell render patches in a quadtree, power-of-two chunk sizes and sampling steps, quadrant masks, and continuous vertex collapse toward the next coarser grid. Smaller chunks use smaller patches.
- Preserve 0.5 m mesh spacing near the player, with automatic 1 m, 2 m, 4 m, and coarser levels as needed farther away. These are rendering levels, not user-selectable near-ground quality settings.
- Add visibility culling and camera-dependent LOD selection over a fixed region, without disk caching or world streaming yet.
- Connect neighbouring patches at different resolutions without cracks, including corners, and exercise transitions while moving the camera.
- Establish height and normal representations for each level. Preserve finer per-pixel normals near the player and provide appropriate filtering and transitions at coarser levels.
- Keep the underlying world independent of rendering LOD so future collision and gameplay data do not change with camera detail selection.
- Use small fixtures for mixed-resolution edge/corner checks and generation consistency; inspect movement for popping and shading discontinuities.

### Completion criteria

The fixed landscape renders with fine detail nearby and reduced geometry farther away. Mixed-resolution boundaries remain connected as LOD selection changes. Height/normal representation and transition rules are documented and verified sufficiently to serve as the basis for streaming; remaining visual refinement belongs in milestone 4.

## Milestone 3: Disk caching and bounded streaming

Status: implemented. See [streaming design and verification](terrain_streaming.md). Two cache workers handle filesystem work; generation and transfers progress in bounded GPU batches. The demo follows the camera with radius 0–8 (default 2), plus coverage prefetch. As agreed during planning, every resident chunk retains full 0.5 m heights; coarse normals provide initial coverage and fine normals stream independently. Reducing distant height residency remains milestone 4.

### Scope

- First add chunk serialization and loading for the representations established in milestone 2b, then asynchronous streaming around the moving camera.
- Treat generated terrain on disk as a cache: missing chunks can be regenerated.
- Associate cached data with the world seed, generator version, settings identity, and chunk coordinates. Reject incompatible cached terrain rather than mixing generator outputs.
- Include layer and representation/resolution identity where distinct generated outputs require it; keep camera-dependent LOD selection separate from world identity.
- Coordinate asynchronous chunk requests, disk reads, generation, GPU uploads, and eviction.
- Integrate LOD demand with streaming, retaining coarse coverage while finer terrain is pending. Define behaviour when requests exceed available budgets.
- Enforce explicit limits on resident memory and work performed each frame.
- Keep future persistent player edits conceptually separate from the disposable procedural cache.

### Completion criteria

Flying across chunk boundaries requests and displays new terrain while old terrain is evicted. Available coarse terrain remains visible while refinement is pending. Cached chunks can be loaded on a subsequent run; missing or incompatible entries are regenerated. Memory remains bounded, and generation or upload work does not introduce unbounded frame stalls. Small fixtures exercise cache invalidation, delayed refinement, and resource reuse; larger workloads separately establish streaming performance.

## Milestone 4: Larger viewing distances and LOD refinement

Status: implemented. See [hierarchical residency and verification](terrain_hierarchy.md). Default views extend about 8 km, with a slider up to about 16 km; the near grid remains 0.5 m. Software Vulkan correctness and large-view probes pass. RTX 3060 profiling against the 120 FPS target remains pending hardware availability.

### Scope

- Refine the LOD and streaming systems established in milestones 2b and 3, including distant height residency when longer distances require it.
- Improve LOD selection and geometry/normal transitions to reduce visible popping and shading discontinuities.
- Extend viewing distance while keeping geometry and memory costs controlled.
- Profile representative large landscapes, including frame-time spikes, generation/upload throughput, memory use, and behaviour under resource pressure.

### Completion criteria

The camera can traverse a larger visible landscape with appropriate detail near and far. Neighbouring LODs connect correctly, and transitions are checked for visible popping and shading discontinuities.

Streaming controls residency; LOD controls rendering cost. Both are established before this milestone and refined here before investing heavily in visual polish.

## Regional geography milestone 1: Distinct base landforms

Status: implemented; software Vulkan correctness passes, but full-size cold flights exceed the default 60-second settling deadline (complete in about 75 seconds after stopping). RTX 3060 profiling remains pending. See [regional terrain geography](terrain_geography.md).
Replace the uniform base generator with regional plains, foothills and rounded
ridged mountains with broad valley floors, recognizable without erosion. Preserve
independent generation and streaming, adapt erosion to regional relief, and split
base settings into a separate collapsible ImGui window. Warping and connected
river networks remain later work. Completion includes gradient, seam, cache and
Vulkan checks plus multi-seed landscape review and generation profiling.

## Milestone 5: Materials and the broader rendering system

Status: current development focus; implementation slices are proposed in the
[rendering roadmap](rendering_roadmap.md). [Slice 5A](rendering_hdr_pbr.md) is implemented
with software Vulkan verification; live windowed/target-GPU checks remain pending.
[Slice 5B](rendering_materials.md) adds image-generated terrain palettes, bindless/fixed
texture binding, 8×/16× filtering and stable world-space mapping.
[Slice 5C](rendering_terrain_blending.md) adds height-aware blending, signed cliff
projection and dedicated macro artwork with independent filtering. An optional terrain depth prepass is available for performance comparison; stochastic sampling was removed after visual/cost review.
[Slice 5D](rendering_clustered_lighting.md) implements bounded clustered point/spot lights,
ordinary mesh submission and opaque/masked/blended materials, with a toggleable terrain demo.
The material-preview scene is replaced by owned mesh fixtures. Debug/Release and software
Vulkan checks pass; target-GPU/live review remains pending.
[Slice 5E](rendering_gpu_visibility.md) implements bounded GPU indirect drawing and
opt-in conservative current-frame Hi-Z, with Debug/Release and software Vulkan
verification. [Slice 5E.5](rendering_gtao.md) adds spatial GTAO for local indirect diffuse
occlusion, defaulting to Off with Half/Full opt-in modes. Debug/Release and
software Vulkan checks pass; target-GPU/live review remains pending.
[Slice 5F](rendering_shadows.md) implements stable CSM: four near
cascades and one far mountain cascade. Native review rejected the virtual path's
motion quality and cost; that implementation and its dense fallback are removed.
Shared rendered terrain surfaces replace the canonical receiver workaround.
Debug/Release and software Vulkan checks pass; target-GPU quality and performance
gates remain pending.

### Scope

- Establish linear HDR lighting, exposure, an ACES-style output transform and shared PBR materials.
- Add bounded bindless material descriptors, trilinear/8×/16× anisotropic filtering, terrain texture arrays, stable world-space mapping, height blending, cliff projection, multiscale variation.
- Author the terrain texture artwork with Codex image generation; validate seamless tiling and coherent PBR maps through the offline workflow in roadmap slice 5B.
- Use clustered forward lighting for many unshadowed local lights and transparent materials.
- Add GPU indirect drawing and conservative Hi-Z occlusion before stable cascaded sun shadows for the sun.
- Add GTAO after the current-depth foundation, using it for local indirect-light occlusion with an explicit distance fade and later GI integration.
- Use TAA as the primary antialiasing method and add HDR bloom before the display transform.
- Add a skydome/physical atmosphere, aerial perspective and a deliberate distant-coverage policy, followed by local froxel volumetrics.
- Follow with separately scoped vegetation, GPU particles, reflection probes and screen-space reflections; evaluate SDF/voxel GI and DDGI after the scene and lighting foundations exist.

### Completion criteria

Use the ordered slices and per-increment acceptance criteria in the roadmap. Each
increment needs a reviewable demo, owned correctness fixtures, Vulkan validation
where available, and relevant target-GPU measurements. This milestone describes
a development phase rather than requiring every rendering feature in one demo;
advanced material caches, shadow options and GI remain explicit evaluation gates.

## Future milestone: Regional geography and connected drainage

Status: deferred until after the current rendering focus. This records future
directions, not a finalized algorithm or implementation sequence. Split the work
into separately designed and verified increments when terrain generation resumes.

### Scope

- Add domain warping to make mountain ranges and regional boundaries less regular while preserving continuous world-space sampling and correct gradients.
- Generate connected river networks and drainage-aware valley carving. Treat this as a separate system from the existing erosion filter, which provides erosion-like surface detail without guaranteeing connected drainage.
- Develop broader regional geography, including coastlines. Evaluate regional simulation or planning where it provides a concrete benefit; no simulation method is selected yet.
- Preserve deterministic generation across chunk boundaries and generation order, large-world coordinate precision, and bounded streaming memory and work. Define any regional dependencies and cache identity changes before implementation.

### Completion criteria

Multi-seed reviews demonstrate less regular regional landforms, coherent coastlines,
and connected drainage across chunk and regional boundaries. Focused tests verify
sampling continuity, repeatability and dependency lifetimes; representative streaming
and generation measurements establish the cost of each increment. Agree on detailed
acceptance criteria and designs before starting each increment.

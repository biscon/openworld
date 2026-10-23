# Milestone 5 rendering roadmap

Status: roadmap updated 2026-10-23. Slices 5A through 5G.2, including 5E.5, are implemented; optional 5G.3 is deferred and subsequent slices remain proposed. Software verification does not replace pending target-GPU acceptance. See the implementation documents for verification and limitations. This expands [milestone 5](milestones.md#milestone-5-materials-and-the-broader-rendering-system); terrain geography remains a separate deferred milestone.

## Direction and starting point

Build a clustered forward renderer with linear HDR lighting, reusable PBR materials, layered terrain texturing, GPU visibility and indirect drawing, and stable **cascaded shadow maps** for the sun. Use bindless material textures with trilinear/anisotropic filtering, GTAO for near-field ambient occlusion, TAA as the main antialiasing method, and HDR bloom. Add sky/aerial perspective and local volumetrics after the material and lighting foundation. Keep GI an explicitly evaluated later extension.

The implemented 5A foundation has RGBA16F pre-exposed scene colour, shared GGX PBR, a sun and analytic environment, ACES fitted/AgX output, owned material fixtures and display-linear ImGui composition. It retains Vulkan 1.3 dynamic rendering, synchronization2, reversed-Z D32 depth (clear 0, compare GREATER), camera-relative terrain rendering, bounded hierarchical residency and terrain LOD. 5B adds image-generated terrain palettes, bounded bindless/fixed texture binding, stable world mapping and anisotropic filtering. 5C adds normalized height blending, signed cliff projection, dedicated macro artwork with separate trilinear filtering and an optional terrain depth prepass; see the [5C design](rendering_terrain_blending.md). 5D adds ordinary mesh submission, full-depth clustered local lights and sorted transparency, replacing the interactive preview with owned fixtures. 5E adds bounded GPU indirect drawing and opt-in current-frame Hi-Z for terrain and opaque/masked meshes. Its prepass depth is sampleable and retained. 5E.5 adds optional GTAO; 5F adds stable cascaded sun shadows with shared rendered terrain geometry. Atmosphere remains future work. See the [5E design and verification](rendering_gpu_visibility.md). Relevant starting points are `src/renderer.cpp`, `src/renderer.hpp`, `src/lighting.hpp`, `shaders/lighting.hlsli`, `src/vulkan_terrain_stream.cpp` and `src/camera.cpp`; see the [5A design](rendering_hdr_pbr.md).

Preserve those working terrain systems. New rendering consumers must not change the procedural world or turn terrain cache files into camera-dependent shading caches. The RTX 3060 shown in the demo is the initial performance reference; previous software-Vulkan correctness runs establish no target-GPU rendering budget.

## Recommended order

Each row is a separately reviewable increment, with smaller slices below. Dependencies describe data requirements; later features are not prerequisites for shipping the earlier demos.

| Increment | Visible result | Depends on |
| --- | --- | --- |
| 5A — HDR and PBR foundation | Correct exposure and material response under a sun and environment | Existing renderer |
| 5B — Material textures, bindless binding and terrain mapping | Filtered grass, soil, rock and gravel with stable world-space scale | 5A |
| 5C — Terrain blending and repetition control | Natural transitions, textured cliffs, macro variation | 5B |
| 5D — Clustered lights and transparency | Many bounded local lights; lit transparent test objects | 5A–B |
| 5E — GPU drawing and Hi-Z visibility | GPU-compacted visible draws; conservative occlusion | 5B; integrate 5D |
| 5E.5 — GTAO | Local ambient contact/crevice shading from current depth | 5E depth prepass; 5A indirect-light interface |
| 5F — Cascaded sun shadows | Stable near detail and coarse mountain shadows with bounded caster residency | Shared terrain geometry and mesh materials |
| 5G — TAA and HDR post-processing | Stable fine detail in motion and controlled bloom | 5C–F; integrate 5E.5 |
| 5H — Sky, aerial perspective and view boundary | Coherent sky/sun lighting and atmospheric depth | 5A, 5F–G |
| 5I — Froxel volumetrics | Lit local fog, mist and light shafts | 5D, 5F–H |
| 5J — Vegetation, particles and reflections | Representative populated rendering workload | 5D–I; subdivide independently |
| 5K — GI evaluation and selected implementation | Budgeted indirect diffuse illumination | Stable lighting, scene and streaming contracts |

**Slice 5A is implemented:** see [HDR/PBR design and verification](rendering_hdr_pbr.md). Review its controlled fixtures and complete target-GPU/windowed checks alongside the [5B material review](rendering_materials.md). This roadmap does not commit us to completing all of milestone 5 before reviewing the result.

## Research conclusions: terrain materials

There is no single replacement for splatting that solves blending, projection, repetition and residency together. These are different responsibilities. Current Unreal and Unity terrain workflows still expose weighted layers and height blending. This is evidence of continued use, not a claim that every modern engine uses the same algorithm. [Unreal landscape materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-materials-in-unreal-engine), [Unity terrain layers](https://docs.unity.com/en-us/engine/6000.7/manual/creating-environments/script-terrain/terrain-textures/class-terrain-layer).

The 2023 Call of Duty terrain presentation is a useful production reference: material indices bound local layer cost, world-based rotations hide repetition, larger-scale texture sampling supports distant views, and adaptive virtual texturing caches material composition. Its specific representation also has transition and authoring tradeoffs. Adopt the bounded-cost principle; do not copy its authored-world pipeline wholesale into our procedural world. [Large Scale Terrain Rendering, SIGGRAPH 2023](https://advances.realtimerendering.com/s2023/Etienne%28ATVI%29-Large%20Scale%20Terrain%20Rendering%20with%20notes%20%28Advances%202023%29.pdf).

| Technique | Responsibility and tradeoff | Choice for this engine |
| --- | --- | --- |
| Weighted splatting | Distributes materials; many global layers can make every pixel expensive | Start with four procedural layer weights and a shared texture-array palette |
| Height-aware blending | Uses material micro-height to make dirt collect between stones rather than merely crossfade colour | Add in 5C, with a normalized fallback when all contributions vanish; this is not terrain displacement |
| Indexed materials | Separates palette size from local material count; categorical IDs need special filtering | Evaluate once four layers actually constrain content; preserve a bounded local list |
| Triplanar mapping | Avoids stretched vertical cliffs, but can multiply texture fetches | Use on steep surfaces, with correct projection-space normal handling; use cheaper top projection on plains |
| Multiscale mapping | Separates near grain, mid-scale patterns and broad regional variation | Include early; keep scales anchored to the world and fade detail by footprint |
| Stochastic tiling/blending | Breaks repeated motifs; adds samples and can alter contrast | Removed after visual review: extra cost did not justify the result |
| Runtime virtual texturing/material caches | Amortizes expensive composition and supports broad palettes | Profile-gated extension; not required for an infinite world with a small repeating palette |
| Parallax/displacement | Adds apparent depth but complicates shadows, LOD and sampling | Defer; do not change geometry just to introduce height blending |

Terrain3D provides another current, inspectable example of automatic terrain shading combined with material IDs and local blend values. It is useful evidence that indexed control data remains practical, without prescribing its two-material representation for us. [Terrain3D texturing](https://terrain3d.readthedocs.io/en/stable/docs/texture_painting.html).

Stochastic material sampling was removed after user review found its cost unjustified by the visible result. Conventional micro textures and dedicated macro variation remain the supported path.

### 5A — HDR and reusable PBR

Status: implemented; [design, controls, captures and verification](rendering_hdr_pbr.md). Target-GPU and live windowed verification remain pending.

1. **Explicit colour pipeline.** Add a scene-linear RGBA16F target, exposure and a display-transform pass. Keep sRGB presentation initially: internal HDR does not require an HDR monitor. Decode colour textures as sRGB; keep normals, roughness, metallic, AO and height linear. Define units, working primaries and exposure conventions before shader implementation, including pre-exposure to keep physical daylight values within FP16 range and history rescaling when exposure changes. Compose ImGui after the display transform, with exactly one output transfer encoding.
2. **Shared material evaluation.** Introduce a small `SurfaceMaterial` shader result (base colour, shading normal, roughness, metallic, AO, emissive, opacity) consumed by shared lighting. Use a GGX/Smith/Schlick metallic-roughness model with energy-aware diffuse/specular weighting, a roughness floor and a documented normal convention. Terrain is normally dielectric. Material evaluation must be reusable by future meshes and transparent surfaces. [Filament's PBR reference](https://google.github.io/filament/main/filament.html).
3. **Lighting baseline.** One directional sun plus a controlled environment: diffuse irradiance and prefiltered specular IBL/BRDF lookup. Start with an owned analytic environment fixture; this enables useful roughness/metal tests before a physical sky. Environment light is not local bounced GI.
4. **Output transform.** Start with a documented ACES-style fitted transform, clearly labelled an approximation, behind one output-stage interface. Compare against AgX on sky blue, sun highlights, saturated lights, grass and snow before settling the look. Full ACES 2 is a broader colour/gamut transform, not a five-coefficient curve. Keep exposure, colour conversion and tone mapping separate so changing the look does not require retuning material values. [ACES 2 output transforms](https://docs.acescentral.com/system-components/output-transforms/), [Blender's AgX description](https://docs.blender.org/manual/en/latest/render/color_management.html?highlight=filmic).

Acceptance: owned roughness/metallic swatches, neutral ramps and emissive values above 1; sun-only and environment-only views; finite results at grazing angles; no double gamma; matching offscreen/display paths; resize and resource retirement under Vulkan validation. Preserve existing terrain diagnostic modes through an explicit diagnostic display path.

### 5B — Material assets, bindless textures and basic terrain mapping

Status: implemented; [design, controls, generated assets and verification](rendering_materials.md). Target-GPU performance and live flight review remain pending.

**Asset authoring decision:** use Codex's image-generation tool to create the terrain texture artwork, starting with grass, soil, rock and gravel. This is the chosen production-art workflow for this milestone. Generate and validate one complete material first, then expand the palette with a consistent visual style and physical scale. Generation is an offline authoring step; runtime rendering uses ordinary imported texture assets.

Prompt for an orthographic surface view, seamless repetition in both axes, a stated physical footprint, and neutral base colour without directional shadows, highlights, perspective or baked ambient occlusion. Inspect offset seams and a repeated grid: requesting seamless output does not establish that it tiles correctly. Iterate with image-generation edits to remove seams, conspicuous repeated landmarks and baked lighting. Review the texture at ground level and from a distance in the engine.

Build a coherent PBR set from each approved artwork. Image-generated height or roughness maps are candidates to validate and refine, not measured physical data. Keep maps spatially registered; derive tangent-space normals from the approved height field with wrap-aware filtering and a documented physical height scale. Do not assume dark albedo means low height, or independently generate unrelated normal/height maps. Use controlled roughness and conservative AO where inferred detail is unreliable; these terrain materials default to metallic zero. Validate under rotating light and grazing views before accepting the set.

Retain generated source images, prompts, available generator/version metadata and processing settings alongside asset provenance. Store final approved outputs so builds never depend on regenerating an identical image. Automated tests continue to use small deterministic fixtures; generated production artwork is reviewed separately.

1. Load the generated palette with base colour, tangent-space normal and packed scalar maps. Record metres per texture repeat, normal convention, height range, provenance and import settings. Define one engine packing convention for all authored maps. Generate correct mips offline; prefer supported BC compression and provide an uncompressed fixture/fallback path. Query texture-format support.
2. Establish the bounded bindless material texture library and shared filtering policy below. Terrain can retain texture-array palettes with common dimensions/formats/mip counts per semantic, referenced through the global descriptors. Keep stable material IDs, immutable published records and fallback textures. Allocate the initial palette at load time; bindless binding does not require a texture streaming system or loading the entire world’s assets.
3. Add world-space XZ mapping and four smooth procedural weights for grass, soil, rock and gravel. Use stable terrain attributes and low-frequency world noise; classify from a defined physical sampling scale rather than a camera-dependent mesh normal. Snow and climate-driven materials follow when their inputs exist.
4. Preserve precision with integer world cells plus local coordinates. Compute repeat phases without converting the full distant world position to float. Use unwrapped derivatives/explicit gradients where wrapping  would break mip selection. Texture phase and weights must survive origin shifts and parent/child terrain transitions.

Acceptance: generated artwork passes repeated-grid/offset seam inspection and a rotating-light PBR review; texture scale unchanged across chunks, negative and distant coordinates; no seam or texture sliding through rebasing/refinement; material changes do not regenerate terrain heights; missing assets have explicit fallbacks; mip/anisotropy review at grazing angles. Test data is owned and independent of production artwork.

#### 5B.1 — Trilinear and anisotropic material filtering

Use **8× anisotropy as the initial normal preset and 16× as the high preset**, subject to target-GPU measurement. Query and enable `samplerAnisotropy`; clamp the requested maximum to `maxSamplerAnisotropy` and expose the effective setting. Unsupported devices retain trilinear filtering with anisotropy disabled. Both presets use `minFilter = magFilter = VK_FILTER_LINEAR` **and** `mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR`, with the full mip chain accessible. `VK_FILTER_LINEAR` alone does not enable interpolation between mip levels. [Vulkan sampler specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkSamplerCreateInfo.html).

Share a repeat-addressing sampler for ordinary terrain/object material reads using this policy. Keep a small, explicit sampler palette for different needs: clamp-addressed material maps, environment/BRDF LUTs, post-process images and shadow comparisons. A single 16× sampler is not suitable for every renderer resource. Its anisotropy value is a maximum, not a promise of identical filtering work on every read. Correct mip contents and derivative-aware sampling remain essential; a sampler cannot repair broken UV gradients or make `Load` anisotropic. Preserve colour-space-aware mips, normal/roughness filtering and alpha-mask coverage where applicable.

Acceptance: compare trilinear-only, 8× and 16× on sloping checkerboards and natural textures, at grazing angles and in motion, with TAA disabled initially. Verify mip transitions, format/feature fallback and sampler selection. Record the worst-case layered/triplanar sampling cost; anisotropy and TAA address different sources of aliasing.

#### 5B.2 — Bounded bindless material texture library

Adopt descriptor indexing for terrain and future object materials. Its Vulkan 1.2 core promotion fits our Vulkan 1.3 renderer, but individual capabilities still need querying/enabling; the API version alone is insufficient. Plan for `runtimeDescriptorArray` and `shaderSampledImageArrayNonUniformIndexing`, with partially-bound, variable-count and update-after-bind features only where the chosen allocation/update policy needs them. [Khronos descriptor-indexing guide](https://docs.vulkan.org/guide/latest/extensions/VK_EXT_descriptor_indexing.html).

Our design is one scene-wide material descriptor set with **bounded, typed sampled-image descriptor arrays** and a small shared sampler palette. An array of descriptors is different from a `Texture2DArray`: independent images can differ in size and format, while layers within one array image share its shape/format. Keep separate compatible bindings for 2D images and terrain array-image palettes; add cube bindings only when needed. Material records store descriptor indices plus any palette-layer index. Reuse image allocations where possible while preserving sRGB views for base colour and linear views for data. This supports global binding without forcing every map into one enormous array image. The [Khronos sample](https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html) demonstrates descriptor-array access and separate samplers.

Choose capacities from queried descriptor/per-stage limits and the actual material budget in the detailed design. Reserve fallback slots and validate typed, generation-checked CPU handles before publishing GPU indices; use HLSL `NonUniformResourceIndex` when indices vary across shader invocations. An unbound slot must never be sampled, even with partially-bound support. Exhaustion rejects/defer-loads an asset with a visible fallback rather than allocating without a bound. Devices lacking the selected features use a bounded fixed-binding palette path with the same material semantics and an explicit capacity limit.

Publish descriptors and material records only after uploads/layout transitions complete. The initial static palette needs no update-after-bind. For later loading, use frame-safe descriptor versions or carefully feature-gated updates; update-after-bind does not make overwriting an in-use texture slot safe. Retire image views, images, descriptor slots and their material references only after every submitted reader has finished. Descriptor indexing changes binding, not texture memory residency or texture streaming policy.

Acceptance: equivalent results against a small fixed-binding reference; non-uniform material selection; missing/stale handles, capacity exhaustion, resource replacement with frames in flight and unsupported-feature fallback. Use validation and verify bounded allocation and descriptor update work. Ordinary material draws must not rebuild or rebind a descriptor set per texture.

### 5C — Blending, cliffs and non-uniform terrain

Status: implemented; [design, controls, captures and verification](rendering_terrain_blending.md). Balanced is the default; weak-axis pruning and a terrain depth prepass remain opt-in. Stochastic sampling has been removed. Target-GPU timing and live flight review remain pending.

1. Add normalized height-aware blending of all material channels, with bounded influence and stable zero-weight handling. Blend/reorient normals in a consistent frame; do not interpolate encoded normal colours. Filter small height features and normal variance to avoid glittering distant surfaces.
2. Add slope-weighted triplanar projection for rock/cliffs, including signs, rotations and projection-frame normal transforms. Start with a correct three-axis reference, then measure axis pruning/two-axis variants. Keep plain terrain on the cheaper mapping path.
3. Add broad albedo/roughness variation and a second material scale. Avoid simply enlarging grass blades into distant giant motifs. Fade micro-normal detail with footprint and retain meaningful macro patterns as fine mips converge.
4. Keep conventional micro sampling and independently filtered macro variation. Stochastic blending was removed after target-user visual/cost review.
5. Profile the maximum combination: layers × projections × scales × texture channels. Define quality variants and counters instead of enabling every technique everywhere.

Acceptance: side-by-side mapping/blending/variation toggles; ground-level and aerial captures across multiple terrain seeds; motion review for shimmer; no hard transitions when layer dominance changes. Measure fragment cost at 1080p and the demo's native resolution.

**Later material scalability gate:** if needed, replace the four-global-layer control scheme with indexed control data, gathering categorical IDs and merging duplicate contributions. Never linearly filter IDs or independently take a top-two list per texel and assume seams are solved. Specify neighbourhood unions, palette transitions, weight filtering and coarse representation first. If material shading then dominates, prototype a bounded material-composition cache with halos, mip fallback and independent appearance versions. Neither optimization is assumed necessary now.

### 5D — Clustered forward lighting and transparency

Status: implemented with Debug/Release and software Vulkan verification; target-GPU/live review pending. See [detailed design and measurements](rendering_clustered_lighting.md). Use a reusable indexed mesh API with generated fixtures, preserve a future glTF import boundary, remove the material-preview scene, and add an opt-in terrain lighting demo.

Use a regular screen XY grid with logarithmic view-depth slices. Keep the sun outside local-light lists. Begin with point and spot lights stored contiguously; assign conservative light bounds to clusters on the GPU. No hardware ray tracing is required. Clustered forward is a good fit for sharing lighting across opaque and transparent surfaces. [Original clustered forward research](https://research.chalmers.se/publication/161726).

1. Add typed light handles and fixed-capacity per-frame GPU light records; implement a brute-force small-light reference.
2. Add cluster assignment, bounded index storage, occupancy/overflow diagnostics and GPU timing. Start correctness fixtures small; performance sweeps cover hundreds to thousands of lights with both sparse and highly overlapping distributions. Light count alone is not a performance guarantee.
3. Use full-depth conservative cluster lists for transparency. An opaque-depth-only occupied-cluster optimization must not discard lights needed by transparent surfaces or fog. On list overflow, fall back to scanning the bounded active-light buffer for affected clusters; avoid silently dropping lights. Tune capacities from measurements.
4. Add minimal ordinary mesh/test-instance submission and explicit opaque, alpha-mask and alpha-blend modes. Alpha-blend uses premultiplied linear-HDR colour, depth testing without depth writes, and back-to-front sorting initially. Alpha masking uses a consistent cutoff in colour, depth and shadow passes. Define double-sided normal handling.

Acceptance: clustered results agree with brute-force lighting within tolerance, including slice boundaries, lights crossing the near plane, camera-inside-light and overflow. Demonstrate transparent surfaces in front of/behind opaque terrain and several overlapping layers. Sorted transparency has known intersecting-surface limitations; weighted blended OIT is a later option for particles, not a claim of exact glass/refraction. [Weighted blended OIT paper](https://www.jcgt.org/published/0002/02/09/).

### 5E — GPU-driven drawing and conservative Hi-Z culling

Status: current-frame baseline implemented; [design, controls and verification](rendering_gpu_visibility.md). Indirect drawing defaults on supported devices; Hi-Z remains opt-in. The optional two-phase optimization below is deferred. Target-GPU/live review remains pending.

Adopt GPU visibility and indirect draws. Hardware queries are not inherently unusable, but a per-object CPU readback path is unnecessary here. Hi-Z still has GPU dependencies and can cost more than it saves in an open plain; retain a measured off switch.

1. **Indirect foundation.** Upload a bounded flat candidate buffer containing bounds, geometry range, material and terrain sampling/morph data. Keep current CPU residency/LOD selection initially; GPU culling refines visibility rather than rewriting streaming. Move per-draw push-constant data into indexed GPU records so candidates can be compacted without losing their source identity. Group by compatible pipeline/index-buffer state. Generate `VkDrawIndexedIndirectCommand` records and use `vkCmdDrawIndexedIndirectCount` when enabled, or fixed-capacity indirect records with zero instance counts. Query features/limits and cap every counter. [Khronos GPU-rendering sample](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html).
2. **Current-frame baseline.** Extend the optional terrain depth prepass from 5C into a retained, sampleable opaque depth pass, keeping the same terrain morph, clipping and eventual alpha-mask semantics as colour. Store/sample depth, build Hi-Z, then cull the expensive colour draws. Initially this saves shading and colour-pass geometry, not the depth-pass geometry. A selected-occluder version may improve that later, but proxies must never cover empty space or sit in front of actual occluders.
3. **Conservative test.** Our reversed Z needs a **minimum reduction** to retain the furthest depth; clear/sky depth 0 must prevent rejection. Cull only if the nearest possible object depth is strictly behind the conservative depth over its entire projected bounds, with a safety tolerance. Cover all overlapped mip texels; handle non-power-of-two edges, near-plane intersections and camera-inside-bounds conservatively. Unknown bounds render. Bounds include morph/refinement ranges, not just the last coarse surface.
4. **Optional two-phase optimization.** Render previously visible candidates with current transforms/depth, build current Hi-Z, test previously hidden/new candidates and draw newly visible ones in the same frame. Use history to select work, never as proof of current occlusion. Handle removed/moved occluders, rebasing, resize, camera cuts and new terrain without a delayed reveal. A pyramid from last frame alone is not sufficient for conservative visibility.

Insert explicit depth-write → compute-read, pyramid-level, compute-write → indirect-read and shader-record-read barriers. No CPU visibility readback on the frame path. Visibility must not retire terrain needed for shadows or other consumers. Transparent surfaces can be tested conservatively against opaque depth, but do not become solid occluders.

Acceptance: compare culled and unculled images, including thin gaps, screen edges, odd resolutions, hidden-to-visible motion, fast turns, cuts, newly streamed patches and buffer exhaustion. Render uncertain candidates rather than lose geometry. Measure depth/pyramid/culling/colour costs separately in valley and open-plain flights. Depth-prepass and colour-pass equality must tolerate the existing terrain transition rules.

### 5E.5 — Ground Truth Ambient Occlusion (GTAO)

Status: **implemented**, with Off by default and Half/Full opt-in modes,
spatial denoising, depth-aware reconstruction and indirect-diffuse-only integration.
See [GTAO design and verification](rendering_gtao.md) for Debug/Release and software
Vulkan evidence; target-GPU 1080p/native profiling and live motion review remain pending.
The requirements below record the slice's intended contract; temporal sample integration is implemented in 5G.

Add GTAO after the current-frame depth foundation in 5E, before forward opaque lighting. It estimates horizon-based visibility from visible depth; it cannot recover hidden/offscreen geometry and is not actual ground truth for the full scene. Clustered forward can consume its AO texture without adopting a full deferred G-buffer. Use Intel's MIT-licensed XeGTAO as an implementation reference. It has **depth preparation, horizon evaluation and edge-aware denoising passes**, rather than one universally fast dispatch. Its spatial path works without TAA, with temporal sampling available when TAA is present. [XeGTAO implementation and limitations](https://github.com/GameTechDev/XeGTAO).

1. **Inputs and depth contract.** Preserve/sample the current opaque/alpha-mask prepass depth, using the same terrain morph and clipping as colour. Start with discontinuity-aware geometric normals reconstructed from depth; evaluate a compact prepass normal attachment if quality requires it. Do not require texture micro-normals or a full material G-buffer. Build the AO algorithm's view-space filtered depth mips: our conservative reversed-Z **min-reduction Hi-Z** is for culling and must not be substituted for those mips. They can share the source depth and scheduling, not an assumed identical reduction.
2. **Horizon evaluation and reconstruction.** Use a world-space effect radius with bounded projected search, sample count and thickness assumptions. Begin with a full-resolution spatial reference; profile a half-resolution variant with depth-aware upsampling and denoising. Allocate intermediate AO/edge/depth resources at startup/resize/quality changes, insert explicit depth/compute/fragment barriers, and measure preparation, evaluation and filtering separately. No CPU readback on the frame path.
3. **Lighting integration and range.** Sample AO during indirect diffuse lighting, before adding direct sunlight/local lights and emissive. Never multiply the completed lit HDR image by AO. Define how it combines with authored material AO by spatial scale to avoid double-darkening the same crevice. Keep diffuse AO separate from any later specular-occlusion model. Treat sky/background and uncertain screen boundaries conservatively. Add an explicit smooth distance/projected-footprint fade towards unoccluded visibility; neither seamless distance scaling nor a GI handoff comes automatically from GTAO.
4. **TAA and later GI integration.** Ship spatially denoised GTAO first; integrate bounded temporal sample variation with 5G.1 and validate trails/disocclusion. Add a separate AO history only if it produces a measured benefit. Before 5K, distant surfaces retain environment irradiance, not an imaginary macro-GI solution. With GI, define non-overlapping occlusion scales and verify the transition so GTAO supplies local contact detail without reapplying visibility already represented by the GI backend. Transparent surfaces do not inherit opaque-background AO as their own occlusion.

Acceptance: open plane remains unoccluded; corners/crevices agree reasonably with a small offline ray-sampled fixture under documented depth assumptions. Test thin ridges, silhouette halos, missing offscreen occluders, sky, view-distance changes, camera cuts/rebasing, odd dimensions, terrain refinement and half-resolution edges. Review with AO/TAA individually disabled. Record AO buffers and separate timings at 1080p/native resolution within the existing frame budget.

### 5F — Stable cascaded sun shadows

Implemented: four stable near cascades plus one coarse mountain cascade.
The virtual implementation failed native motion/quality and performance review and
has been removed completely, including its dense clipmap fallback. Debug/Release
and software Vulkan checks pass; native CSM acceptance remains pending. See the
[CSM design and acceptance gates](rendering_shadows.md).

Use texel-snapped fixed-extent projections, overlapping filtered cascades, shared
rendered terrain geometry and bounded independent offscreen caster residency.
CPU culling and ordinary depth passes replace all page requests/allocation/caching.
Shadows refresh every frame and do not require camera depth. Retain the 2 ms p95
incremental 1080p target and native RTX 3060 motion review before acceptance.

### 5G — Temporal antialiasing and HDR post-processing

Status: 5G.1–5G.2 implemented; [design, controls and verification](rendering_temporal_post.md). The demo enables TAA and subtle bloom at its configured scene resolution. Auto-exposure (5G.3) remains deferred; target-GPU/live motion acceptance remains pending.

**TAA is the primary antialiasing method. Bloom has independent off/intensity controls.** Keep these independently reviewable; neither replaces correct texture filtering or fixes incorrect visibility/shadow data. TAA depends on the depth/motion and material contracts; its integration with cascaded shadows is covered by the combined fixtures.

#### 5G.1 — Native-resolution TAA

1. Add bounded subpixel projection jitter, current/previous transforms and motion vectors, with explicit jitter-inclusion and pixel/UV conventions. Use matching jitter in the depth prepass and colour pass, and conservative frustum/Hi-Z bounds. Track previous camera origins and actual terrain morph/residency states; camera motion alone does not describe changing terrain vertices. Newly published surfaces and replaced topology must invalidate history or provide valid previous positions.
2. Resolve pre-exposed HDR at native resolution with reprojection, depth/disocclusion rejection, neighbourhood/variance clipping and a bounded history weight. Rescale previous colour by `currentPreExposure / previousPreExposure` before comparison/blending. Reset on cuts, teleports, resize and incompatible settings; use frame-safe ping-pong resources. Auto-exposure and temporal upscaling are not prerequisites. [Intel temporal-AA reference (archived)](https://github.com/GameTechDev/TAA). Adapt its algorithm to our pre-exposed HDR history contract rather than inheriting its default tone-mapped history mode.
3. Establish transparent/emissive reactivity or reduced-history handling and useful motion for moving opaque/alpha-mask objects. A single opaque velocity cannot exactly reproject all overlapping transparent layers; document/test this limitation. Keep UI outside jitter/history and retain a TAA-off comparison. Add modest sharpening only after measuring blur/ringing.
4. Integrate temporally varied GTAO samples and later sky/fog histories without letting histories hide systematic errors.

Acceptance: static convergence versus a supersampled owned fixture; fine terrain textures, slow pans/fast turns, thin ridges and later foliage, moving/transparent objects, disocclusion, changing exposure, world rebasing and terrain replacement. Track ghost trails, loss of detail, shimmer, history memory and resolve cost. TAA complements 8×/16× anisotropy; it does not remove the need for correct mips and normal/specular antialiasing.

#### 5G.2 — HDR bloom

Add a bounded multiresolution bloom chain from resolved pre-exposed HDR: filtered downsample, low-resolution reconstruction/upsample and controlled composition, followed by ACES/AgX. Include opaque/transparent emissive and the later sky/atmosphere contribution; exclude UI. Start with a small fixed maximum level count and configurable intensity/radius, with half-resolution entry and correct handling of odd/tiny extents. Avoid an expensive full-resolution large-radius blur. HDR bloom is also part of [Filament's post-processing baseline](https://github.com/google/filament).

Specify threshold behaviour explicitly: a soft-knee artistic threshold is an initial option, with threshold expressed in scene-referred units and converted by current pre-exposure. A threshold-free optical-style mode can be evaluated separately. Prevent a single extreme texel from producing unstable fireflies through robust downsampling, without silently clipping legitimate HDR highlights. Do not apply exposure twice, leak UI into bloom, or sample LDR/tone-mapped colour for extraction. Allocate/reuse images at resize/quality changes and retain a zero-intensity identity path. Bloom can be implemented and reviewed without TAA; in the integrated frame it consumes the TAA resolve.

Acceptance: deterministic impulse/emissive ramps, threshold/knee continuity, zero-intensity identity, screen edges and odd dimensions, bright moving subpixel sources, exposure changes, no UI bloom and no stale data after resize. Compare radius/intensity and downsample quality at 1080p/native resolution and report all mip-chain memory and per-pass cost.

#### 5G.3 — Exposure refinement

Optional auto-exposure follows the tested manual-exposure baseline, with bounded adaptation and reset rules. Keep fixed exposure for material inspection and ensure changes rescale TAA history and bloom thresholds consistently. Auto-exposure remains independently switchable.

### 5H — Physical sky, aerial perspective and distant coverage

**Scattering versus froxels.** Rayleigh scattering describes molecular scattering; aerosols add Mie-like scattering and absorption. Extinction removes surface light and in-scattering adds atmospheric light, reducing contrast and changing colour with distance. This is not simply a saturation slider. A froxel is a frustum volume cell used to discretize transport. Aerial perspective can itself use a coarse froxel-like volume; local fog often uses a finer one. They overlap in the transport calculation, while differing in scale and density fields.

Hillaire's paper is an appropriate later target, not unnecessarily exotic: it factors work into transmittance, multiple-scattering, sky-view and aerial-perspective lookups. Use its scalable architecture; do not port the entire demo/application. The accompanying code is a D3D11 reference, so adapt algorithms and preserve applicable attribution/licensing. [Paper](https://sebh.github.io/publications/egsr2020.pdf), [author's code](https://github.com/sebh/UnrealEngineSkyAtmosphere).

1. **Sky baseline.** Add a camera-centred skydome as requested, rendered only behind geometry without depth writes. The dome is a carrier for directional sky radiance, not the atmosphere model or a terrain horizon. A fullscreen background pass remains an equivalent optimization. Start with a simple analytic sky, then replace radiance evaluation with LUTs.
2. **Physical atmosphere.** Add Rayleigh, aerosol/Mie and absorption profiles, sun transmittance, sky-view and multiple-scattering LUTs. Parameter changes invalidate only affected products. Feed the same sun/sky state into surface lighting and budgeted environment-map/irradiance updates. For an infinite flat world, define a local virtual-planet atmosphere with altitude relative to a shared datum; horizontal travel must not accidentally wrap the terrain onto a planet.
3. **Aerial perspective.** Store/evaluate RGB transmittance and in-scattered radiance along camera rays. Composite opaque surfaces using their depth; evaluate transparent surfaces at their own depth before premultiplied blending. Ensure the backdrop is not fogged twice. Validate interpolation at the horizon and near mountains.
4. **View-boundary policy.** Clear-air physical scattering alone does **not** guarantee hiding an 8–16 km cutoff, particularly from high altitude or in dry weather. Keep coarse terrain coverage beyond the detailed range and evaluate a bounded low-detail horizon extension. Optional distance/height haze can reduce contrast before the outer coverage boundary, using atmospheric colours. Exponential extinction approaches zero transmission asymptotically, so it does not mathematically erase a geometry edge. Test looking down from altitude and looking toward/away from the sun. Define a maximum supported view/altitude envelope or extend coverage rather than promise invisible boundaries in every clear-air view.

Do not add independent full-strength "distance fog", physical atmosphere and volumetric fog over the same air. Use one composition contract: `L_out = T * L_surface + L_scatter`. Consecutive non-overlapping segments compose in depth order; overlapping media require combined extinction and scattering sources during integration, or a documented approximation tested against that reference.

Acceptance: noon/sunset, low/high altitude, clear/hazy air, changing sun, camera rebase, sky/terrain horizon continuity, transparent surfaces and view-radius changes. Compare LUT output with a slow small ray-marched reference rather than judging screenshots alone.

### 5I — Local froxel volumetrics

Add a separately budgeted near/mid-range frustum grid for height fog, local density volumes and single-scattered lighting. Inject sun and selected clustered local lights, sample sun shadows, integrate front-to-back and reconstruct onto surfaces/sky. Reuse light data, but do not assume the surface cluster grid has the right volume resolution. Local lights remain unshadowed unless later explicitly budgeted.

Combine the local medium with the atmospheric background exactly once over shared segments, and retain atmospheric transport beyond the local volume. Add temporal reprojection only with camera-cut rejection and controls for moving-light trails; fast effects may need reduced history. Volume sampling and integration depth have their own quality settings. [Epic volumetric fog reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine).

Acceptance: analytic homogeneous-fog transmittance, zero-density identity, local fog boundaries, lights in empty screen regions, transparent objects at multiple depths, camera motion and history resets. Check light shafts against shadow coverage and cap volume/light work independently of terrain generation.

### 5J — Representative scene content and reflections

Split this into independent demos rather than one content milestone:

- **Vegetation:** bounded streamed instances, alpha-mask materials, shared GPU visibility, wind-aware bounds, and explicit shadow deformation distance/quality. Measure caster cost with actual wind; shadow deformation must match the visible geometry.
- **GPU particles:** fixed pool, bounded spawn/retire work, indirect rendering and transparent lighting/fog. Evaluate weighted OIT only where sorting fails visibly or costs too much.
- **Reflection probes:** begin with bounded static/local probes and correct roughness filtering, then budget updates; always retain sky/environment fallback.
- **Screen-space reflections:** later depth/normal/roughness and history consumers. Define a compact auxiliary surface buffer if needed; clustered forward does not require a full deferred G-buffer. SSR cannot recover offscreen/hidden content, so it supplements probes.

Water/refraction, cloud rendering, coloured transparent shadows and complex subsurface materials need their own designs when requested. A sky atmosphere does not automatically provide volumetric clouds.

### 5K — GI decision gate and implementation slices

Do not select SDFGI solely because it avoids RTX, or reject DDGI solely because NVIDIA supplies an SDK. SDF describes a scene/intersection representation; DDGI describes probe-based indirect-light estimation. These dimensions can be combined, but that still requires building and maintaining a usable tracing scene.

| Candidate | Engine implications | Evaluation |
| --- | --- | --- |
| Sky irradiance/probes plus GTAO | Cheap baseline, no local bounce solution | Keep as fallback; AO must not multiply all direct sunlight |
| SDF/voxel clipmap GI | No hardware RT requirement; costs include scene voxelization/distance updates, leaking/thin geometry and streaming | Evaluate if non-RT hardware is a priority; terrain height alone is not a valid general 3D signed distance field |
| DDGI with hardware ray queries/tracing | Probe relocation/visibility/history and bounded ray work; also BLAS/TLAS and scene-material access | Strong candidate for an RT-capable tier, with a portable fallback |
| DDGI with software tracing | Could trace heightfield/SDF/proxies, with different quality and update costs | Prototype only if it solves a concrete baseline requirement |

Godot's SDFGI supports dynamic lights but has restrictions on dynamic occluders/emissive geometry and can be expensive. Those are properties of that implementation, not an impossibility theorem for SDF-based GI. [Godot SDFGI documentation](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_sdfgi.html). RTXGI's documented integration expects GPU ray tracing through DXR/Vulkan; this is not limited to NVIDIA-branded RTX GPUs when the required API features are supported. [RTXGI integration](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Integration.md).

Implement in three decisions: (1) owned outdoor/indoor probe fixtures and a bounded indirect-light sampling interface; (2) compare one software representation and one hardware-RT prototype on supported devices, including world movement and updates; (3) select and productionize one backend with scrolling volumes, newly exposed region fallback, invalidation, temporal convergence and resource retirement. Do not build both full backends in advance. Include thin walls, moving objects/lights, changed terrain, teleports and light leaking in acceptance. Diffuse GI does not replace specular reflections.

## Renderer boundaries, ownership and frame order

Proposed components are concrete responsibilities, not a general render-graph or scene-framework project:

| Responsibility | Owned state and interface |
| --- | --- |
| Renderer/frame coordinator | Frame resources, pass order, barriers, presentation and deferred retirement |
| Material library | Typed generation-checked handles, immutable published records, bounded global texture descriptors, shared sampler palette and fallback assets |
| Terrain surface evaluation | Material weights/coordinates from stable world attributes; reads terrain data without owning generation |
| Scene draw submission | Bounded mesh/terrain draw records and conservative bounds; explicit source IDs after GPU compaction |
| Lighting | Sun/environment state, contiguous local lights, cluster lists and overflow policy |
| Visibility | Conservative Hi-Z and bounded indirect command buffers; no authority over world residency |
| Ambient occlusion | Separate filtered AO depth, geometric normals, GTAO/denoise resources and indirect-light visibility |
| Sun shadows | Stable cascades, shared terrain geometry and bounded offscreen caster demand |
| Atmosphere/volumetrics | LUTs, volume/history resources and transport composition |
| Temporal/output stage | Motion/history, HDR TAA resolve, bloom mip chain, exposure metadata, display transform and UI composition |

At the 5I stage, the conceptual sequence is:

```text
publish completed terrain/assets; upload bounded scene/light records
  -> cluster lists; cascaded sun-shadow rendering (independent of camera depth)
  -> current-frame jittered opaque/alpha-mask depth (or conservative two-phase visibility)
  -> conservative Hi-Z + visible indirect draws
  -> AO-specific depth preparation + GTAO + edge-aware denoise
  -> atmosphere LUT updates + volumetric lighting/integration
  -> pre-exposed opaque HDR lighting (GTAO on indirect diffuse), motion/reactivity
  -> sky + opaque aerial perspective
  -> transparent HDR lighting/reactivity, each surface sampling transport at its depth
  -> HDR TAA resolve (motion, depth, reactivity and exposure-aware history)
  -> HDR bloom downsample/upsample + composition
  -> display transform -> display-linear UI -> sRGB presentation
```

This describes dependencies, not mandatory queue splits. Use the current graphics queue initially; consider async compute only after measurement demonstrates overlap and accounts for terrain generation competing for compute time. GTAO uses current depth and completes before opaque indirect lighting; TAA and bloom consume the lit scene. The two-phase visibility optimization must provide complete current depth for AO, even when some colour work was previously fused with its first phase. Later reflections will refine this sequence in their own design. Set pre-exposure before lighting; the final display transform must not apply it a second time.

Allocate frame buffers, lists and pools at startup/resize or explicit quality changes. Record capacity and exhaustion behaviour for each. Retire images/descriptors/slots only after all submitted readers finish. GPU command generation must obey Vulkan indirect limits and preserve vertex/descriptor lifetimes; CPU arrays must reserve their maximum routine size. History resources need deliberate ownership across frames in flight. No new `waitIdle` or GPU-to-CPU visibility-decision waits in ordinary rendering.

## Validation and performance gates

Every implementation slice updates its detailed design and reports executed versus unavailable checks. This roadmap is not a benchmark result.

- Run focused CPU/GPU correctness tests with owned tiny fixtures and synchronization validation; retain terrain seam, LOD, residency and large-coordinate tests. Test capacity exhaustion and stale handles alongside ordinary use.
- Keep deterministic offscreen colour/depth/material/shadow views; compare against simple reference paths with justified tolerances. Validate both streamed and fixed diagnostic terrain paths.
- For appearance, review fixed-exposure ground/aerial views and motion at negative/distant coordinates, through streaming transitions, sunrise/sunset and changing view radius. Include trilinear/8×/16× filtering comparisons, GTAO/TAA/bloom debug and off modes, AO halos/fades, TAA trails and bright subpixel bloom stability.
- For performance, record per-pass GPU timestamps, CPU frame time and high percentiles, allocation counts, total renderer/terrain memory, and overflow/fallback counters. Use warm/cold flights, stationary/rotating/moving camera, heavily overlapping lights, moving sun and later vegetation wind.
- Retain the existing RTX 3060 **120 FPS / 8.33 ms** performance objective. Measure at 1080p and the demo's native resolution, with exact settings recorded; do not silently lower the objective as effects are added. Derive per-feature budgets from the 5A baseline and retain scalable quality settings. If the full feature set cannot meet the objective, present the measured quality/frame-time tradeoff before changing the target. Terrain generation must retain explicit headroom.
- Gate optional indexed controls, material caching, advanced soft shadows, async compute, OIT and GI on demonstrated quality/performance needs. Software Vulkan remains valuable for correctness, but cannot sign off target-GPU frame time.

Before each increment, write the focused implementation design with exact capacities, layouts, feature requirements and fallbacks. The [implemented 5A design](rendering_hdr_pbr.md) records the first foundation; keep the remaining slices as an ordered roadmap until their dependencies exist.

# Slice 5B: terrain material assets and mapping

Status: implemented; software Vulkan verification below. Target-GPU performance and live flight review remain pending. See the [roadmap](rendering_roadmap.md).

The current renderer extends these 5B contracts with [slice 5C](rendering_terrain_blending.md): a fourth optional macro array, 3D projection coordinates, height-aware composition and spatial repetition control. The descriptions below record the 5B baseline; the 5C design records the current combined sampling costs and interfaces.

## Responsibilities and contracts

An offline C++ importer reads PNG artwork and a JSON recipe, derives wrap-aware tangent normals from authored height in metres, builds linear-light colour mips and normal/roughness-aware mips, and writes bounded DDS DX10 2D-array palettes. Runtime reads DDS only; builds never generate artwork. Colour uses BC7 sRGB, normals BC5 UNORM (positive Z reconstructed), packed scalars BC7 UNORM: roughness/AO/height/one. RGBA8 alternatives have identical semantics. Four 2048-square layers represent grass, soil, rock and gravel, each repeating over 4 metres. Sources, built-in imagegen prompts and processing settings remain under assets/terrain. Scalar maps are authored estimates, not measured scans. Metallic is zero.

The renderer owns a Vulkan material library with set 2, separate typed 2D/array descriptor bindings, shared repeat samplers and 64 immutable material records. Requested descriptor capacities are 128 2D and 16 array views, bounded against device limits with reserved capacity for existing sets. Missing indexing support selects a three-array fixed-binding shader. Every descriptor has a valid fallback; no partially-bound or update-after-bind features are required. CPU handles validate generation before publishing GPU indices. Exhaustion rejects replacement and retains the published palette.

Image allocations, including pending retirement, are limited to 320 MiB by default. Uploads are recorded before sampling with transfer-to-fragment barriers. Two frame snapshots own published resources; descriptor/record updates occur only after the corresponding frame fence. Old images and staging allocations remain alive until all readers finish. There are no per-draw descriptor changes or ordinary frame allocations. Loading/reloading is an explicit operation, not texture streaming.

[Slice 5D](rendering_clustered_lighting.md) extends this library with mesh texture/material
publication: 112-byte records, fixed-path 2D descriptors, shared budgets and frame-safe retirement.
Terrain IDs and packing remain unchanged. The material probe now owns its plane fixture shaders;
there is no interactive material-preview scene. Binding descriptions below record the 5B baseline.

## Coverage continuity correction

The original classifier differentiated the base generator's hard-clamped octave feedback. Heights are continuous at feedback saturation, but their analytic gradient is not: seed 1 near world X=7.25 m, Z=5468.5 m changes rock coverage from zero to one within 0.25 m. Smoothstep on the final slope cannot smooth a discontinuity in its input. This is a 5B classification defect, independent of the height blending planned for 5C.

The correction evaluates a material-only version of the same four-octave landform. Feedback remains linear up to 0.8, transitions with a quadratic shoulder to saturation at 1.2, and stays at one thereafter. Both the feedback value and its first derivative are continuous; the maximum deviation from the original clamp is 0.05. The classifier differentiates this smooth proxy throughout the octave chain. Generation retains the original clamp, heights, normals and cache keys. This retains all landform octaves and the same nine noise evaluations per fragment, with a small amount of extra scalar arithmetic and no new allocations, buffers or texture reads. Material classification still describes base landforms rather than each eroded crease.

Validation uses owned palettes and close, top-down rendered coverage views across the reproduced mountain boundary, including camera rebasing and both descriptor paths. Adjacent-pixel coverage must stay continuous at sub-metre scale; a coarse regional render alone cannot establish this. Existing geography tests protect generation, and material tests run with Vulkan synchronization validation. Target-GPU timing and review of the user's exact camera remain separate checks.

## Sampling and placement

Trilinear, 8x (default) and 16x samplers use linear min/mag and linear mip interpolation; anisotropy is feature-gated and clamped to the queried limit. Environment and LUT samplers remain separate.

Both terrain paths evaluate the same material shader. Texture phases use integer chunk origins and local camera-relative positions, never a distant float world position; explicit unwrapped derivatives retain correct mip choice. Terrain classification uses the analytic gradient of the smooth material landform proxy at stable world positions rather than interpolated mesh normals. It does not evaluate erosion. Weights smoothly blend grass/soil/gravel/rock; seeded low-frequency variation breaks up coverage. The reference classifier costs eight base-landform plus one 64 m appearance noise evaluation per shaded fragment and must be profiled against constant shading. Render LOD changes do not change the classifier. Terrain settings and classification move together when generation settings are changed.

Decoded tangent normals are blended and mapped into a Y-up surface basis with U along +X and V along +Z; neutral normals preserve the geometric normal. Material edits do not touch height cache identities. Height-aware blending, signed cliff projection and macro variation are implemented in [5C](rendering_terrain_blending.md). The stochastic experiment was removed after visual/cost review. XZ stretching on cliffs is a known 5B limitation.

## Inspection and validation

A separate collapsible Terrain Materials window controls comparison, isolation, weights, scale, tint, roughness, normal strength and effective filtering. Preview planes supplement the existing PBR swatches.

Owned tiny fixtures cover importer colour/mip/normal semantics, malformed DDS rejection, fixed/bindless equivalence, fallback/limits, frame-safe replacement, negative/distant world phases, rebasing and material/height independence. Production artwork is separately reviewed in repeated grids and under changing illumination. Run existing CPU/GPU regressions and synchronization validation. Software Vulkan is correctness evidence only; RTX 3060 timing and live flight remain pending when hardware is unavailable.


## Interfaces and ownership details

- `RendererOptions.materialDirectory` selects the imported palette; an empty path skips asset loading. The demo supplies its configured asset path. `--material-dir`, `--fixed-materials`, `--rgba-materials` and `--no-material-assets` provide explicit overrides.
- `SceneSettings.terrainMaterial` supplies draw-only controls. `Renderer::materialStats()` reports binding mode, descriptor capacities, effective AF, image allocation bytes and errors. `reloadMaterials(path)` performs explicit synchronous CPU I/O/staging allocation; GPU transfers happen in the next submitted frame. It retains the previous palette on failure. This is not asynchronous asset streaming.
- `VulkanMaterials` owns images/views, upload buffers, samplers, two mapped uniform/record buffers and two descriptor sets. Destruction requires completed GPU work; `Renderer` provides that shutdown boundary. A maximum of four weak version entries bounds bookkeeping; only current and in-flight snapshots retain images. No descriptor slot is overwritten while its frame is in flight.
- Set 2 bindings: 0 uniform, 1 material records, 2 bounded 2D descriptors (bindless only), 3 array descriptors; fixed mode uses bindings 3/4/5 for colour/normal/scalar, and both modes use binding 6 for the chosen sampler. There are no per-texture draw bindings. The 2D binding is initialized and reserved for future ordinary mesh materials; 5B's production shader consumes array palettes.
- Metadata records reserve 64 entries; material IDs 1–4 refer to grass/soil/rock/gravel, zero is the fallback material; texture slots 0/1/2 reserve colour/normal/scalar fallbacks. Texture handles and material handles validate slot generations on publication. Current palette replacement keeps stable material IDs through frame snapshots; this slice exposes palette loading, not arbitrary scene asset streaming.
- The image budget counts actual driver allocation sizes, including fallback and retained old palettes. Palette payload is about 64 MiB compressed or 256 MiB RGBA8. Offline sources and both runtime variants occupy about 352 MiB on disk. Explicit loading temporarily holds decoded file bytes and staging buffers on the CPU; the image budget is not a total process-memory budget.
- Coverage uses the smooth analytic material-proxy slope: rock blends from tan(25°)≈0.47 to tan(45°)=1, gravel grows over slopes 0.2–0.7, and 64 m seeded patches vary soil/grass. All weights sum to one. This classifies broad landforms, not every eroded micro-crease. Isolating a material bypasses classification; zero-weight layers skip texture fetches. Maximum cost is 12 filtered texture reads plus nine analytic noise evaluations per fragment.
- Continuous material noise extends the existing canonical integer-domain evaluation with fractional quarter-metre residuals; compute generation keeps its original sampling rules. Texture phase reduces the integer origin before adding local camera coordinates. Streaming epoch changes hide obsolete terrain; fixed requests generate their new geometry before colour drawing, so classification follows the visible terrain settings.

## Authoring and review

See [source provenance](../assets/terrain/README.md), [exact prompts](../assets/terrain/source/prompts.json) and [import recipe](../assets/terrain/source/palette.json). The built-in imagegen tool returned 1254-square sources; these were resampled to 2048-square runtime palettes. No model version or reproducible seed was exposed. Height edits are approximate and use deliberately shallow physical ranges; rock was reduced to 2 cm at import. Roughness is conservative per-material data, AO is one, and height is retained for 5C.

The importer accepts square PNG sources and generates wrap-aware bilinear resampling, linear-light box mips, normalized normal encoding and roughness widened from the accumulated normal mean's variance. Physical footprint is fixed at 4 m in this first palette contract; draw-time repeat changes compensate normal slope by 4/repeat. The DDS reader accepts only supported DX10 2D/array layouts with complete mip chains, checked dimensions, formats and exact payload sizes.

Reviewed [repeated tiles](images/materials/repeated_palette.png), [half-offset seams](images/materials/offset_palette.png), [PBR panels](images/materials/palette.png), [changed light direction](images/materials/palette_rotated_light.png), [grazing rock](images/materials/rock_grazing.png) and [smooth terrain coverage](images/materials/terrain.png). No large seam cross was apparent in these captures. Repeating motifs remain visible on broad isolated surfaces; imagegen does not establish mathematical seam continuity or physically measured material properties. Height blending, triplanar cliffs and repetition control remain 5C.

## Reproduction and performance

```sh
cmake --build cmake-build-release --target material_import material_probe
cmake-build-release/material_import assets/terrain/source/palette.json assets/terrain/runtime
cmake-build-release/material_probe assets/terrain/runtime /tmp/material-review 1920 1080
```

The probe captures flat/sloped surfaces, rotating light and terrain weights, then records median scene/output GPU timestamps for constant, trilinear, 8× and 16× modes. The first software Vulkan run at 1280×720 measured scene medians of 21.55 / 199.38 / 569.95 / 573.14 ms respectively. These are **llvmpipe software costs**, not RTX 3060 performance estimates. Production palette image allocation was 67,112,256 bytes including fallback. No hardware GPU/display is available here; 1080p/native RTX timings and motion/shimmer review remain unmeasured. Keep the constant/trilinear comparison switches when evaluating performance.

## Verification results

Material CPU/import/GPU checks pass on llvmpipe with synchronization validation. Owned fixtures exercise sRGB mips, normal orientation and periodic derivatives, scalar packing, DDS truncation/oversized headers/incomplete mips, handle recycling and exhaustion, fixed/bindless output equivalence, failed and in-flight replacement, retirement, budget exhaustion, anisotropy fallback/clamping, negative/distant phases, classifier rebasing, and fixed/streamed sampling agreement. Existing lighting checks pass after adding textured preview panels. Production artwork is never required by these fixtures; the smoke CLI explicitly skips material assets.

The initial parallel Debug run timed out the hierarchy GPU test under concurrent work; its isolated rerun passed in 62.68 s. Full Release regression: **19/19 passed**, 137.22 s total, with software Vulkan and synchronization validation. Debug checks passed across the initial run and focused/isolated reruns.

Final focused reruns after DDS header/fallback hardening: 3/3 material checks passed in both Release and Debug. Full production BC/bindless and RGBA/fixed preview captures each completed with zero validation errors.


Coverage correction regression: the owned seed-1 mountain fixture failed before the fix (maximum adjacent HDR coverage-colour step **0.681763**) and passes after it (**0.006836**) in both bindless and fixed-binding modes. See the same central terrain footprint [before](images/materials/coverage_before.png) and [after](images/materials/coverage_after.png); these are diagnostic coverage colours, enlarged with nearest filtering, not production textures. Non-flat camera rebasing also passes. A separate before/after GPU comparison of 99,846 geography samples across three seeds and positive/negative regions produced byte-identical heights, gradients and erosion outputs on llvmpipe. No generation/cache migration is required.

For this correction, `material_gpu`, `terrain_geography_gpu` and `terrain_gpu` passed in both Debug (18.61 s total) and Release (14.68 s total), with Vulkan validation enabled and zero reported errors. Both demo builds and all shader variants compile. These are software Vulkan checks; the exact reported camera view and RTX 3060 performance still need live review.

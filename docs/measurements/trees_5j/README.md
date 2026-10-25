# Tree integration verification

The implementation includes four species, six forms per species, offline meshes
and octahedral captures, bounded streamed placement and attachment, camera/cascade
GPU instancing, three geometric LODs and depth-reconstructed impostors, wind,
shared lighting/CSM/TAA, and a canopy influence on existing vegetation.

This environment exposes llvmpipe (LLVM 23.1.3), not a usable NVIDIA driver.
`nvidia-smi` could not communicate with the NVIDIA driver. Software Vulkan is used
for correctness and captures. It cannot establish the 1080p RTX 3060 target,
incremental GPU budget, representative frame spikes or extended 8 km performance.

Select software Vulkan here with:

```sh
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
```

## Checks

The CPU, asset-build and GPU tests own small generated recipes/textures and temporary
files. They do not read the production tree artwork. Vulkan fixtures request the
validation layer; CTest rejects validation messages as well as assertion failures.

- `trees_cpu`: geometry determinism, geometric LOD reduction, metadata/capture
  bounds and index rejection, binary round trip/truncation, full-sphere octahedral
  mapping, extreme signed coordinates, settings validation and fence retirement.
- `tree_asset_build`: stable no-op rebuild, source changes, missing DDS recovery,
  deterministic geometry and failed-import completion-stamp removal.
- `trees_gpu`: baked alpha silhouette, actual lit mesh/impostor pixels, indirect
  counts, reconstructed shadow depth, view axes/poles, wind/TAA, rebasing and disable.
- `trees_stream_gpu`: combined grass/tree rendering, indexed stitched terrain
  root attachment, CPU/GPU identity agreement, 3 m cross-cell spacing, coherent
  stands, bounded distinct update jobs, teleport/return and terrain edits.

Debug and Release engine builds succeed. All nine tree SPIR-V modules pass
`spirv-val --target-env vulkan1.3`. The four tree-specific tests pass, including
Vulkan API/synchronization validation. The actual production asset target also
performs an unchanged no-op rebuild.

The broader run passed 33 of 34 checks: CPU systems, UI/review settings, materials,
shadows, temporal rendering, foliage and vegetation interaction. The standard
grass-streaming test hit its 12-second terrain-settle deadline on llvmpipe. Its
identical workload and assertions pass with the explicit software timeout:

```sh
OPENWORLD_SOFTWARE_GPU_TESTS=1 timeout 240s cmake-build-debug/vegetation_stream_gpu_tests
```

That run also exposed and verified a shared scheduling correction: one expensive
population cell must be allowed when a positive soft budget remains. Otherwise,
rounding an observed cost above the allowance can permanently prevent new cells.
The CPU regression covers this case, required repairs and exhausted budgets.
The ordinary grass test retains its original deadline for native runs.


## Review

The generated source art and exact ImageGen prompts are under `assets/trees/source`.
The full library is built in each CMake build directory; generated runtime files
are not tracked. Geometry is generated offline from branch skeletons and crown
attractors. Mature pines use raised, broader crowns; young pines retain a conical
form. Bark bump, roughness and leaf ambient visibility are authored approximations.

[All 24 silhouettes](library.png) show one unlit baked direction per asset.
[Oak mesh](oak-final.png), [forced oak impostor](impostor-final.png), and
[birch close-up](birch-close.png) expose silhouette, bark and leaf detail.
[The combined canopy view](combined.png) includes existing ground vegetation and
sunlight through the crown gaps.
[The panorama](panorama.png) uses an 8.2 km-wide static fixture, a 4 km draw range,
and 151,123 visible entries. Its shadows are disabled to keep software review
practical; shadow reconstruction is verified in the close views and GPU fixtures.

The rendered reviews exercise mesh and forced impostor silhouettes, bark/leaf
close-ups, under-canopy gaps, dappled shadows, and existing vegetation alongside
trees. They are engineering review captures, not a claim that final artistic
quality has been accepted against the user's references.

The short captures use only two measured frames after three warmup frames; their
CSV/summary files are recorded beside the images. They are smoke measurements,
not statistically meaningful p95 results. Warm render-thread `new` counts were
zero. The 32,768-cell RGBA fallback allocation probe reported 755,340,048 bytes
(720.3 MiB), below its 768 MiB ceiling. The 151k-entry panorama reported zero list
overflow. GPU timings are llvmpipe timings and should not be compared to native
budgets.

Representative capture commands (Release, software Vulkan selected as above):

```sh
cmake-build-release/tree_probe --scene canopy --vegetation --quick --frames 2 --warmup 3 --output /tmp/tree-combined
cmake-build-release/tree_probe --scene panorama --span 128 --quick --no-shadows --width 480 --height 270 --frames 2 --warmup 3 --output /tmp/tree-panorama
cmake-build-release/tree_probe --scene specimen --capacity 32768 --rgba --quick --width 160 --height 90 --frames 2 --warmup 3 --output /tmp/tree-budget
```


## Remaining acceptance

- Run paired trees-off/on Release measurements on the intended native GPU at
  1920x1080, including full scene systems and representative terrain.
- Measure cold fill, walking, fast aerial travel, teleport/return and 8 km stress.
- Verify bindless-capable native hardware as well as the exercised fixed-binding
  path; tree materials themselves use dedicated fixed sets on either device.
- Tune density, shadow resolution, texture/crown detail and transition bands from
  those measurements. Distant CSM cannot preserve individual leaf shadows.
- Final visual acceptance for the two user references remains separate from
  automated correctness checks.

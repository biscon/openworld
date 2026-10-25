# European summer tree library

The engine loads the generated library from its CMake build directory. Source
artwork and recipes are tracked here; runtime geometry and DDS atlases are not.

- Beech (*Fagus sylvatica*), pedunculate oak (*Quercus robur*), silver birch
  (*Betula pendula*) and Scots pine (*Pinus sylvestris*).
- Six deterministic forms each: two young, two woodland, two spreading/irregular.
- Three geometric LODs plus a full-sphere octahedral impostor per form.
- Eight source images: one transparent leafy twig/needle spray and one bark
  texture per species. Generated with the built-in ImageGen tool. Exact prompts
  and provenance are in [source/prompts.json](source/prompts.json).

Build with `cmake --build cmake-build-release --target tree_assets`. The ordinary
`engine` target depends on this import. The offline baker needs Vulkan 1.3;
software Vulkan can build the assets without a native graphics card. When the
system Vulkan loader is misconfigured, select the desired installed ICD using
`VK_DRIVER_FILES` before building. The importer retains optimization in Debug builds to keep BC7 encoding practical.

`tree_import source/library.json OUTPUT SHADER_DIRECTORY [--validation]` is the
standalone importer. Never invoke ImageGen during a normal asset build. All meshes,
atlases, alpha-preserving mips and compression are derived deterministically from
the saved recipe and source images. Small test recipes can set `texture_size`,
`tile_size`, `frames`, `attractors` and `compressed` independently.

Runtime output contains `library.bin`, species bark/leaf DDS files and per-tree
colour, normal/material and R16 depth atlases. `_rgba.dds` alternatives support
uncompressed devices and `--rgba-materials`; their impostor colour/normal tiles
are 64 pixels instead of 128 to preserve the memory budget. Depth remains 128
pixels. Normal/material alpha is the leaf classification, not transparency.
Bark roughness/bump and leaf ambient visibility are artistic approximations.

See [the system design](../../docs/rendering_trees.md) for placement, ownership,
budgets, controls, verification and remaining native acceptance work.

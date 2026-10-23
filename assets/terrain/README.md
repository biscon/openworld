# Temperate uplands palette

Authored using the built-in Codex image-generation tool, 2026-10-17. No API/CLI
image generator was used. The tool did not report a model/version or reproducible
seed. Sources returned at 1254×1254 despite requesting 2048×2048; the importer
resamples them to a common 2048×2048 palette. These are not native 2K scans.

`source/prompts.json` preserves the exact authoring prompts and returned filenames.
`source/palette.json` specifies import settings and physical scale. Height maps
were generated as edits of their associated colour map, not independently.
Height is an artistic approximation; small relief amplitudes deliberately limit
artifacts from imperfect registration. Roughness is a conservative constant per
material, AO is one, metallic is zero. No albedo-to-height conversion is used.

Build the runtime palette explicitly (a normal `engine` build does this automatically):

```sh
cmake --build cmake-build-release --target terrain_assets
```

Generated DDS files and the copied manifest live in
`cmake-build-release/assets/terrain/runtime` (or the corresponding Debug build
directory). Only source artwork, prompts, recipes and importer code are tracked;
runtime palettes are build products. Source/recipe/importer edits or missing outputs
trigger import; unchanged builds reuse the existing files. No build invokes imagegen
or downloads artwork. `*_rgba.dds` are the format fallback and comparison versions.
Colours are sRGB, normals linear XY with positive Z, scalars linear roughness/AO/
height/one. All materials repeat over 4 metres at default settings. Changing
repeat scale retains physical height through the normal-strength correction.

`rock_review.json` is the initial 256-square vertical-slice recipe, intentionally
using rock in all four slots; it is not the production palette.

## Slice 5C macro palette

`source/*_macro.png` are four separate built-in imagegen outputs, generated
2026-10-17. Exact prompts are in `source/macro_prompts.json`; model version and
seed were not exposed. Original 1254-square images are retained. These describe
broad colour regions over 64 m, not enlarged copies of the 4 m micro textures.

The importer area-filters colour to 32 square, normalizes each linear RGB channel
around 0.5 and wrap-reconstructs a 512-square texture. Runtime sampling bounds its
effect to ±20% colour. Alpha is an independently seeded periodic roughness field
whose recipe amplitude is 0.35; it is not inferred from albedo. The runtime bounds
roughness modulation to ±0.08. sRGB encoding applies to RGB only; alpha is linear.
Both compressed and RGBA palettes contain four layers and complete mip chains.

Rebuild only the macro array without touching approved micro assets:

```sh
cmake-build-release/material_import assets/terrain/source/palette.json cmake-build-release/assets/terrain/runtime --macro-only
```

[Repeated/offset review](../../docs/images/materials/macro_review.png) shows the
normalized modulation textures, not final terrain colours. The low-pass import
removes fine generated grain and smooths wrap boundaries. Runtime rendering and
limitations are documented in [the 5C design](../../docs/rendering_terrain_blending.md).

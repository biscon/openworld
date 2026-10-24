# Vegetation source assets

[Fern](source/fern.png), [bilberry-like shrub leaf](source/shrub.png),
[daisy](source/daisy.png) and [heather](source/heather.png) albedo/opacity were generated with the built-in
image_gen tool on 2026-10-24. Exact prompts and provenance are in [source/prompts.json](source/prompts.json).
Source PNGs retain generated transparency. These are artistic samples, not scans or
measured PBR data. The importer authors a deliberately shallow analytic leaf normal,
independent of colour brightness; roughness and transmission are explicit constants.

`vegetation_assets` imports source/library.json into each build tree. Recipe version 2 packs four sources into a 2×2 colour/normal atlas, builds tile-local
coverage-preserving mips and ends at 4×4 texels per tile. Six baked assets include
three LODs and species/interaction metadata in library format 4. Runtime reads
only saved library binaries and DDS files. Keep source artwork and recipes tracked;
do not commit generated build outputs. Tests own their synthetic sources.

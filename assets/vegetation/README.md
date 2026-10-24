# Vegetation source assets

[Fern](source/fern.png) and [bilberry-like shrub leaf](source/shrub.png) albedo/opacity were generated with the built-in
image_gen tool on 2026-10-24. Exact prompts and provenance are in [source/prompts.json](source/prompts.json).
Source PNGs retain generated transparency. These are artistic samples, not scans or
measured PBR data. The importer authors a deliberately shallow analytic leaf normal,
independent of colour brightness; roughness and transmission are explicit constants.

`vegetation_assets` imports source/library.json into each build tree. Runtime reads
only saved library binaries and DDS files. Keep source artwork and recipes tracked;
do not commit generated build outputs. Tests own their synthetic sources.

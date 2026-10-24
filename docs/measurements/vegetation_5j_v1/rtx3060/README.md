# RTX 3060 vegetation experiment — 2026-10-24

## Decision

Keep **blade tufts** as the starting representation. The tested intersecting cards
and blade/card hybrid did not improve the median incremental GPU time in any of the
five views. Some repeat ranges overlap: this is evidence about these assets and
this implementation, not a universal claim that cards are slower.

**Do not cast individual grass-blade shadows in production.** Grass still receives
terrain/tree/object sun shadows and participates in GTAO when enabled. Shrubs,
larger plants and rocks retain independently budgeted casting. GTAO affects ambient
lighting and does not replace directional shadows or guarantee subpixel blade detail.
The probe retains the original 25 m casting default for repeatability; explicitly
use `--grass-shadow-distance 0` for the agreed production policy.

The moving-view receive-only case costs **1.248 ms** incremental GPU time without
AO and **1.388 ms** with half-resolution GTAO, versus **2.749 ms** in the full run
with grass casting (2.848 ms in the independent focused default run). The GTAO case
has repeat deltas **1.196–1.555 ms**: its median fits the approximately 1.5 ms budget,
but the margin is narrow and this is not all-view or full-scene acceptance.
Proceed to V2 asset/foliage work with this policy and repeat measurements as coverage
and appearance improve. Do not relax the grass budget to accommodate future trees
and water. The eventual complete scene may target 60 FPS on this card; retain the
tighter 120 FPS development objective and existing feature budgets.

## Method and limits

Native NVIDIA GeForce RTX 3060, 12 GiB, driver 615.78.08; Release with IPO, GCC 16.2.1.
The sandbox hides GPU devices; these runs used approved execution outside it.
No clock or power settings were changed. [Provenance](provenance.json) records
fingerprints and sampled telemetry. The diagnostic probe adds command-line controls
and an optional receiver fixture; renderer/shader binaries are unchanged.

1920×1080 scene/native output; 256 m ground fixture, 524288 candidates at 8 tufts/m²;
20/45/70 m LOD, fade 55–70 m, 2048 CSM maps. TAA on. Main comparisons use GTAO off,
CSM on, grass shadow fade 20–25 m. Coverage calibration selects card width/density
1/1; mean 4×4-tile coverage error is 0.0202 flat and 0.0205 rolling. Equal aggregate
coverage does not establish equal silhouettes or quality.

Every timed case has 120 warmup frames, 600 samples, three paired vegetation-off/on
repetitions with alternating order. Tables report the median of the three differences
between on/off median total GPU times, with the repeat range. They are **not**
percentiles of per-frame differences. Raw `paired.csv` also contains differences
between p95 values; actual per-run p95 times are in `timings.csv`.

The full matrix contains 90000 measured frames / 75 pairs; focused variants contain
25200 / 21; each receive-only comparison contains 3600 / 3. All record zero overflow
and zero C++ `new/new[]` calls inside `render()` with validation disabled. Peak charged
vegetation allocations are **72,332,992 bytes (69.0 MiB)**, including retained upload
and diagnostic buffers, excluding shared render targets and driver-private memory.
CPU vegetation preparation is approximately 0.035–0.040 ms median, with typical p95
0.042–0.051 ms. Whole-render CPU times include synchronization and are not equivalent
to vegetation CPU preparation.

This fixture has a plain ground mesh, no streamed terrain, trees, water, atmosphere
or volumetrics, and unfinished foliage assets/shading. Total-frame fixture timings
are not an estimate of complete-game FPS. Final density, ground blending, leaf
transmission and long-duration motion quality remain open.

## Representation comparison

Incremental GPU milliseconds, median [minimum–maximum across three pairs]:

| View | Blade tufts | Calibrated cards | Calibrated hybrid |
| --- | --- | --- | --- |
| close | 2.498 [2.433–3.127] | 2.815 [2.700–2.828] | 2.697 [2.605–2.739] |
| grazing | 2.425 [2.323–2.750] | 2.966 [2.723–3.101] | 2.612 [2.535–2.779] |
| moving | 2.749 [2.580–2.846] | 3.240 [3.035–3.303] | 3.133 [3.077–3.148] |
| aerial | 1.951 [1.866–1.970] | 2.081 [1.910–2.084] | 2.138 [2.058–2.159] |
| rolling | 2.598 [2.525–2.872] | 3.049 [2.873–3.118] | 2.967 [2.815–3.048] |

The original grass-casting configuration exceeds the 1.5 ms target in every view.
In close view, cards reduce shadow raster time (~0.61 vs ~1.31 ms) but increase
colour time (~1.88 vs ~0.96 ms). Card alpha coverage/overdraw can offset their lower
triangle count. Pass timestamps diagnose work; do not add them as a substitute for
paired total-frame deltas.

## Focused moving-view comparisons

| Configuration | Incremental GPU ms [repeat range] |
| --- | --- |
| Original default: grass casts to 25 m | 2.848 [2.758–2.901] |
| ALL CSM off (casting and receiving) | 0.455 [0.352–0.499] |
| taa_off | 2.616 [2.359–2.897] |
| strong_wind | 2.961 [2.712–3.098] |
| low_sun | 3.077 [2.670–3.080] |
| prepass | 2.760 [2.673–2.849] |
| Grass casting + half GTAO | 3.082 [3.081–3.102] |
| Grass receiving only, AO off | 1.248 [0.978–1.275] |
| Grass receiving only, half GTAO | 1.388 [1.196–1.555] |

The all-CSM-off variant must not be used to estimate receive-only grass cost.
Receive-only metadata confirms CSM and TAA remain enabled; grass caster counts and
shadow raster timings are zero. With half GTAO, camera vegetation depth is rendered
for AO, followed by colour shading. AO is enabled in both off/on members of that
pair, so 1.388 ms is the incremental vegetation cost in an AO-enabled scene,
**not** the combined standalone cost of vegetation plus the entire GTAO system.

Receive-only selection still costs about 0.50 ms: the experiment dispatches selection
for all cascade views even when no grass caster survives. Skipping empty caster-view
selection is a concrete future optimization; no saving from it is claimed here.
Production shadow policy must also exclude grass from caster bounds/submission.

## Validation and images

[Native vegetation GPU tests](validation.log) pass with synchronization validation
on both fixed and bindless material paths. Performance runs disable validation.
Short canopy captures separately exercise the new diagnostic controls with validation;
all three completed without validation errors. They are visual evidence, not
performance samples. The canopy is an artificial overhead
quad casting a broad shadow, not a tree asset. Reviewed captures retain that broad
shadow with grass casting disabled. Individual grass casting adds soft streaks on
the ground; GTAO changes are subtle in this dark, unfinished lighting fixture.
These captures do not establish final foliage appearance or sufficient contact
shading in a finished meadow.

- [Close blades](../../../images/vegetation_5j_v1/rtx3060/close_blades_count.png),
  [close cards](../../../images/vegetation_5j_v1/rtx3060/close_cards_coverage.png).
- [Grass receives shadows + GTAO, no grass casting](../../../images/vegetation_5j_v1/rtx3060/canopy_receive.png).
- [Same scene with grass casting + GTAO](../../../images/vegetation_5j_v1/rtx3060/canopy_cast.png).
- [Receiving shadows, GTAO off](../../../images/vegetation_5j_v1/rtx3060/canopy_noao.png).

## Reproduction and files

Build `vegetation_probe` and `vegetation_gpu_tests` in Release. Run sequentially on
the native GPU; do not benchmark concurrent GPU jobs. Base command:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json cmake-build-release/vegetation_probe OUTPUT_DIRECTORY ARGUMENTS
```

| Saved directory | Arguments |
| --- | --- |
| `full` | No additional arguments |
| `variants` | `--sequence moving --representation blades --variants --no-captures` |
| `receive` | `--sequence moving --representation blades --grass-shadow-distance 0 --no-captures` |
| `receive-ao` | `--sequence moving --representation blades --grass-shadow-distance 0 --gtao half --no-captures` |
| `canopy-cast` | `--sequence close --representation blades --canopy-fixture --gtao half --frames 4 --warmup 16 --repeats 1 --require-validation` |
| `canopy-receive` | Same canopy arguments plus `--grass-shadow-distance 0` |
| `canopy-noao` | Same as canopy-receive but omit `--gtao half` |

Each timed directory preserves metadata, calibration, per-run median/p95, paired
results, a derived JSON summary and losslessly compressed raw `frames.csv.gz`.
The unedited generated `findings.md` is generic harness output; decisions are in this
report. `summarize.py DIRECTORY` regenerates JSON after decompressing `frames.csv.gz`
into `frames.csv`. The full transient runs were written under
`/tmp/vegetation-rtx3060-*`; only selected PNGs are archived here. Rerunning the full
probe regenerates all 24-frame motion sequences and diagnostic images.

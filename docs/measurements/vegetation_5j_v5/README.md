# 5J-V5 — interaction and habitat verification, 2026-10-25

Implemented: bounded GPU bend/recovery history, an independently controlled ground
proxy, blended habitats, and six saved species assets including daisy and heather.
The correctness checks pass on the RTX 3060. The approximate 1.5 ms incremental
GPU budget is **not met** by these full-scene measurements; performance and extended
visual acceptance remain V6 work. Nearby density has not been reduced to meet a budget.

## Measurements

Release, NVIDIA GeForce RTX 3060, uncapped offscreen rendering. The existing streaming
probe uses normal engine terrain, seed 173, 16 km streaming distance, curved horizon,
physical atmosphere, production terrain palette, CSM, TAA and bloom. Blended habitats,
default wind, full nearby density, 12,288 vegetation cells. GTAO and vegetation Hi-Z
are off for timing. Native desktop resolution is 2560×1440.

Each sequence measures vegetation off, vegetation with interaction disabled, then
vegetation with a moving circular proxy. Population must settle for eight consecutive
completed frames before the enabled pair; each phase resets animation time and has
240 warmup plus 240 measured frames. These are single sequences, not pooled trials.
Terrain habitat shading remains enabled in the vegetation-off baseline. CPU time
is the render-call interval, including waits; vegetation CPU is instrumented preparation
and recording. GPU frame intervals are not sums of overlapping per-pass timings.

| Resolution / range / state | CPU median / p95 (ms) | GPU median / p95 (ms) | Vegetation CPU median / p95 (ms) |
| --- | --- | --- | --- |
| 1080p / 80 m / vegetation off | 8.685 / 9.575 | 6.824 / 7.708 | 0.005 / 0.007 |
| 1080p / 80 m / interaction off | 8.916 / 10.231 | 8.532 / 9.745 | 0.248 / 0.318 |
| 1080p / 80 m / interaction on | 9.149 / 10.834 | 9.032 / 10.727 | 0.243 / 0.279 |
| 1080p / 200 m / vegetation off | 8.169 / 8.821 | 7.015 / 8.065 | 0.003 / 0.005 |
| 1080p / 200 m / interaction off | 11.414 / 13.476 | 11.498 / 13.473 | 0.389 / 0.464 |
| 1080p / 200 m / interaction on | 11.276 / 12.789 | 11.392 / 12.851 | 0.411 / 0.521 |
| 1440p / 80 m / vegetation off | 13.946 / 15.184 | 9.733 / 10.883 | 0.003 / 0.008 |
| 1440p / 80 m / interaction off | 12.511 / 14.505 | 12.329 / 14.527 | 0.248 / 0.286 |
| 1440p / 80 m / interaction on | 12.813 / 14.472 | 12.569 / 13.997 | 0.265 / 0.321 |
| 1440p / 200 m / vegetation off | 12.484 / 14.881 | 10.490 / 14.422 | 0.006 / 0.009 |
| 1440p / 200 m / interaction off | 15.216 / 16.456 | 15.159 / 16.494 | 0.461 / 0.548 |
| 1440p / 200 m / interaction on | 14.514 / 15.822 | 14.409 / 15.933 | 0.418 / 0.510 |

Incremental GPU medians with interaction enabled are **2.208 / 4.377 ms at 1080p**
and **2.836 / 3.919 ms at 1440p**, for 80 / 200 m respectively. The 200 m
interaction-on sample being faster than interaction-off is run variability, not an
interaction speedup. The field dispatch itself is approximately 0.003 ms; this
excludes its vertex sampling/deformation cost, which appears in depth/colour/shadows.
The 80 m on/off interaction difference is 0.500 ms at 1080p and 0.240 ms at 1440p.
Multiple interleaved trials are needed to isolate small differences reliably.

Actual charged vegetation GPU memory is **163.889 MiB** at both ranges, including
field storage. All measured intervals have zero render-thread C++ allocations,
zero pending cells, and zero population/attachment updates. The allocation counter
excludes drivers and background workers. Visible draw entries are approximately
23,826 / 63,884 at 80 / 200 m. These settled runs do not establish cold-start or
moving-camera streaming cost. The proxy moves independently of the stationary camera.

Console records: [1080p, 80 m](80.txt), [1080p, 200 m](200.txt),
[1440p, 80 m](native80.txt), [1440p, 200 m](native200.txt).
Reproduce with a Release build, for example:

```sh
cmake-build-release/vegetation_stream_probe /tmp/vegetation-v5 --engine-scene --distance 80 --v5-check --size 1920 1080
```

## Correctness and visual review

The [retained verification log](verification.txt) records eight Debug CPU/build checks
and twelve Release checks, including native temporal, UI, material, shadow, foliage,
import, streaming and interaction tests. Vulkan synchronization validation is enabled
for GPU correctness checks and disabled for timing. Both fixed and bindless material
paths are exercised. Debug and Release builds succeed; all 103 SPIR-V modules in
each build validate for Vulkan 1.3. Final proxy removal/route recovery checks pass
in both builds after the helper correction.

Owned fixtures cover bounded contacts and displacement, root anchoring, half-life
recovery, exact scrolling overlap and zero strips, camera-only large rebasing,
teleport/return, disable/re-enable, reset, in-flight ring reuse, matching depth/colour
and submitted deformation motion. Streaming fixtures cover large signed habitat
coordinates, deterministic population, conservative offscreen casters and shared
terrain suitability. CPU/build checks cover old bookmark/library formats, atlas mip
isolation and dependency rebuilds. Proxy removal leaves the existing trail to recover;
explicit reset clears it. Sweeps longer than 64 m are rejected as invalid teleport input.

The four source images and exact generation prompts remain in the
[asset directory](../../../assets/vegetation/README.md). Fern and shrub geometry have
unequal branches and leaf sizes; daisies have curved stems and tilted radial flower
heads; heather has branched, tapering flower clusters. Each has three saved LODs.

Uniform inspection captures: [meadow](v5-habitat-0.png), [moor](v5-habitat-1.png),
[understory](v5-habitat-2.png). The [blended scene with a moving proxy](v5-bent.png)
uses the same production population path. These presets expose the different
species distributions; they are inspection modes, while ordinary engine startup
uses blended habitats. Full scene captures retain the default EV 15 exposure.

The ordinary `engine` executable also completed three 1,200-frame runs with zero
Vulkan validation errors: [close TAA](engine-close.png), [grazing low sun, wind 2,
AA off, half-resolution GTAO and Hi-Z](engine-grazing.png), and [aerial TAA with
GTAO/Hi-Z](engine-aerial.png). These inspection views use EV 14. The aerial view
shows the blue independent ground marker and local disturbance. Saved startup views
are [close](engine-close.json), [grazing](engine-grazing.json) and
[aerial](engine-aerial.json); load them from the repository root with `--review-view`.
The temporary bend field is deliberately regenerated, not saved in those files.

Individual close specimens: [daisy](daisy.png), [heather](heather.png),
[fern](fern.png), [shrub](shrub.png). The existing specimen probe ran each with
default TAA/CSM, shadows off, TAA off, strong wind, low sun, prepass off and GTAO,
with native synchronization validation and no errors. Retained specimen images show
the default variant; the quick probe is visual/correctness evidence, not a benchmark.
Thin silhouettes remain noisy in the grazing AA-off view, and the distant grass
boundary is conspicuous. Those are explicit final tuning items, not passed visual gates.

## Remaining acceptance

The 256 MiB allocation ceiling and settled no-allocation requirement pass. The
incremental GPU budget fails at both ranges/resolutions; CPU preparation also exceeds
the approximate 0.25 ms target at 200 m and at native-resolution 80 m. The 120 FPS
engineering target remains unchanged. These figures do not establish production
scale streaming performance or guaranteed 60 FPS.

V6 must tune selection/vertex/colour costs and review longer camera/player routes,
cold/refinement stress, temporal stability in motion, and distant ground-to-grass
continuity. The close plant assets are implemented, but these captures do not close
the roadmap's final reference-quality/density acceptance gate. The pre-existing
standalone GTAO numerical issue recorded in the V4 report is not claimed fixed by V5.

# 5J-V4 — engine integration verification, 2026-10-24

RTX 3060, Release, uncapped offscreen 1920×1080. The existing streaming probe uses
normal engine-scale terrain (128 m tiles, 0.5 m spacing, 8 km view, horizon and
curvature), the production terrain palette, physical atmosphere, CSM, TAA and bloom.
Seed 173, default terrain height scale, ground-level camera. Vegetation uses the
saved V2 library, full nearby density, 12,288 cells and a 256 MiB ceiling. GTAO and
vegetation Hi-Z are off except in the stated occlusion check. No new viewer was added.

## Measured cost

Each stationary pair uses 120 warmup + 600 measured frames per member, after terrain
and vegetation settle. Values are milliseconds; CPU is the render-call interval,
which includes frame pacing/waits. Vegetation CPU is its instrumented preparation
and recording work. GPU totals are timestamp intervals, not sums of overlapping
per-pass costs. Measurements are individual runs, not statistically pooled trials.

| Range / state | Total CPU median / p95 | Total GPU median / p95 | Vegetation CPU median / p95 |
| --- | --- | --- | --- |
| 80 m off | 8.194 / 9.200 | 5.968 / 6.898 | 0.002 / 0.004 |
| 80 m on | 8.514 / 9.396 | 7.106 / 8.646 | 0.226 / 0.288 |
| 200 m off | 8.146 / 8.972 | 6.060 / 6.590 | 0.002 / 0.004 |
| 200 m on | 9.798 / 11.062 | 9.577 / 10.927 | 0.420 / 0.529 |

Incremental median GPU cost is **1.137 ms at 80 m**, **3.517 ms at 200 m**.
The close views contain 23,044 and 77,939 visible draw entries respectively.
Actual charged vegetation GPU allocations are **151.955 MiB** at both ranges;
capacity is preallocated. All four measured stationary intervals have zero
render-thread C++ allocation calls and zero population/attachment updates.
The counter excludes driver and background worker allocations.

| Range | Selection | Vegetation depth | Colour | CSM |
| --- | --- | --- | --- | --- |
| 80 m | 0.603 | 0.365 | 1.052 | 0.006 |
| 200 m | 2.625 | 0.469 | 1.298 | 0.005 |

A deliberately occluding wall at 80 m reduces visible entries from 23,044 to 1,765.
Optional vegetation Hi-Z changes total GPU median from 5.711 to 5.198 ms (p95
6.815 to 6.493); both samples allocate zero times on the render thread. This is a
controlled benefit case, not a claim that Hi-Z helps open meadows. It defaults off.

The 80 m stationary/occlusion pair precedes the final CPU surface-cache correction;
the GPU selection/rendering code is unchanged. The correction retains an owned,
stably ordered patch snapshot so traversal order and distant topology edits do not
spuriously change otherwise identical attachment signatures. Canonical data-only
provider readiness still invalidates missing-provider caches. Moving measurements
are repeated after the correction; see the [retained console summaries](measurements.txt).

## Streaming and remaining acceptance

The route walks 27 m over 180 frames, teleports eight terrain chunks away for eight
frames, returns, then measures another 172 frames. A separate bounded interval
checks recovery to zero pending/update work. At 200 m the fixed 32-cell update cap
cannot refill the entire population during those 172 frames; this is progressive
repopulation, with missing cells hidden, not an allocation or density workaround.

| Final route | Total CPU median / p95 | Total GPU median / p95 | Vegetation CPU median / p95 | Pending at route / recovery end |
| --- | --- | --- | --- | --- |
| 80 m | 9.523 / 11.122 | 7.596 / 9.675 | 0.493 / 1.670 | 0 / 0 |
| 200 m | 10.012 / 12.583 | 8.151 / 10.317 | 1.362 / 3.128 | 6,652 / 0 |

Both routes and their recovery intervals have zero render-thread C++ allocations.
Update GPU p95 is 0.044 ms at both distances; observed maxima are 0.712 ms at 80 m
and 0.050 ms at 200 m. These include transient spikes, not just settled averages.
The post-return recovery interval at 200 m performs 13,304 frame-attachment updates,
including 6,652 newly populated cells, and finishes with no pending cells.

The default stationary GPU and median vegetation CPU meet the approximate 1.5 ms /
0.25 ms goals. Moving CPU spikes still exceed the CPU goal, and 200 m has a materially
higher selection cost. These remain explicit V6 tuning/acceptance items. Nearby
population density was not reduced to obtain these results.

Cold initialization records four render-thread C++ allocations. The first 600 frames
of a genuinely cold terrain run precede vegetation readiness, so they do not establish
cold population cost. The retained 200 m run starts from a warmed disk cache and has
update GPU p95/max 0.041/0.050 ms during that interval. This is not a cold-disk claim.

## Correctness and appearance

[Native verification logs](verification.txt) are retained. Debug and Release builds succeed. Nine focused Debug CPU tests and seven Release
CPU tests pass, covering live range, identity, capacity/retirement, canonical terrain
demand, bookmarks and UI state. All 93 SPIR-V modules validate in both builds.
Native foliage and streaming tests pass with synchronization validation, including
fixed/bindless materials, longer range, independent offscreen casters, stable overflow,
solid-depth occlusion/restoration, GTAO integration, stitched surfaces, teleport,
large coordinates and local temporal history. Existing vegetation, temporal, visibility,
shadows and UI native checks also pass. The material GPU test passes when run directly
with a 120-second allowance; its CTest 60-second timeout was insufficient.

The standalone GTAO numeric test fails on this GPU at the same assertion and with
identical values on unchanged HEAD: `0.976471 != 0.978487` in its tiled-filter reference
comparison. That pre-existing failure is not suppressed or counted as passing. The
vegetation/GTAO integration fixture passes. Software Vulkan was not repeated.

Main-engine captures: [close, 80 m](close.png), [grazing, 80 m](grazing.png),
[low aerial, 200 m](aerial.png), [strong wind/low sun, Hi-Z + half GTAO, TAA off](wind-low-sun.png).
The [Vegetation window](vegetation-panel.png) is exercised by the owned UI fixture
(which intentionally uses its own 70 m setting).
All four main-engine runs finish with zero validation errors and settled terrain.
They use the actual `engine` executable with native validation,
loaded review bookmarks, production assets and settled terrain. The dense close view
is integrated with terrain, atmosphere and shadows. Existing assets remain dark and
uniform, and the transition to distant ground still needs art tuning; these captures
are integration evidence, not final habitat-art approval. Extended motion-art review
and production-scale stress remain V6 acceptance work.

## Reproduction

```sh
cmake --build cmake-build-release --target engine vegetation_stream_probe
cmake-build-release/vegetation_stream_probe /tmp/v4-80 --engine-scene --distance 80 --occlusion-check
cmake-build-release/vegetation_stream_probe /tmp/v4-200 --engine-scene --distance 200
# Focused traversal/recovery without another stationary pair:
cmake-build-release/vegetation_stream_probe /tmp/v4-200 --engine-scene --distance 200 --route-check
# Review the same close camera in the engine itself:
cmake-build-release/engine --review-view /tmp/v4-80/close-view.json
```

Native execution needs GPU access outside the sandbox. The probe has a 180-second
full-scene deadline and bounded settling/recovery intervals; failures stay failures.
Small main-engine captures and console summaries accompany this report.

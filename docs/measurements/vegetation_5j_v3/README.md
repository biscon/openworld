# 5J-V3 — short native verification, 2026-10-24

RTX 3060, Release, uncapped offscreen 1920×1080, TAA and CSM, GTAO off.
Saved V2 mixed assets, seed 173, 4 m cells, 96 m demand / 112 m retention,
128 candidates/cell, 70 m rendering distance, simple streamed terrain (12 m height
scale, 512 m maximum terrain range). This is a controlled integration scene, not a
full-feature game workload or final habitat-art acceptance.

## Retained results

| Check | Result |
| --- | --- |
| Paired total GPU median, vegetation off / on | 1.375 / 2.581 ms |
| Incremental GPU median | 1.205 ms |
| Paired total GPU p95, off / on | 2.014 / 3.917 ms |
| Final stationary vegetation CPU median / p95 | 0.095 / 0.127 ms |
| Final on-only total GPU median / p95 | 2.584 / 3.513 ms |
| Charged vegetation GPU allocation | 72.62 MiB |
| Visible draw entries in the close view | 17,907 |
| Final stationary render-thread C++ allocations | 0 in 120 measured frames |
| Final stationary generation / attachment work | 0 |
| Cold update GPU p95 / maximum in final CPU check | 0.047 / 0.058 ms |
| Cold vegetation CPU median / p95 | 0.096 / 0.386 ms |

The paired measurement used 120 warmup + 600 measured frames per member. A bounded
360-frame walking/teleport/return route completed with zero pending cells at the end
and no list overflow. Its update GPU p95/max was 0.045/0.049 ms. No per-frame CSV was
produced. Full raw console summaries and two PPM captures remain in `/tmp/vegetation-v3-*`;
only this compact report and a small review image are retained in the repository.

The first pair exposed excessive CPU reads from mapped upload memory (2.15 ms median
vegetation preparation). Moving preparation into CPU storage reduced it to 0.75 ms;
caching unchanged surface records reduced the final stationary sample to 0.095 ms.
Only the concrete regression was rechecked: one repeated short pair/route, followed
by a 120-frame stationary CPU sample. The final surface-cache change did not change
GPU shaders; the paired GPU values precede it, and the on-only values follow it.
The route was not repeated after that CPU-only cache change, so moving CPU p95 on
the final version is not claimed. Background terrain/cache-worker allocations are
excluded from the final render-thread allocation counter; earlier global counts
were not vegetation-specific. Four render-thread allocations occurred during cold
initialization; none occurred in the stationary sample.

## Correctness and limits

[Final check summary](verification.txt): focused Debug/Release CPU checks cover residency, lifetime, identity, shared work
budgets and existing vegetation assets. Twelve affected SPIR-V modules validate.
Native Vulkan synchronization validation covers the new streamed fixture and the
existing vegetation/foliage paths. An eight-frame main-demo `--vegetation` smoke
run also reports zero validation errors; it checks startup, not terrain settling. Fixtures own their data and do not load game assets.

The streamed GPU fixture checks negative and large coordinates, CPU/GPU identity
agreement, sloping triangle attachment, forced stitched boundaries in four orientations and refinement,
local temporal rejection, density independence, exclusions, missing terrain during
teleport, recovery, and fixed/bindless rendering. CPU tests cover capacity pressure,
stale handles, retirement and invalid configuration.

The [close review image](../../images/vegetation_5j_v3_close.png) shows the controlled
plain-ground scene. Dark grass, uniform ground and the final fade remain V2/V4/V6
art/coverage limitations. Root/provider correctness is established by owned numeric
fixtures; this image is not reference-quality signoff. Software Vulkan, an extended
stress matrix, low-aerial full-scene art review and final moving CPU-spike acceptance
were not run in this deliberately short verification pass.

Reproduce the short probe with:

```sh
cmake-build-release/vegetation_stream_probe /tmp/vegetation-v3-review
# Focused CPU-only follow-up, without another pair/route:
cmake-build-release/vegetation_stream_probe /tmp/vegetation-v3-review --cpu-check
```

Native execution requires GPU access outside the sandbox. The probe imposes a
75-second internal deadline; correctness tests have independent short deadlines.

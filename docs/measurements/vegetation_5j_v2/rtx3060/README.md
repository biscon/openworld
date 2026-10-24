# 5J-V2 handoff — RTX 3060, 2026-10-24

The asset/foliage foundation is implemented. **Mixed foliage and GTAO performance
acceptance remain open.** Do not repeat the full matrix without a relevant change;
use the focused cases below to investigate the remaining cost.

## Results worth retaining

Incremental GPU milliseconds, median of three paired off/on comparisons:

| View | Grass | Mixed grass/fern/shrub |
| --- | ---: | ---: |
| Close | 0.943 | 1.578 |
| Grazing | 0.671 | 1.082 |
| Moving | 0.928 | 1.445 |
| Aerial | 1.022 | 1.424 |
| Rolling | 0.962 | 1.674 |

The target remains approximately **1.5 ms GPU, 0.25 ms CPU preparation, 128 MiB**.
Half-GTAO moving comparisons cost **1.585 ms grass / 1.957 ms mixed**; GTAO is also
active in each no-vegetation baseline. These are not the combined cost of GTAO and
vegetation. CPU preparation is 0.062–0.098 ms median; storage is 49.82 MiB grass /
50.60 MiB mixed. All measured frames have zero C++ allocation calls and list overflow.

Mixed moving variants: default 1.543 ms, all CSM off 1.008, TAA off 1.085, strong
wind 1.452, low sun 1.579, **no prepass 2.554**. Keep the depth prepass default.
Separate runs vary: the full matrix's mixed moving value is 1.445 ms. Dynamic GPU
clocks and baseline cost affect paired differences; these results do not establish
stable headroom or complete-game FPS.

[Compact measurements](measurements.json) retain repeat ranges, total median/p95,
pass costs, CPU preparation, counts and memory for future profiling. Raw CSV runs
remain under `/tmp/vegetation-v2-{grass,mixed,variants,grass-ao}-final`; they are not
added to the repository. [Provenance](provenance.json) identifies the measured build.

## What changed and what to investigate next

- Saved 24/6/2-blade LODs, masked fern and two-part shrub; shared compact instancing,
  transmission, wind, submitted motion and isolated viewer. Sources and exact image
  prompts: [asset notes](../../../../assets/vegetation/README.md).
- Grass casting is excluded from bounds/lists/dispatch/draws. Sparse plant/shrub
  casters share 128-lane groups across cells, reducing mixed selection groups from
  49158 to 8838; measured selection falls from roughly 0.287 to 0.167 ms.
- Retain the existing prepass. A tested 48-byte per-instance animation cache had no
  total-frame win and was removed. Avoid repeating it without a new hypothesis.
- Remaining mixed costs are approximately 0.94 ms colour, 0.36 ms depth,
  0.17 ms selection plus casting. Investigate close/rolling and half-GTAO before
  expanding the plant library. Do not lower quality or relax budgets silently.

## Verification and visual limits

[Final native Release suite](final-native.log): all six vegetation CPU/build/GPU
checks pass with synchronization validation, including fixed/bindless and BC/RGBA
paths, offscreen casters, overflow, history reset and large coordinates.
[Final Debug software foliage check](final-foliage-software.log) also passes.
Debug engine and Release viewer/probe builds succeed; native SDL3 viewer smoke
passes; 46 affected SPIR-V modules validate. Incremental asset tests verify clean,
no-op, source/recipe edits, missing outputs and failure recovery.

[Broader native checks](debug-native.log) pass except the existing GTAO tiled-filter
comparison (`0.976471 != 0.978487`). The identical failure reproduces on unchanged
HEAD ([baseline evidence](gtao-baseline.log)); its tolerance was not changed.
Software GTAO and native foliage+GTAO integration pass.

Representative captures: [grass](../../../images/vegetation_5j_v2/grass-close.png),
[mixed](../../../images/vegetation_5j_v2/mixed-close.png),
[rolling](../../../images/vegetation_5j_v2/mixed-rolling.png),
[scene shadow + GTAO](../../../images/vegetation_5j_v2/canopy-gtao.png),
[fern](../../../images/vegetation_5j_v2/fern.png),
[shrub](../../../images/vegetation_5j_v2/shrub.png).
Specimens use EV100 14; scene measurements use 15. Dark backlit blades, tuft gaps,
sparse shrub silhouettes and the 70 m grass-to-ground transition still need visual
refinement. Full reference-quality acceptance is not claimed.

## Reproduction when needed

Release, native RTX 3060/driver 615.78.08, default clocks/power, 1080p, 256 m fixture,
524288 candidates, seed 173. Each case: 120 warmup, 600 measured frames, three
alternating off/on pairs. Configured prepass/GTAO stay active in both pair members.
Captures/readback are outside timing; shared targets and driver-private bookkeeping
are excluded from vegetation storage. The allocation counter covers C++ new/new[].

```sh
cmake --build cmake-build-release --target vegetation_probe vegetation_viewer
cmake-build-release/vegetation_viewer
# Native GPU access requires execution outside the sandbox.
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json cmake-build-release/vegetation_probe /tmp/vegetation-next-close --profile v2 --mixed --sequence close --no-captures
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json cmake-build-release/vegetation_probe /tmp/vegetation-next-ao --profile v2 --mixed --sequence moving --gtao half --no-captures
```

V3 streaming/attachment, V4 production visibility/terrain coverage, V5 interactions
and V6 full-scene acceptance remain separate work. No full-scene FPS claim is made.

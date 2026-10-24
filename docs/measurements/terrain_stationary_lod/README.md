# Stationary terrain review — 2026-10-24

Software Vulkan: llvmpipe LLVM 23.1.3, eight worker threads; host CPU Ryzen 9 7900.
This is correctness evidence, not RTX 3060 performance acceptance.

The explicit user bookmark supplies chunk (9,15), local
(61.958796,619.603861,116.658758), yaw 2.309998, pitch -0.205, and the default
terrain parameters. The production 256-cell / 0.5 m data grid and terrain palette
are retained. Coverage is reduced to 512 m and the distant bank is omitted;
curvature remains enabled. Shadows, GTAO, TAA and bloom are off to isolate surfaces;
the depth prepass is on. This does not test the distant horizon's visual appearance.

The 1080-pixel static capture uses the saved camera exactly. Ground motion follows
finest resident triangle heights with at least 1.8 m clearance, at 2, 6 and 30 m/s,
four seconds forward then four back. Both additional-quality settings are exercised.
Motion uses 1046×540 at four simulated updates per second because of software
raster cost. The GPU regression separately checks sub-metre observer movements.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json LP_NUM_THREADS=8 \
  cmake-build-release/terrain_ground_probe OUTPUT REVIEW.json 540 512 4
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json LP_NUM_THREADS=8 \
  cmake-build-release/terrain_ground_probe OUTPUT1080 REVIEW.json 1080 512 0
```

Both builds were built and tested using full suites plus focused reruns. The new
checks cover all sixteen stitch patterns, balanced complete coverage, allocation-free
selection, priority under pressure, frozen/cold bookmark restore, invalid bookmarks,
stationary GPU positions in both quality modes, shared curved edges, and depth/colour
identity. The final readiness-latch change also has explicit cancellation/re-entry
CPU coverage in both builds and a repeated Debug hierarchy GPU check. The cancellation
GPU fixture retains its real terrain payload and 32 KiB sliced transfers while using
a small framebuffer; its withdrawal step is outside the complete detail-demand range.
All 76 SPIR-V modules in each build passed `spirv-val --target-env vulkan1.3`.

[1080p wireframe](1080-distance-wireframe.png) and
[1080p textured view](1080-quality-textured.png) show the stationary mesh. No circular
compressed grid band is visible. Native motion at full coverage and RTX 3060
frame-time/visual acceptance remain unexecuted.

At 1080p the fixed 2,097,152-triangle partition budget limits further refinement:
620,944 triangles are selected in the viewing frustum, protected-ground shortfalls
are zero, and maximum estimated replacement error is 2.603 pixels in either quality
mode. Thus the one-pixel target is not guaranteed under resource pressure; distant
switches can still pop. This is explicit budget degradation, not continuous morphing.
Managed terrain storage is 514,692,012 GPU bytes and 58,598,224 host bytes in this
reduced setup and remains fixed through the review. Driver overhead is excluded.

`motion.csv` records CPU-selected triangles and the replacement estimate, not measured
pixel motion. CPU milliseconds include render-call fence waits. GPU milliseconds
refer to the most recently completed frame, not necessarily the current CSV row.
Software timings must not be extrapolated to the native GPU.

## Ground motion results

The final replay completed 198 motion frames plus settling/captures, with zero Vulkan validation errors and no managed allocation growth. All six return captures were pixel-identical to their respective starting captures. Protected-ground shortfalls stayed zero.

| Extra quality | Speed (m/s) | Selected triangles (min–max) | Maximum replacement estimate (px) |
| --- | ---: | ---: | ---: |
| Off | 2 | 619,440–624,048 | 1.300 |
| Off | 6 | 604,560–663,192 | 1.342 |
| Off | 30 | 572,328–675,248 | 1.351 |
| On | 2 | 619,440–624,048 | 1.300 |
| On | 6 | 604,560–663,192 | 1.342 |
| On | 30 | 572,328–675,248 | 1.351 |

Both modes spend the available refinement budget on transition safety in this view, so their geometry matches. This is intentional: optional quality cannot displace transition or protected-ground work.

[Per-frame records](motion.csv), [test target results](tests.csv), [motion configuration](motion-review.txt), and [1080p configuration](1080-review.txt) retain the measurements. Test results combine full suites and focused reruns; the final publication-latch change was checked by the hierarchy CPU tests in both builds, Debug hierarchy GPU validation, and this Release ground replay.

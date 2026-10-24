# Curvature precision correction — 2026-10-24

The native review reports a stable inner circle and wobbling triangles outside it,
including with LOD frozen and TAA disabled. The GPU curvature helper previously
switched at an angle of 1e-6 radians (6.36 m for the default planet) from a series
to `sin(a)/a`. Flat mesh/UV readbacks did not exercise this precision hazard.

The new owned fixture runs the production curvature position and normal helpers
through Vulkan. It sweeps 1/64-m steps through the former threshold in four
quadrants, distances through 1,048,576 m, both sides of the new 0.5-radian join,
and planet radii 1,000,000 / 6,360,000 / 100,000,000 m. It compares against CPU
double positions and independently differentiated normals. A second pass rounds
sine results to 1e-7, testing bounded absolute error without pretending to emulate
an NVIDIA implementation. Radial ordering, step continuity, disabled curvature,
and invalid scratch sizes are also checked.

| Measurement | Old formula | Correction |
| --- | ---: | ---: |
| Near displacement from controlled sine perturbation, default radius | 0.312531 m | 0 m |
| Maximum error against double reference within 256 m, default radius | not used as acceptance | 0.000006974 m |
| Precision regression | fails | passes |

Device: llvmpipe LLVM 23.1.3, eight threads. Vulkan validation enabled. Debug and
Release `curvature_gpu`, `horizon_gpu`, and `temporal_terrain_gpu` pass. Both builds'
77 SPIR-V modules pass `spirv-val --target-env vulkan1.3`. The old formula was
tested using an isolated shader directory; production binaries were not replaced.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json LP_NUM_THREADS=8 \
  ctest --test-dir cmake-build-debug \
  -R '^(curvature_gpu|horizon_gpu|temporal_terrain_gpu)$' --output-on-failure
```

Repeat with `cmake-build-release`. On native hardware the precision fixture can
also be run directly as `cmake-build-release/curvature_gpu_tests`.

The correction adds no frame resources, allocations, or CPU work. Native cost is
not measured. On 2026-10-24 the user confirmed that the rebuilt application on
RTX 3060 resolves the walking-speed wobble and apparent texture swimming.
The remaining CSM jagged edges and distant filtering are separate observations;
this confirmation is not a native performance measurement.

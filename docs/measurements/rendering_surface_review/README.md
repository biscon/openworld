# Terrain surface correction measurements

Recorded 2026-10-23, Release build, llvmpipe (LLVM 23.1.3, 256 bits), eight worker
threads, Vulkan validation enabled. These are short software comparisons, not
RTX 3060 performance acceptance. Probes ran sequentially without other GPU tests.

## Results

| Measurement | Before | After |
| --- | ---: | ---: |
| CSM scene GPU median, ms | 3.70185 | 3.71329 |
| CSM scene GPU p95, ms | 3.81813 | 3.95744 |
| Shadow raster p95, ms | 1.73105 | 1.70759 |
| Shadow casters / triangles | 33 / 40,970 | 33 / 40,970 |
| Shadow bytes | 3,253,152 | 3,253,152 |
| Visibility storage bytes | 30,508,544 | 34,702,848 |
| Hi-Z plain scene GPU median, ms | 3.58195 | 3.845663 |
| Hi-Z valley scene GPU median, ms | 4.008417 | 4.000261 |

Shadow overflow and incomplete counts were zero. The extra 4 MiB of visibility
storage is the additional depth/colour triangle counters, reserved at startup.
All modes in the small streamed visibility workload selected/submitted 67 patches
and rejected none through occlusion. The after counters report 8,576 terrain
triangles in both depth and colour when a prepass is enabled. This fixture therefore
does not demonstrate a culling improvement. A separate GPU regression with terrain
behind an opaque wall verifies colour rejection and image equivalence.

## Commands and baseline

The before executables and compiled shaders were copied outside the tree before
the correction. They already include the horizon implementation. The old visibility
probe had no shader-directory argument; an isolated executable copy had its embedded
shader-directory string redirected to the preserved old shader directory. The after
probe now accepts an explicit shader directory. No baseline source files were reset.

```sh
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
export LP_NUM_THREADS=8
# Run each command sequentially. Use the preserved executable/shaders for "before".
cmake-build-release/shadows_probe /tmp/shadows-after --quick --frames 16
cmake-build-release/visibility_probe /tmp/visibility-after --quick --frames 16 --streaming --require-validation
```

CSV files retain per-frame shadow timings and all visibility modes; metadata files
retain device, dimensions and settings. Each quick run uses 320×180 and 16 measured
frames. Software timing variance and the very small workload limit conclusions.

## Saved-view and motion checks

`view.json` is an owned small single-chunk fixture. Saving it at exit and restarting
with it produced byte-identical PPM captures (`replay-result.txt`). The recorded
material path is a reference; assets are disabled in these fixtures.

```sh
cmake-build-release/engine --headless --no-ui --single-chunk --terrain-cells 16 --no-horizon --no-material-assets --no-taa --no-bloom --render-height 64 --frames 4 --require-validation --capture /tmp/saved.ppm --save-review-view /tmp/view.json
cmake-build-release/engine --headless --no-ui --review-view /tmp/view.json --frames 4 --require-validation --capture /tmp/replayed.ppm
cmp /tmp/saved.ppm /tmp/replayed.ppm
cmake-build-release/shadows_probe /tmp/slow-shadow --review-view docs/measurements/rendering_surface_review/slow-review.json --flight-speed 1 --frames 32 --capture-every 16
cmake-build-release/visibility_probe /tmp/slow-wire --review-view docs/measurements/rendering_surface_review/wire-review.json --flight-speed 5 --frames 32 --require-validation
cmake-build-release/visibility_probe /tmp/topdown --review-view docs/measurements/rendering_surface_review/topdown-review.json --flight-speed 5 --frames 4 --require-validation
```

The shadow path moves X at 1 m/s and Z at −0.4 m/s. Visibility replay moves along
negative Z at 5 m/s. Wireframe intentionally falls back to direct draws. The frozen
top-down view was also replayed with only the old terrain vertex shader substituted
to isolate geometry from counters and fragment shading. Its dense wireframe is
undersampled at this height; it is a smoke check, not visual acceptance of the
reported circular band. Slow-flight fixtures are plain, nearly flat terrain and do
not reproduce the user's textured mountain camera position.

## Correctness checks

- Debug: 38/38 passed, 617.11 seconds; Release: 38/38 passed, 498.30 seconds.
- Final Debug focused rerun: 5/5 passed, 28.30 seconds, including the flat-stream
  parent-height regression and current review-file validation.
- 76 SPIR-V modules validated in each build for Vulkan 1.3.
- GPU checks and replay probes completed with Vulkan validation; no validation errors.
- Geometry, seam, curved endpoint, frozen LOD, grazing receiver, actual shadow
  occluder, terrain occlusion, counter fallback and atomic review-file failure cases
  are covered by deterministic owned test fixtures.

Native RTX 3060 visual acceptance, the original mountainous camera path and 1080p
frame-time targets remain unmeasured. The implementation preserves the full depth
prepass and global LOD budget, so high selected triangle counts can remain even when
opaque-colour occlusion works correctly.

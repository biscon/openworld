# 5J-V1 software Vulkan evidence

All runs use Release and llvmpipe (LLVM 23.1.3, 256 bits), on 2026-10-24. They are
correctness, capacity and visual-review records, **not target-GPU benchmarks**.
The sandbox hid NVIDIA device access. Subsequent runs outside it completed on the
RTX 3060: see the [native hardware report](rtx3060/README.md), separate from these
historical software runs. See also the [design and findings](../../rendering_vegetation_experiment.md).

Each directory contains settings (`metadata.txt`), completed per-frame records
(`frames.csv`), median/p95 results (`timings.csv`), on-minus-off deltas (`paired.csv`),
and calibration (`coverage.csv`). Difference of p95 values is labelled separately
from a percentile of paired frame differences. Coverage files were normalized to
add explicit flat/rolling landscape labels; measured values are unchanged.

| Directory | Purpose | Validation | Command arguments after output directory |
| --- | --- | --- | --- |
| [capacity](capacity/metadata.txt) | Maximum 524288-candidate population, 320×180 | Off | `--quick --side 256 --sequence close --representation blades --frames 4 --no-captures` |
| [review](review/metadata.txt) | 128 m meadow, 640×360, full 2048-pixel CSM maps | Off | `--side 128 --width 640 --height 360 --frames 4 --warmup 4 --repeats 1 --sequence close` |
| [all_views](all_views/metadata.txt) | Close/grazing/moving/aerial/rolling paths, both fixture landscapes | Off | `--quick` |
| [moving_validation](moving_validation/metadata.txt) | Fixed binding path; shadow/TAA/wind/sun/prepass/GTAO variants | On | `--quick --sequence moving --variants --fixed --require-validation` |

Commands used `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` and
`cmake-build-release/vegetation_probe`. These short runs and software timings do
not support statistical performance acceptance. Debug/Release correctness tests,
including the bindless path, run separately with required synchronization validation.

The validation-disabled records have zero C++ allocation calls inside `render()`.
Validation-enabled allocation counts include validation-layer bookkeeping. GPU
memory figures charge actual Vulkan allocation requirements including staging and
diagnostic resources; existing shared render targets are not charged again.

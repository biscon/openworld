# 5J-V1: vegetation representation experiment

Status: implemented and checked in Debug/Release with software Vulkan on 2026-10-24.
Native RTX 3060 measurements and synchronization validation are recorded in the
[hardware report](measurements/vegetation_5j_v1/rtx3060/README.md). Final
density/appearance and full-scene acceptance remain open.

This implements the automated-probe-only plan agreed for [5J-V1](rendering_vegetation.md).
A fixed owned meadow compares blade tufts, trimmed cards and a blade-to-card distance
transition inside the existing renderer. No interactive demo, streaming, biomes,
player interaction or general asset framework is introduced.

## Interfaces and ownership

`VegetationFixture` owns saved mesh/texture assets and deterministic compact candidate
records. A build-time tool produces the library; runtime loads it. The renderer's
optional startup configuration enables `VulkanVegetation`; normal applications allocate
nothing for it. Frame settings select representation, density, distances and explicit
animation time. Completed statistics and diagnostic snapshots are observation only.

Instances are 32-byte records, grouped into 4 m cells. The maximum owned fixture is
256 m square at eight tufts/m² (524288 candidates). Immutable geometry and instances
are uploaded once. Two fence-owned frame slots hold uniforms, GPU count/prefix/scatter
work, indirect commands, six compact lists (camera plus five cascades), and readback.
Each candidate occupies at most one bin per view. Fixed indirect commands have zero
firstInstance; a pushed bin index selects the compact-list offset. No optional
multi-draw/count feature, per-tuft CPU objects or CPU visibility readback is required.

All buffer/image allocation requirements, retained staging and frame copies count
against a 128 MiB cap. Invalid configurations fail explicitly. Writes are bounded;
overflow invalidates a measurement. Resources live until renderer destruction after
GPU completion. Frame resources are rewritten only after their existing renderer
fence. History advances on successful submission, not frame preparation.

## Passes and representation

Shared vertex deformation evaluates root-anchored broad wind and tip motion. Current
and previous submitted state produce motion; incompatible LOD/representation changes
reject correspondence. Colour/depth/shadows share alpha and coverage masks. Culling
bounds include maximum wind displacement. Existing double-sided materials, PBR,
CSM, TAA and atmospheric transport are reused; transmission remains V2.

GPU selection precedes shadow rasterization. Bounded external caster bounds contribute
to CSM depth fitting without ordinary per-tuft shadow records. Camera and caster lists
are independent. Vegetation participates in existing depth and colour passes, including
depth consumed by GTAO. V1 does not perform Hi-Z rejection or change pyramid scheduling.

Starting ranges are 20/45/70 m LOD, 55–70 m fade, and 20–25 m shadow fade. Stable hash
selection transitions LOD/density without duplicate geometry. Cards and blades share
source silhouettes and comparable material settings. Coverage-preserving alpha mips
and colour padding prevent an unfair card comparison.

## Probe and acceptance

The offscreen probe saves deterministic close, grazing, travel/turn, aerial and rolling
views. Coverage diagnostics precede timing and fixed calibration is recorded. Matched
count and matched coverage comparisons remain distinct. Full runs use Release, 1080p,
120 warmup + 600 measured frames, deterministic 1/120 s animation, three paired off/on
repetitions with reversed order. Captures and blocking diagnostics are outside timing.
Record completed frame identities, CPU total/preparation, GPU total and vegetation
selection/depth/colour/shadows, median/p95, candidate/triangle counts, overflow,
allocation calls, actual bytes and all effective settings. Software Vulkan is correctness
evidence, never RTX 3060 budget acceptance. Initial load/upload is measured separately;
this fixture does not establish streaming performance.

Tests cover deterministic assets/population, triangle attachment, mip coverage, bounded
writes and capacities, independent shadow visibility, consistent masks and wind,
submitted history, frame reuse, material bindings and synchronization validation.
Target gates remain approximately 1.5 ms incremental GPU, 0.25 ms CPU preparation and
128 MiB, with later streaming/interaction costs explicitly excluded from this slice.
Findings must distinguish implementation, observed correctness and unverified budgets.

## Historical V1 ABI and experiment controls

The layout below describes the measured V1 implementation. V2 adapts these same
fixtures into its [shared foliage backend](rendering_vegetation_assets.md): 64-byte
vertices, 2976-byte uniforms and 385-word per-view work regions, with twelve active
legacy bins. V1 geometry, blade/card policies and probe defaults remain available;
current runs are compatibility checks, not the original measured binary.
The general settings default now disables grass casting; the V1 probe explicitly
requests its historical 25 m casting comparison.

The startup boundary is `RendererOptions::vegetation`; `SceneSettings::vegetation`
selects blades (0), cards (1), or hybrid (2), density, card calibration, time and wind.
`Renderer::vegetationStats()` returns a fence-completed record with a submission ID.
`readVegetationSnapshot()` is explicitly blocking and copies compact lists only for
inspection. The normal application leaves the startup fixture null.

The combined geometry buffer contains twelve bins: two tuft variants × two
representations × three LODs. A bin has immutable index count/offset and material ID.
The 704-byte frame uniform holds six matrices, current/previous submitted camera,
wind and selection parameters, capacities and bin metadata. Each view's 97-word work
region contains twelve eight-word entries (five Vulkan indirect-command words,
count, scatter cursor and compact-list offset), then an overflow counter. Prefix
sum clamps indirect counts before scatter; exhausted lists cannot write past their
allocation. The diagnostic overflow configuration deliberately invalidates a run.

Every group owns 128 candidates in a 4 m cell. CPU work traverses at most 4096 cell
bounds for shadow fitting, never individual plants. Selection uses the camera's
conservative unjittered projection and independent finalized CSM matrices. Shared
world-anchored wind displaces tips, transforms normals using the bend derivative,
and leaves roots fixed. The envelope is validated against loaded mesh vertices and
instance scales. Camera/depth/shadow fragment masks use the same plant-local hash.

The fixture builder uses the source recipe in `src/vegetation.cpp`; its standalone
CMake target depends only on CPU asset code, so renderer/shader edits do not rebuild
artwork. Its versioned binary mesh cache is a local build artifact, not a portable
production asset format. Cards use uncompressed RGBA with preserved alpha coverage;
compression and a general material importer belong to V2. Full blade LODs contain
12/6/3 blades; cards use 4/2/1 vertical segments. A 256 m meadow has 524288 candidates,
with density 4 selecting a stable half of the eight-per-square-metre population.

The reported GPU allocation number is the peak committed buffer/image requirements,
including retained instance/geometry upload staging, two frame lists, and the
preallocated diagnostic readback buffer. Texture upload staging contributes its
peak even after the shared material system retires it. Existing shared colour,
depth, TAA and CSM targets are not charged a second time. Driver-private pipeline
and descriptor bookkeeping is not exposed as Vulkan memory requirements.

## Running the experiment

```sh
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DOPENWORLD_GPU_TESTS=ON
cmake --build cmake-build-release --target vegetation_probe vegetation_cpu_tests vegetation_gpu_tests
ctest --test-dir cmake-build-release -R '^vegetation_' --output-on-failure

# Small correctness/capture run. Never use this preset for target-GPU acceptance.
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/vegetation_probe /tmp/vegetation-review --quick --require-validation

# Native GPU: all camera paths, three alternating paired repetitions, 1080p.
cmake-build-release/vegetation_probe /tmp/vegetation-full
# Targeted integration comparisons (shadows/TAA/wind/sun/prepass/GTAO).
cmake-build-release/vegetation_probe /tmp/vegetation-variants --sequence moving --variants
```

`--sequence` accepts close/grazing/moving/aerial/rolling/all; `--representation`
accepts blades/cards/hybrid/all. `--density 4|8`, `--side`, `--width`, `--height`,
`--frames`, `--warmup`, `--repeats`, `--fixed`, and `--no-captures` provide focused
runs. Put `--quick` before explicit overrides. Quick mode uses a 16 m fixture,
320×180, 128-pixel CSM maps, four warmup frames and eight samples. Full mode uses
256 m, 1920×1080, 2048-pixel maps, 120 warmup frames and 600 samples. Neither mode
uses presentation throttling. The full matrix is intentionally not a routine test.

Outputs include `frames.csv` (completed-frame IDs), `timings.csv`, `paired.csv`
(on-minus-off median and p95 differences, not a percentile of per-frame differences),
`coverage.csv`, `metadata.txt`, PNG captures, numbered motion frames and motion/history
diagnostics. Missing timestamps use -1 and an availability flag, never a synthetic
zero timing. C++ allocation counts include validation-layer calls during render;
use validation-disabled Release runs to assess steady-state engine allocation.

Coverage calibration measures white grass on black ground in sixteen screen tiles
across close/grazing/aerial views, sweeping fixed card width/density multipliers.
The chosen pair minimizes mean tile error, then stays fixed throughout timing.
A five-percentage-point aggregate gate is only a diagnostic: it does not certify
individual leaf silhouettes, lighting or movement. Matched-count results are kept
alongside calibrated results. Calibration and captures use blocking diagnostics
outside timed intervals. A separate pair of drain submissions retrieves the last
two timed frames; CPU and GPU samples retain their matching submission identity.

## Verification and findings (2026-10-24)

Implemented V1 is an experiment harness, not production vegetation. The initial
sandbox hid the NVIDIA device nodes; this was not a host driver failure. Running
outside that sandbox enabled native RTX 3060 validation and benchmarking; see the
[hardware report](measurements/vegetation_5j_v1/rtx3060/README.md). The historical
checks below use llvmpipe (LLVM 23.1.3, 256 bits), whose timings are not native-GPU
performance evidence.

- Vegetation CPU/GPU tests pass in Debug and Release. GPU cases require Vulkan
  synchronization validation and cover both fixed and available bindless materials.
  They include zero/exact/overflow capacity, invalid budgets, stable density subsets,
  actual middle/far hybrid bins, depth/colour mask agreement, independent offscreen
  caster lists, wind motion vectors, history reset and frame reuse.
- Existing temporal, material, scene, visibility, GTAO and shadow GPU regressions
  pass (eight selected Debug tests including vegetation CPU/GPU). The ordinary
  application builds successfully with the vegetation fixture disabled by default.
- All eight vegetation SPIR-V modules pass `spirv-val --target-env vulkan1.3`.
  Rebuilding `vegetation_fixture_assets` with unchanged inputs reports no work;
  changing the source recipe regenerated its mesh/texture outputs during development.
- The 256 m / 524288-candidate capacity run completed without overflow. Peak charged
  vegetation allocations were **72,332,180 bytes (69.0 MiB)** on this driver, including
  retained staging and diagnostic storage. One ground view selected 18172 tufts and
  647148 camera triangles; cascade lists contain repeated casters by design.
- Validation-disabled Release capacity/review/all-view runs recorded **zero C++
  allocation calls inside `render()`**. Validation-enabled runs also count layer
  bookkeeping, so their larger allocation counts do not indicate engine heap traffic.
- These are reduced-resolution, short software-Vulkan correctness/capacity runs.
  The subsequent native benchmark uses 1080p, 120 warmup/600 sample frames and
  three alternating paired repetitions. Neither fixture establishes remaining
  full-scene tree/water headroom or final reference-density coverage.

Retain **blade tufts as the provisional default**. The reviewed cards form more
block-like clumps and hide changes in silhouette behind similar aggregate coverage.
A distant-card stage has not earned production adoption. The hybrid path remains available for longer-distance visual review. Native
measurements show no median performance win for the tested calibrated cards or
hybrid, with some overlapping repeat ranges; this is not a universal result for
all possible card assets or implementations.

The initial fixture still has conspicuous ground gaps at close range. It uses a
plain ground material and conventional two-sided PBR without leaf transmission;
its dark, sparse-looking clumps do **not** establish the reference images' final
appearance or continuous ground coverage. V2 asset/shading work must revisit
coverage and repeat this experiment before treating these densities or distances
as accepted settings. Coverage calibration alone cannot settle this tradeoff.

### Recorded evidence

[Measurement index and exact commands](measurements/vegetation_5j_v1/README.md)
links raw frame records, paired results, calibration and settings. The probes remain
reproducible; small fixtures/short samples are explicitly labelled in metadata.

| View | Blade tufts | Calibrated cards | Hybrid |
| --- | --- | --- | --- |
| Close, 640×360 / 2048 CSM | [blades](images/vegetation_5j_v1/close_blades_count.png) | [cards](images/vegetation_5j_v1/close_cards_coverage.png) | [hybrid](images/vegetation_5j_v1/close_hybrid_coverage.png) |
| Grazing, quick preset | [blades](images/vegetation_5j_v1/grazing_blades_count.png) | [cards](images/vegetation_5j_v1/grazing_cards_coverage.png) | [hybrid](images/vegetation_5j_v1/grazing_hybrid_coverage.png) |
| Aerial, quick preset | [blades](images/vegetation_5j_v1/aerial_blades_count.png) | [cards](images/vegetation_5j_v1/aerial_cards_coverage.png) | [hybrid](images/vegetation_5j_v1/aerial_hybrid_coverage.png) |
| Rolling ground, quick preset | [blades](images/vegetation_5j_v1/rolling_blades_count.png) | [cards](images/vegetation_5j_v1/rolling_cards_coverage.png) | [hybrid](images/vegetation_5j_v1/rolling_hybrid_coverage.png) |

The checked-in numbered moving sequences run from `motion_000` through `motion_003`:
[blade start](images/vegetation_5j_v1/moving_blades_count_motion_000.png),
[blade end](images/vegetation_5j_v1/moving_blades_count_motion_003.png),
[card start](images/vegetation_5j_v1/moving_cards_coverage_motion_000.png),
[card end](images/vegetation_5j_v1/moving_cards_coverage_motion_003.png).
Also inspect [motion vectors](images/vegetation_5j_v1/moving_blades_count_motion_vectors.png)
and [history rejection](images/vegetation_5j_v1/moving_blades_count_history_rejection.png).
These very short quick-preset sequences establish diagnostic operation, not a final
motion-quality sign-off; the full probe writes longer 24-frame sequences.


### Receive-only diagnostic extension

The native investigation separates grass casting from receiving with the probe's
`--grass-shadow-distance 0` option. CSM remains enabled for terrain and other objects;
grass still calls the existing shadow-receiving shader. `--gtao off|half|full` selects
existing GTAO without changing engine quality defaults. An optional `--canopy-fixture`
adds one owned opaque horizontal quad above/outside the starting camera view, casting
a broad shadow onto visible grass. It is a shadow receiver diagnostic, not tree content.
It is off for the main performance comparisons and is recorded in capture metadata.

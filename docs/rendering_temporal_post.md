# Temporal antialiasing and HDR bloom (slice 5G)

Status: implemented; verification results are recorded below. Target-GPU and live
windowed acceptance remain separate gates. Auto-exposure, temporal upscaling and sharpening are deferred.
[Slice 5I](rendering_volumetrics.md) adds independent fog source history and feeds
its rejection into final-image reactivity. [Slice 5H](rendering_atmosphere.md)
adds sky reprojection to the existing TAA history.

## Colour, resolution and controls

The demo enables TAA and bloom intensity 0.05. Library `SceneSettings` defaults
remain off for deterministic existing callers. `TaaSettings` exposes enable and
maximum history weight (0–0.9); `BloomSettings` exposes intensity (0–2), scene
threshold (0–1e6), fractional knee (0–1) and filter radius (0.5–2). Invalid/nonfinite
settings are rejected. Initial bloom threshold/knee/radius are 10000/0.5/1.

Processing is scene-resolution linear Rec.709 pre-exposed HDR, then TAA, bloom,
output scaling/ACES or AgX, and native UI. Bloom is added in the existing output
pass without a full-resolution composite image. Neither stage reapplies exposure.
Zero bloom intensity skips its dispatches and preserves the existing output path.
`--no-taa`, `--no-bloom`, `--bloom-intensity N` and the lighting-window controls
provide independent comparisons. The UI reports effective stages, readable
fallback/reset reasons, memory, completed timings and rejected/overflow records.

Raw `readHdrSnapshot()` remains unchanged. `readPostSnapshot()` is a blocking
observer, returning resolved colour, motion, reactivity, signed history depth,
reconstructed bloom levels and dimensions. Negative history depth marks rejected
history; magnitude is view-space metres. These buffers never feed CPU rendering
choices. Slice 5H writes rotation-only sky motion (W=2) and permits background-to-
background history when depth remains zero. Geometry motion retains W=1 and depth
validation. The synthetic clear colour with atmosphere Off does not accumulate history.

## Submitted-frame state and geometry

Eight Halton(2,3) offsets remain within half a scene pixel. UV is top-left oriented;
velocity is previous **unjittered** UV minus current unjittered UV. Raster projection
adds jitter, and resolve samples current raster colour at output UV plus jitter/extent.
History is on the unjittered output grid. Previous state and sequence advance only
on successful queue submission, including when presentation subsequently requests
recreation. Skipped acquisition does not advance them.

Mesh history uses generation-checked instance identities and last submitted poses,
independent of dense record ordering. Multiple CPU pose updates do not overwrite
that history. Replacement mesh/material identities reject correspondence. Call
`invalidateInstanceHistory(handle)` for object teleports. Camera origins retain
integer cells plus double local coordinates before conversion to relative floats.

Terrain previous/current records live in preallocated contiguous arrays. A compact
32-bit index array is sorted by surface key at submission and queried with binary
search during preparation; 432-byte records stay in append order. Cost remains
O(n log n), with no allocator traffic. Keys include world/slot
identity, generation/content revision, patch coordinates, topology, LOD and relevant
residency/detail state. The original 128-byte draw ABI remains unchanged. A separate
368-byte GPU record stores previous draw constants, blend weights, observer and
camera. Dense and streamed vertex shaders evaluate previous geometry using saved
morph state and unchanged resident heights. New/replaced topology rejects history
locally; stale buffers are never retained or read for rejected correspondence.
Dense generation keys use the revision being recorded, not only the last committed
revision. Normal/detail identity changes conservatively reject affected history.

Tracking is bounded by visibility capacity, further reduced to the actual maximum
draw count for non-streamed terrain. Overflow rejects temporal correspondence,
reports a counter and keeps direct rendering intact. Mesh storage follows instance
capacity. The streaming terrain tracker uses at most two arrays of 432-byte CPU
entries, one 32-bit index per slot and two GPU record buffers; at 65536 entries
these are about 54.25 MiB CPU and 46 MiB GPU. Residency budgets and world/cache
identities are unchanged.

`resetTemporalHistory()` handles explicit camera cuts. Automatic cuts exceed 256 m
translation or 45 degrees rotation between submitted frames. Scene extent changes,
post resource reconfiguration, world replacement and entering/leaving diagnostic
rendering reset history. Rebase-only changes, manual exposure changes and output-only
resize with unchanged scene extent retain it. Tone mapping remains outside history.
The current implementation recreates both stages at a post enable/disable boundary;
this deliberately also resets TAA when bloom allocation is toggled. Intensity/radius/
threshold changes that keep bloom enabled require no recreation.

## Resolve and neighbouring systems

TAA reconstructs current colour with a positive 3x3 tent, selects nearest-depth
neighbourhood motion, and samples history with nine-tap bicubic filtering. It rejects
invalid/outside/depth-mismatched history, rescales previous RGB by current/previous
pre-exposure, and clips in a 3x3 YCoCg variance box. Depth tolerance is the larger
of 2 cm and 1% of expected previous view depth, including FP16 motion-depth precision.
History weight is bounded by 0.9 and reduced by motion, reactive coverage and
luminance disagreement. Calculations use FP32; finite RGBA16F output uses the existing
65504 representable limit. Signed R32F history depth doubles as rejection diagnostics.
The [continuous-horizon renderer](rendering_horizon.md) stores motion depth in
kilometres internally and converts it back to metres for history validation and
public snapshots. It uses interpolated rendered positions for motion, with separate
flat geographic positions for materials, and retains each frame’s curvature mapping.

Algorithm reference: [Intel TAA](https://github.com/GameTechDev/TAA). The implementation
is engine-owned HLSL; no upstream application or copied shader source is included.
History stays linear/pre-exposed rather than using the reference's tone-mapped mode.

Opaque/masked colour writes motion and reactivity, including the equal-depth path.
Transparency leaves opaque motion intact and combines reactivity with MAX blending.
Reactive strength uses opacity and scene-referred emission; history alpha carries
previous reactivity so disappearing sources also reduce accumulation. Overlapping
transparent layers cannot all be reprojected by one opaque vector. Conservative
reduced history is an intentional limitation, not exact transparent motion.

Depth and colour use identical jitter. TAA retains sampled D32 through the final
colour pass but does not alone force a prepass. CPU terrain selection uses an
unjittered frustum expanded by the half-pixel envelope; Hi-Z/GPU visibility uses
current raster projection and existing conservative bounds. Shadow stabilization
remains unjittered. Cluster assignment and lookup already both project actual
geometry into the same unjittered cluster grid; pixels in the jitter fringe use the
existing brute-force fallback, so cluster storage needs no per-frame rebuild.

GTAO subtracts jitter during view-position reconstruction and rotates sample noise
only when effective TAA resolves it. TAA-off and diagnostic views retain fixed
spatial noise. No separate AO history is introduced. Legacy, material, AO, shadow
and geometry diagnostic paths bypass temporal post processing and projection jitter.

## Bloom and GPU ownership

Bloom uses at most six separate ceil-halved levels, starting at half scene resolution
and stopping at 1x1. A filtered overlapping downsample covers odd tails and clamps
screen boundaries. The first level extracts a luminance soft-knee threshold in
pre-exposed units. Normalized scene-referred luminance weights suppress isolated
fireflies without a new hard highlight clamp; they deliberately attenuate isolated
very bright sources more than broad highlights. Subsequent levels use ordinary
filtered downsampling. Normalized tent reconstruction preserves constant fields;
radius scales its footprint. Both opaque and transparent emission contribute.

`VulkanPost` coordinates independently budgeted TAA and bloom resource groups,
descriptors, shaders and timestamp queries. TAA owns two RGBA16F motion targets,
R8 reactivity targets, RGBA16F history targets and R32F depth histories. Bloom owns
two frame slots of RGBA16F downsample/reconstruction chains. Per-pixel image payload
is approximately 42 bytes/scene pixel for TAA and 10.67 for bloom, plus driver padding
and temporal record buffers. TAA and bloom budgets are 512/128 MiB respectively.
Sizing queries precede allocation. Driver-reported image/buffer sizes are reported;
small descriptor-valid metadata buffers remain allocated even when disabled or below
minimum budget. Driver-internal descriptor/query allocation is not included.
Scene-owned instance uploads are accounted separately under the fixed instance
capacity: each GPU instance record grows from 144 to 208 bytes for its previous
transform, with bounded CPU pose snapshots. They are not included in post-stage
memory counters or budgets.

Sampled/storage/attachment formats, independent blending and storage-buffer range
are checked. Unsupported features or budget exhaustion disable the affected stage
with a reason; allocation errors propagate. No async compute or additional ordinary
frame wait is added. Current frame resources are reused after their existing fence;
explicit image dependencies order immediate-previous history reads and subsequent
reuse across submissions on the graphics queue. Descriptor updates target fence-safe
slots. Recreation uses the existing device-idle boundary. Snapshot availability is
invalidated when resources change.

Each frame's fixed timestamp set measures TAA and each bloom down/up level. Timing
collection never waits and reports availability. Ordinary frame work uses preallocated
containers and mapped buffers. UI rendering is outside both effects.

## Verification and reproduction

`temporal_cpu`, `temporal_gpu` and `temporal_terrain_gpu` use owned fixtures. They
cover projection/UV convention, bounded tracking, unsubmitted-state handling, handle
reuse, pose updates, rebase, exposure, explicit resets, supersampled convergence,
transparency/disappearance, masks, bloom constant fields, single-pixel emissive
ramps and screen-edge impulses, odd/tiny extents, budgets,
resolution changes, diagnostics, dense/flat/hierarchical terrain, morphing and
replacement. Combined tests exercise Direct/Indirect/Hi-Z, prepass, GTAO Half/Full,
fixed materials and cascaded shadows under synchronization validation. The scene
resolution fixture verifies that native UI output is unchanged and absent from
history/bloom. It also exercises eight-byte snapshot-region alignment after odd-sized
R8/R32 images, covering a Vulkan validation regression found during implementation.

The independent static fixture compares a slanted triangle against 8x8 supersampling.
Its summed squared red-channel error is 2.35063 with TAA off and 1.24003 after 24 TAA
frames (about 47% lower). This demonstrates that fixture's edge improvement, not
universal motion quality or a guarantee about textured terrain.

```sh
cmake --build cmake-build-release --target engine temporal_probe temporal_gpu_tests temporal_terrain_gpu_tests
ctest --test-dir cmake-build-release -R '^temporal_' --output-on-failure
cmake-build-release/temporal_probe /tmp/5g-1080 --require-validation
cmake-build-release/temporal_probe /tmp/5g-4k --width 3840 --height 2160 --require-validation
cmake-build-release/temporal_probe /tmp/5g-streaming --streaming --require-validation
# Software correctness/instrumentation only:
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json cmake-build-release/temporal_probe /tmp/5g-quick --quick --require-validation
```

The probe warms each mode for 16 frames, follows the same deterministic camera/object
path for Off/TAA/bloom/combined, and writes median/p95 CPU/total GPU/stage costs,
per-level bloom timing CSVs, post-owned image/buffer memory, and colour/motion/reactivity/
rejection/bloom captures. A global C++ allocation counter brackets `render()` only;
it includes validation/driver C++ allocations and excludes malloc. Capture/readback
and sample-container growth occur outside measurement. Streaming probes report
whether their bounded 60-second settling period completed.

Verified 2026-10-23 with llvmpipe (LLVM 23.1.3):

- Full Debug and Release builds succeeded. Both full CTest suites passed 34/34.
  After the snapshot alignment fix, all four temporal/resolution checks passed
  again in both builds; the final impulse/emissive-ramp additions also passed in
  both builds. GPU fixtures require validation, including synchronization validation.
- All 36 affected SPIR-V modules across the two builds passed `spirv-val` against
  Vulkan 1.3. The final validation-enabled Release probe completed without messages.
- At 320x180, 16 warmup plus 16 measured frames per mode, the validation-disabled
  Release probe recorded **zero C++ new/new[] calls inside render in every mode**.
  This excludes malloc and does not establish allocator behaviour for all scenes.
  Validation-enabled runs include layer/driver allocations and are saved separately.

| Software probe mode | Frame GPU median / p95 (ms) | TAA median (ms) | Bloom median (ms) | Post memory (bytes) |
| --- | --- | --- | --- | --- |
| Off | 2.819 / 3.013 | 0 | 0 | 1,216 |
| TAA | 3.819 / 4.316 | 0.503 | 0 | 2,558,784 |
| Bloom | 4.125 / 4.580 | 0 | 1.321 | 640,192 |
| Combined | 4.862 / 5.355 | 0.462 | 1.314 | 3,197,760 |

These are software instrumentation checks, not RTX 3060 performance results.
Raw [timings](measurements/rendering_5g/performance_timings.csv),
[per-level costs](measurements/rendering_5g/performance_bloom_passes.csv),
[run metadata](measurements/rendering_5g/performance_metadata.txt), and
[validation-enabled timings](measurements/rendering_5g/validation_timings.csv) are retained.
Captures include [Off](measurements/rendering_5g/off.png),
[combined](measurements/rendering_5g/combined.png),
[motion](measurements/rendering_5g/combined_motion.png),
[reactivity](measurements/rendering_5g/combined_reactive.png),
[history rejection](measurements/rendering_5g/combined_rejection.png) and
[bloom](measurements/rendering_5g/combined_bloom_0.png).

Pending: native/1080p/4K target-GPU timings, representative streaming allocation and
frame-spike measurements, and live motion review of terrain detail, fast turns,
disocclusion, transparent trails and bloom radius/intensity. NVIDIA driver communication
and a window display were unavailable in this environment. Software tests and the
small static convergence fixture do not satisfy those hardware/visual gates.

## Thin-geometry coverage

Implemented. The owned fixture reproduces static thin-grass boiling under the
jitter sequence with zero unjittered motion, no wind and no AO. Ordinary materials keep the
existing resolve. `MeshMaterial::thinGeometry` opts opaque/masked surfaces into
coverage handling; grass/leaf materials opt in without changing saved asset formats.
Motion W explicitly encodes ordinary invalid/valid (0/1), sky (2), thin valid (3)
and thin invalid (-1). Public snapshots decode the usual validity and expose a
separate classification vector. No additional geometry render target is needed.

One compute pass before resolve estimates 3x3 thin coverage, motion consistency and
depth bounds, reprojects previous coverage and accumulates with a 0.875 history
weight. Confidence ramps over eight valid observations. Complete misses in individual
jitter phases may retain validated partial-coverage history; foreground depth and
reactivity still reject it. Sky is explicitly the far endpoint of canopy intervals.
Two frame-owned RGBA16F images store coverage, confidence and depth bounds in km
(about 32 MiB at 1080p). Validated partial coverage locally relaxes colour clipping
and luminance disagreement; neighbourhood-supported depth alternation may retain
history, while invalid motion, occluders, disocclusion and reactive overlays suppress
assistance. Global history weight and colour reconstruction remain unchanged.
The feature follows submitted-frame history and existing reset/fence/resize rules.
Actual allocations count against the TAA budget; insufficient extra budget falls
back to ordinary TAA. `TaaSettings::thinGeometry` defaults on, including old bookmarks,
and has one comparison checkbox beside TAA. No steady-state CPU allocation/readback.

Verification is limited to the owned temporal/foliage fixtures, bookmark compatibility,
shader checks, native Vulkan validation and one main-engine visual review. The static
fixture compares 32 measured frames after 32 warmup frames against ordinary TAA and
an 8x supersampled reference: target at least 50% less temporal variance and no more
than 5% worse reference error. Moving leaves/camera, opaque occlusion, resets and
solid surfaces guard against ghosting and global blur. The existing probe provides `--engine-scene --distance 80 --taa-check` for one
short timing pair and eight successive static captures per setting. Readbacks occur
outside measured intervals. This is a focused adaptation of
[coverage-aware rejection](https://dev.epicgames.com/documentation/unreal-engine/thin-geometry-detection-with-temporal-super-resolution),
not the complete TSR edge/line detection pipeline.


Native RTX 3060 verification (2026-10-24): Debug/Release temporal and bookmark
checks pass, as do Release foliage and streaming integration checks; all 95 shader
modules validate in both builds. The static fixture variance falls from 0.002407
to 0.000979 (59%), while reference MSE improves from 0.004703 to 0.004634. Untagged
solid-edge output agrees within 1e-6. Tests cover moving surfaces/camera, foreground
occlusion, history reset, resizing, masked fixed-binding materials, canopy gaps
against sky and extra-budget fallback. The user's saved engine view, replayed with
80 m vegetation, zero wind and GTAO off, settles with zero validation errors.

The single 1080p Release pair uses 32 warmup + 120 measured frames per setting:
TAA median/p95 is 0.350/0.355 ms ordinary and 0.627/0.851 ms with coverage handling
(+0.277 ms median). Actual added allocations are 33.75 MiB, with zero render-thread
C++ allocation calls in either measured interval. Eight-frame production captures
show 27–48% lower display-space temporal variance across the sampled grass bands;
this is reduced shimmer, not elimination of every dense subpixel artifact. Coverage
classification cannot distinguish every variation between overlapping blades of the
same class. Full TSR contrast-line detection remains outside this correction.


## Spatial antialiasing

Status: implemented and natively verified. `AntialiasingMode` selects Off,
FXAA, SMAA 1x or TAA; new engine sessions default to TAA (restored after the grass LOD stability update). Library scene
defaults stay Off for explicit fixtures. TAA tuning remains separate; only TAA
enables jitter, motion attachments and temporal history. Switching back resets
history. Grass geometry/shading and further temporal improvements are deferred.

Renderer-owned `VulkanSpatialAA` composites bloom and tone maps at scene
resolution into RGBA8, then runs FXAA reference preset 3 or SMAA 1x High.
SMAA uses official luma edges, blending weights and neighborhood blending, with
vendored Area/Search textures and no temporal subsamples or predication. Encoded
UNORM reads serve FXAA and edge detection; sRGB views decode for SMAA linear
blending and final output scaling. Presentation encodes once, then draws native
UI. Off/TAA retain the fused output path; diagnostic views bypass spatial AA.

Two frame slots own reusable source/result images; SMAA additionally owns RG8
edges and RGBA8 weights. Immutable lookup images are shared. Only the selected
mode is allocated, at the existing idle reconfiguration boundary. The 256 MiB
startup budget counts actual image allocations and lookup upload storage;
unsupported formats or insufficient budget fall back to Off with a visible
reason. No steady-state allocation or additional frame wait is introduced.
GPU timestamps include tone-map preparation and filtering.

The Output selector exposes all modes and TAA-only tuning. CLI `--aa` accepts
`off|fxaa|smaa|taa`; `--no-taa` aliases Off. Last explicit AA argument wins and
overrides a loaded bookmark. Review format v4 stores the enum; v1–v3 map legacy
`taa.enabled` to TAA/Off. Existing renderer startup budgets receive defaults
when migrating old files.

Validation: owned small GPU fixtures cover thin/diagonal edges, uniform color,
gamma, frozen stability, tiny/odd extents, resize, mode transitions and budget
fallback; existing temporal/bookmark regression tests and synchronization
validation protect integration. One engine grass view compares frozen/wind/pan
behavior and one short warmed 1080p run compares GPU time and memory. No new
viewer or broad benchmark suite.

Verification (RTX 3060): Debug/Release builds, new spatial GPU fixtures, existing
temporal/resolution/bookmark checks and new SPIR-V validation pass. Native Vulkan
synchronization validation reports no errors. Both spatial modes preserve flat
colors within one UNORM step under ACES/AgX and bloom, and are pixel-identical
across repeated frozen frames. Legacy v1–v3 AA choices and v4 round trips pass. Actual engine startup checks
confirmed the then-current SMAA default, explicit CLI precedence over a loaded bookmark and the
`--no-taa` Off alias, with native UI rendering and zero validation errors.

One warmed 1920×1080 engine grass comparison (80 m, 32 warm-up + 120 sampled
frames per mode) measured these filter costs; spatial times include tone-map
preparation, while TAA time is its resolve/coverage passes:

| Mode | Median / p95 GPU ms | AA allocations MiB | Full-frame median ms |
| --- | ---: | ---: | ---: |
| Off | 0 / 0 | 0.001 | 5.565 |
| FXAA | 0.215 / 0.219 | 33.751 | 5.688 |
| SMAA 1x High | 0.308 / 0.460 | 59.432 | 5.867 |
| TAA + thin coverage | 0.627 / 0.640 | 168.344 | 7.592 |

All four modes recorded zero steady-state render-thread C++ allocation calls.
Off's tiny allocation is shared view/minimal terrain-record buffers, not history.
Eight frozen grass captures were identical for Off/FXAA/SMAA; TAA retained
frame-to-frame variation. Wind/pan captures render correctly; SMAA preserves
sharper fine detail than FXAA. Spatial AA does not reconstruct missing subpixel
geometry or guarantee stability during movement. Grass LOD/shading changes and
further TAA changes remain deferred. `vegetation_stream_probe --aa-check` runs
this focused comparison through the existing renderer, without a new viewer.

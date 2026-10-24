# Vegetation attachment continuity — 2026-10-25

The reported moving-camera patch flicker reproduced as whole cells disappearing
while terrain attachment repairs waited for the bounded update queue. The two
frame-owned attachment copies could require separate repairs. Range fades and
candidate identities were not the cause of those visibility gaps.

## Paired route

RTX 3060, Release, existing engine-scene probe, 1280×720, 96 m vegetation range,
seed 173, blended habitats, TAA, wind, CSM and physical atmosphere. Start from settled
terrain/population, move forward 0.1 m per submitted frame for 360 frames, then stop
for 120 frames (12 m/s at the probe's 120 Hz simulation step). The trace inspects populated cells in a forward cone, at least 4 m
inside the draw range, independently of distance fading and instance selection.

| Cell event | Before | After |
| --- | ---: | ---: |
| Previously ready cell disappears | 653 | 0 |
| Previously seen cell returns after a gap | 665 | 0 |
| Surface signature changes while continuously present | 147 | 830 |
| Cell-level attachment history rejection | 892 | 0 |
| Maximum updates per frame | 32 | 32 |

Returns and disappearances need not match: cells can enter/leave the measurement
cone during a gap. The larger continuously observed change count after the fix
means repairs now arrive while cells remain present. It is not extra regeneration.
These are cell publication events, not a count of perceptual flickers or individual
plants. Diagnostic GPU readbacks serialize this trace; its wall time is not a benchmark.

[Before CSV](before.csv), [after CSV](after.csv), [before log](before.txt),
[after log](after.txt). Matching snapshots at frame 240: [before](before.png),
[after](after.png); still images alone cannot demonstrate temporal continuity.

```sh
cmake-build-release/vegetation_stream_probe /tmp/vegetation-continuity --engine-scene --distance 96 --flicker-check --size 1280 720
```

## Work and ownership

Current and previous-repair attachment buffers replace the two frame copies without
increasing GPU capacity. Previously published cells retain their last valid roots
while repairs wait; their bounds enclose old/new heights. A repaired cell copies
only its 2 KiB range before compute writes the current attachment. Previous roots
and slope drive motion vectors; new or newly accepted plants still reject history.
Missing terrain, incompatible population revisions and recycled slots cannot reuse
unrelated roots. Near-first repairs retain the 32-cell / estimated 0.5 ms cap.

A separate normal-renderer route, without readbacks, used 1920×1080, 96 m range and
the existing uniform-meadow traversal/teleport/return fixture. It recorded **163.889
MiB**, **zero render-thread C++ allocation calls** in the warm route and recovery,
and update GPU p95/max **0.052 / 0.057 ms**. The route returned to zero pending cells.
Whole-frame CPU median/p95 was 9.454 / 11.234 ms, GPU 8.473 / 10.019 ms, vegetation
CPU 0.530 / 1.946 ms. This single run checks bounded work, not a before/after speedup
or resolution of the existing CPU/GPU acceptance budgets. [Timing log](timing.txt).

## Correctness

Debug/Release builds and all 103 SPIR-V modules per build validate. Owned native
GPU tests exercise a one-cell-per-frame backlog in both material binding modes:
published cells remain present throughout terrain repartitioning, motion remains
valid for repaired foliage with fixed camera/wind, and settled roots still match
the actual rendered triangles. Teleport, revision, retirement, masking, interaction
and fixed-population paths remain covered. Vulkan synchronization validation checks
the shared buffers and range copies. See [verification](verification.txt).

Roots may briefly lag a terrain refinement while queued repairs drain. This is the
explicit continuity tradeoff; after repair, attachment is exact. Missing providers
or motion that outruns new population can still leave gaps. The patch fix does not
claim to eliminate every source of subpixel grass shimmer or the distant grass/ground
appearance boundary.

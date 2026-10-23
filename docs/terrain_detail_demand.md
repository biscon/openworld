# Terrain detail demand and cancellation

Status: implemented; target-GPU performance remains unverified. Extends [streaming](terrain_streaming.md) and [hierarchical residency](terrain_hierarchy.md) without changing cache or shader representations.

## Policy

A small CPU policy owns only the previous camera position and smoothed horizontal velocity. Updates take elapsed seconds; no allocation or wall clock is required. Velocity uses exponential smoothing with a 150 ms time constant. Prediction is 0.5 seconds ahead, clamped to two base chunks. Gaps over 0.5 seconds and steps over four chunks reset it. Camera-relative double precision bounds avoid tile-origin bias and preserve signed/distant coordinate behaviour.

Level-zero tiles within one base chunk of the current camera are protected and ordered by current horizontal distance to their bounds. Other candidates use min(current distance, predicted distance + 0.25 chunk), a finer-data-level preference and the existing one-tile retention preference. Hierarchical eligibility uses horizontal bounds distance, matching geometry selection; flat diagnostic mode retains its vertical LOD eligibility. Prediction can refine already-resident prefetch, never request additional coverage or enlarge pools. Orientation has no effect. Hierarchical eligibility uses the same base range as geometry: the previous committed quality scale plus one bounded frame of expansion, clamped to 2. Frozen LOD suppresses this allowance; disabled quality uses the baseline. Existing pool limits still determine which eligible tiles are selected. Diagnostics distinguish eligible, selected and resident-selected detail counts.

## Ownership and lifecycle

Each CPU slot has a monotonically increasing detail revision paired with its generation-checked coverage handle. Detail reads, generation, uploads and fence-delayed publications carry this identity. Losing demand invalidates the revision; preemption also invalidates it while preserving coverage. Returning demand cannot revive an old job. Demand and invalidation run before accepting publications.

Cancellation latches immediately. No subsequent compute, direct copy, upload or export batch may reference that job. Shared scratch and unexposed detail reservations remain held through their last recorded submission serial. Cancellation never extends that serial. Frame-owned staging remains protected by normal frame fences. Completed optional writes already owned by workers may finish.

The two cache mailboxes retain single render-thread ownership. Queued reads can be cancelled with a state CAS competing with the worker's claim. Running reads retain their mailbox until completion; a separate cancellation flag suppresses their result. Bookkeeping clears only for the matching handle/revision so an old completion cannot clear a newer read. No additional worker, GPU queue or blocking wait is introduced.

Still-wanted generation may be preempted after 100 ms and before 75% of workgroups have been recorded. A protected candidate can replace an unprotected job; otherwise the candidate must have twice the priority advantage and improve its distance score by half a base chunk. Partial work is discarded. A preempted tile has a 250 ms cooldown while a better runnable candidate exists; cooldown never leaves the generator idle. Useful uploads are not preempted for rank changes. Urgent missing protected detail may interrupt optional cache export once outstanding readbacks retire.

## Diagnostics and verification

Fixed-size diagnostics expose counters for demand-revision cancellations, preemptions, stale reads/publications, skipped exports and discarded workgroups. Demand cancellations count withdrawn missing-detail requests, including requests not yet scheduled. Skipped exports include urgent interruption, unavailable mailboxes and I/O backoff. Discarded groups were recorded while work was useful; the separate obsolete-group counter detects recording after demand has already been withdrawn. An owned CPU snapshot includes identities, demand, active job phase/progress, cancellation latch and submission/completion serials. ImGui shows compact progress and cancellation counts. Snapshot allocation is diagnostic only; runtime policy, scheduling and cancellation reuse startup storage.

CPU tests inject time to cover prediction, bounds, protection, hysteresis, thresholds and cooldown. Cache fixtures gate workers before claim and during I/O to check queued/running cancellation, lease ownership and repeated identities, including ThreadSanitizer. GPU fixtures cancel while coverage remains valid during compute, sliced upload, direct copy and export, verify stale publication rejection and fence-safe reuse, then settle. Existing coverage, vertex continuity, fade, epoch, cache, budget and memory regressions remain enabled. Separate cold 100/400 m/s flight, reversal and stop probes report readiness, obsolete work, frame percentiles and fixed memory. Software Vulkan is reported separately from RTX 3060 throughput.

## Verification results

- Debug and Release suites passed all 13 cases. After the final coverage-activation/cooldown adjustments, the three affected Debug GPU cases passed again and the complete Release suite passed in 70.23 seconds. Vulkan synchronization validation reported no errors.
- Streaming CPU fixtures passed ThreadSanitizer, including queued and running read cancellation, mailbox ownership, reuse with new revisions and repeated allocation-free demand/policy updates.
- Five GPU cases cover compute, partially copied direct detail, pending publication, optional readback and partially uploaded cached detail. Each withdraws demand while the coarse handle remains resident, returns immediately, and verifies revision invalidation, latched retirement, unchanged last submission serial, absence of stale publication, eventual stationary convergence, fixed allocations and Vulkan synchronization validation.
- Existing coverage, shared-vertex continuity, fades, epoch changes and cache reuse tests remain enabled. Optional exports deliberately skipped for urgent refinement may require later regeneration; warm-cache checks account for that recorded reason.

Software Vulkan measurements use llvmpipe (LLVM 23.1.1), validation enabled. RTX 3060 measurements are unavailable because this environment exposes no NVIDIA device.

### Cold-flight software comparison

The comparison uses a 64-cell grid (32 m chunks), radius 8 / 256 m maximum distance, 450 m camera altitude, separate fresh caches, three seconds forward, three seconds reversed, then settling. The previous source was rebuilt with measurement-only residency and obsolete-group diagnostics. Both use optimized C++ and identical shader outputs; the temporary previous-source rebuild does not enable CMake's link-time optimization, so small CPU timing differences should not be attributed to this policy. Full-size RTX throughput is not established by this reduced workload.

| Build / speed | Flight CPU p50 / p99 (ms) | Obsolete groups recorded after withdrawal | Stop-to-settle (s) | GPU / managed host (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Previous, 100 m/s | 6.23 / 8.95 | 0 | 5.71 | 80.02 / 32.781 |
| Previous, 400 m/s | 5.39 / 7.91 | 189 | 6.82 | 80.02 / 32.781 |
| Revised, 100 m/s | 5.83 / 7.65 | 0 | 5.92 | 80.02 / 32.794 |
| Revised, 400 m/s | 5.35 / 7.21 | 0 | 7.88 | 80.02 / 32.794 |

Both builds had zero protected level-zero detail ready during the short cold flight. After stopping, the revised policy reached 9/9 nearby tiles; the previous policy requested none at this altitude with the small grid's short normal-distance band. The revised policy therefore completes more detail work when settling. Its 100/400 m/s runs recorded 4/21 demand cancellations and skipped 17/21 optional exports. Neither final flight triggered generation preemption or discarded partial generation; threshold/cooldown cases are covered by the deterministic CPU policy tests, and the GPU fixtures exercise the shared cancellation/retirement path. Zero obsolete groups were recorded in either revised flight, compared with 189 at 400 m/s previously. Frame maxima around 141–145 ms include software-driver first drawing use. Fixed GPU allocation was unchanged; request/ranking metadata added about 13 KiB of bounded host storage. These single-run timing differences do not establish a frame-time speedup.

An earlier full-size 256-cell, radius-64 / 8 km cold probe exceeded its 60-second settling deadline on software Vulkan. It did not establish steady-state nearby detail throughput. Do not interpret the reduced fixture or cancellation itself as evidence of full detail at 100 m/s on RTX 3060.


Reproduction (fresh output directory required):

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ctest --test-dir cmake-build-release --output-on-failure -j 1
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json ./cmake-build-release/terrain_hierarchy_probe /tmp/terrain-detail-flight 64 8 256 3
# CPU cache/policy suite was also built with -fsanitize=thread and passed.
```

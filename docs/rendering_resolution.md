# Scene resolution and presentation

Status: implemented; Debug/Release and software Vulkan verified. Live windowed
and target-GPU performance acceptance remain pending.

The demo renders at a fixed height (1080 by default), deriving width from the
output aspect ratio. A smaller window still uses the chosen scene height and
downsamples. Equal-aspect output sizes therefore have identical scene workloads;
wider aspects render more pixels. Output tone mapping/scaling and ImGui still
cost work proportional to the native framebuffer. This is not dynamic resolution.

## Interfaces and ownership

`RendererOptions::renderHeight` and `Renderer::setRenderHeight()` choose scene
height; zero preserves the native-resolution behaviour of existing library users
and test fixtures. `width()/height()` and `resizeOffscreen()` describe output;
`renderWidth()/renderHeight()` describe effective scene dimensions. HDR, GTAO and
Hi-Z snapshots describe scene pixels, while PPM captures describe output pixels.
The demo uses `--render-height N` (1..16384) and ImGui presets
720/900/1080/1440/2160, with session-only settings. Its window and headless output
default to 1920x1080.

A small CPU extent helper rounds the aspect-derived width to the nearest pixel,
with a one-pixel minimum. It proportionally reduces extreme extents to fit the
engine's 16384 limit and device image/framebuffer/viewport limits. Requested and
effective dimensions are separately visible in the UI. Projection uses the
effective scene aspect everywhere, including clustered lights and shadows.

Each of two in-flight frame slots owns HDR and depth images at scene resolution.
Presentation targets own only output colour views/images and per-swapchain-image
presentation semaphores. Scene resources include GTAO, Hi-Z and cluster storage;
terrain pixel-error LOD uses scene height. Shadow-map resolutions are independent.
No additional intermediate colour target or generic render graph is introduced.

The existing output pass samples pre-exposed HDR with a clamp-to-edge linear
sampler at normalized output coordinates, then tone maps to the sRGB attachment.
Diagnostics use nearest texel loads at mapped scene coordinates. ImGui follows
at output resolution with its existing display-linear blending contract.

## Resize and lifetime

Explicit settings are queued and applied at frame boundaries. Output resizing
recreates presentation resources; scene resources are recreated only when their
effective extent changes. Scene graphics pipelines survive resize. Output format
changes recreate the output pipeline; swapchain recreation may reinitialize the
ImGui Vulkan backend and skip the old UI draw data.

Resource replacement uses the existing device-idle boundary before destroying
images or updating descriptors. Snapshot availability and pending timing results
are invalidated on recreation. Ordinary frames add no allocations or waits.
Existing cluster/GTAO budgets apply; allocation failures remain explicit rather
than silently lowering quality. Zero-sized windows defer rendering/recreation.

F10 (non-repeating, independent of ImGui focus) requests SDL borderless desktop
fullscreen. SDL owns restoration of windowed geometry. Fullscreen and pixel-size
events mark presentation dirty; requests remain asynchronous. SDL failures are
reported without terminating the application.

## Verification

CPU tests cover rounding, same-aspect invariance, native mode, smaller outputs,
limits and invalid inputs. Small owned GPU fixtures cover up/downscaling, odd
dimensions, aspect/height changes, HDR/readback sizes, full output coverage,
orientation and native UI edges. Exercise Hi-Z, GTAO, clusters and depth prepasses
with differing extents under Vulkan validation, alongside existing renderer
regressions in Debug and Release.

Live acceptance covers resize/maximize, high DPI, minimize/restore and repeated
F10 transitions with restored geometry. Compare GPU timings at a fixed camera
and scene extent across output sizes; distinguish expected scene-cost stability
from measured results. Record hardware-dependent checks that cannot run.

### Results (2026-10-23)

- Debug and Release builds pass. In each configuration, all 17 selected tests
  pass: CLI help, resolution CPU/GPU, lighting CPU/GPU, scene CPU/GPU, visibility
  CPU/GPU, GTAO CPU/GPU, shadows CPU/GPU, terrain LOD CPU/GPU, terrain LOD quality
  GPU, and Vulkan smoke. GPU tests use llvmpipe (LLVM 23.1.3) with Vulkan validation,
  including synchronization validation; the resolution fixture exercises both
  GTAO and Hi-Z without skipping either path.
- The resolution fixture compares bilinear HDR sampling, both tone mappers and
  sRGB output against a CPU reference (three 8-bit code values tolerance for
  filtering/quantization). It checks native UI edges/alpha blending, nearest
  AO diagnostics, up/downscaling, odd dimensions, both frame slots, and clustered
  versus brute-force light results. Same-aspect output resizing preserves exact
  scene pixels and HDR allocation size in the deterministic fixture.
- A headless 1920x1080 output with custom render height 123, owned fallback
  materials, an 8-cell terrain and UI disabled rendered/captured three frames
  with zero validation errors. CLI rejects zero, negative, oversized, fractional,
  nonnumeric, overflowing and missing heights.
- The NVIDIA Vulkan driver cannot load and the configured X display is
  inaccessible in this execution environment. F10/windowed geometry restoration,
  interactive resizing, high-DPI behaviour and minimize/restore remain unverified
  live. Scene-cost stability on target hardware is expected, not measured;
  software-renderer timings are not hardware performance evidence.

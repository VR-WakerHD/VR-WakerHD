# Both eyes in one draw: multiview (VK_KHR_multiview, core Vulkan 1.1)

Stereo draws every scene draw twice today. Eye 0 resolves the draw, and eye 1 replays it: the same pipeline, with its
own render targets, its own copy of the uniform block that holds the camera projection (shifted on the CPU,
`stereo::project_eye`), and its own versions of the textures that are an eye's render targets. Multiview draws both
eyes at once: the targets are the two layers of one image, the render pass has view mask 0b11, and the shaders read
`gl_ViewIndex` to pick each eye's data. Eye 1's CPU work (recording a second draw) goes away.

This is the final goal of the VR performance work. The eye thread (`docs/VR_EYE_THREAD.md`) is set aside for it.

## Stage M1 done: what the device offers, and what the draws need

**The device** (`WWHD_VK_MULTIVIEW=1` enables the feature; it is queried and logged always):
`[vulkan] multiview: supported (max 32 views, geometry shaders 1, tessellation 1)`. The game runs with it enabled.

**Eye 1's draws** (`WWHD_VK_MULTIVIEW_STATS=1`, render thread, title screen, two 600-frame windows):

| eye 1's draw differs from eye 0's in | per frame | share |
|---|---|---|
| its targets only | 39 to 59 | about 3% |
| also an eye-shifted uniform block | 548 to 833 | about 47%; always exactly 1 block a draw |
| also eye-specific textures | 540 to 856 | about 49%; 1.6 to 1.8 a draw |
| resolved in full (no replay) | 0 | |

Eye 1's draws cost 1.3 to 1.9 ms a frame on the title screen (`WWHD_VK_TARGET_STATS=1`), and 2.6 ms in play.

**What follows:**
- **Uniforms:** a draw has at most one eye-shifted block. A multiview shader can read that block from a second
  binding for view 1, `gl_ViewIndex` choosing between them: one extra binding a stage, not twice every block. It
  stays within NVIDIA's 15 dynamic uniform buffers a set.
- **Exactness:** the shift itself stays the CPU's (`project_eye` on the block, as now), so both views read exactly
  the bytes each eye reads today. A shader-side shift of `gl_Position` would differ wherever the game uses the
  projection for anything but the position.
- **Textures:** half the draws sample an eye's own render target. Those targets become two-layer images, and the
  multiview shader samples them as arrays at layer `gl_ViewIndex`.
- **Targets:** every eye-dependent render target becomes the two layers of one image.

## Stages

1. **M1. Support and measurement:** done, above.
2. **M2. Two-layer eye targets, still drawn per eye.** Eye 1's render targets live in layer 1 of eye 0's image. Each
   eye's pass renders to its own layer (views of one layer), and eye 1's textures are its layer. Pictures must stay
   byte-identical with multiview off.
3. **M3. Shader variants.** The translator emits a multiview variant: `GL_EXT_multiview`, the eye-shifted block
   declared twice (view 0's binding and view 1's), selected by `gl_ViewIndex`, and eye-specific textures as arrays
   sampled at layer `gl_ViewIndex`. Built and validated, not yet used.
4. **M4. Multiview draws.** A replayable draw is drawn once with view mask 0b11 into both layers, with both blocks
   bound. Anything that does not fit (no replay, an unexpected layout) falls back to two draws. Each eye's picture
   is compared with today's.
5. **M5. Timing, then a headset session.**

Tests between stages are short: the title-screen run (about a minute, `scratchpad` `flicker.ps1`), with the
frame-4200 pictures for the stages that change pictures.

## Stage M2a done: surfaces can start at a layer of their image

`Surface::baseLayer` (0 everywhere so far) is the first of its image's layers that are the surface's. Every
subresource range, image view and copy of a surface now starts at it:
- layout transitions;
- the sampling, layer, sampled-texture and depth-copy views;
- clears, copies and blits between surfaces;
- uploads, readbacks and the BC decoder's upload;
- the mip-chain builder's sources, the feedback copy, the depth peek, the TV capture and the cloud depth view.

Images that are never a surface's (the swapchain, scans, overlay, Android presenter) are unchanged. Nothing changes
yet: the short title-screen test passes.

## Stage M2b done: eye 1's render targets are layers of eye 0's images

With `WWHD_VK_MULTIVIEW=1` (off by default):
- **Eye 0's 2D render targets get images with twice the layers** while stereo draws both eyes. The sun atlas is not
  doubled (eye 1 uses eye 0's), and nor are 1D or 3D images.
- **Eye 1's matching target** (same guest surface, format and extent) shares the image: `baseLayer` is eye 0's layer
  count, with its own views and its own layout tracking. Only the owner creates and frees the image
  (`attach_eye1_layers`, `link_eye1_layers`).
- **Each surface knows its map** (`Surface::eye`: 0, 1, or 255 for scans, private and temporary images), so only the
  eyes' own targets take part.
- **Before an owner's image is retired** (a rescale, a format change, stereo switched off), eye 1's half moves to an
  image of its own with its pixels copied (`detach_eye1_layers`). Eye 1 retiring its half only frees its views.
- **Not with dynamic resolution,** which swaps images between surfaces: multiview stays off, logged.
- **Copies of a surface's fields into a private image** (the AO images made like a target) drop the link
  (`Surface::own_image_only`). Copying `baseLayer = 1` into a one-layer image lost the device on the first run.
- `WWHD_VK_MULTIVIEW_NO_ATTACH=1` (diagnostics) doubles the images without sharing them.

Each eye is still drawn on its own, so the pictures must not change. **Checked (desktop stereo):** the title-screen
test passes; frame 4200 byte-identical in both eyes (`89FDFF9DEFEA1038`, `33CB7D247CB2B0D0`).

## Stage M3 done: each shader's multiview variant

`multiview::rewrite` (`multiview_glsl.h/.cpp`) turns a translated shader into its multiview variant. Only the
declarations change, and every use in the code stays as the translator wrote it:
- `#extension GL_EXT_multiview : require` is added;
- **a per-view uniform block** (a guest buffer's `uniformBlockVS<i> { vec4 uf_blockVS<i>[n]; }`, or the support block
  `ufBlock`) becomes a two-element block array. Its names become macros that index it by `gl_ViewIndex`, for example
  `#define uf_blockVS0 uf_blockVS0_blk[gl_ViewIndex].uf_blockVS0_mv`;
- **a per-view texture** becomes a two-element sampler array, its name a macro the same way.

In Vulkan each array is one binding with two descriptors, view 0's and view 1's (M4). Indexing them by `gl_ViewIndex`
needs the device's dynamic indexing of uniform buffer and sampled image arrays, which M4 enables.

**Checked:** `WWHD_VK_MULTIVIEW_VALIDATE=1` builds each new shader's variant with everything per view (every block
and texture it uses) and compiles it (the first failure's text goes to `%TEMP%\wwhd_multiview_failed.glsl`). On the
title-screen test, all 301 or more variants compiled and none failed. Nothing draws with them yet, and the test passes.

## Stage M4a done: one draw for both eyes when only the uniforms differ

With `WWHD_VK_MULTIVIEW=1` and `WWHD_VK_MULTIVIEW_DRAWS=1` (both off by default), eye 0's draw draws both eyes
(`multiview_draw` in `draw.cpp`) when eye 1 would only replay it:
- **Its targets** are two-eye targets, at layer 0, and eye 1's own lookup finds their twins.
- **Its stages** replay (`StageReplay`) and sample no eye-specific texture (that is M4b).
- **What stays out:** the AO downsample and occlusion passes, which have per-eye work of their own; the eye thread,
  the atlas's thread and the held segments; and pipelines still compiling.

**How it draws:**
- **The pass** has view mask 0b11, and its attachments are both-eyes views of the targets (`multiview_view`). Both
  eyes' layers are transitioned. Eye 1 then draws nothing (`StereoShaders::multiview`).
- **The pipeline** is a twin of the guest pipeline (`multiview_pipeline.h/.cpp`): the same state and view mask
  0b11. A stage with eye-shifted blocks uses its multiview variant (M3), whose blocks are two-descriptor bindings.
  Twins are built on a thread of their own; until one is ready, both eyes are drawn as before.
- **The descriptors** hold each eye-shifted block twice: the same buffer at view 0's offset, then at view 1's. View
  1's copy is eye 0's bytes shifted on the CPU for eye 1, as `replay_stage` does, so each view reads exactly the
  bytes its eye reads today.
- **Order:** eye 1's held commands (its draws that were not shared) are emitted before a multiview draw
  (`stereo_eye1_holding`, then `stereo_flush`), so they keep their place in eye 1's layers.

**A fix on the way:** making a target two-eye only while streaming left most targets one-eye, because most are
first made by a clear, an in-order operation. With all of them two-eye, the device was lost. The feedback copies
(`make_feedback_image`) copied a target's whole description, layer links included, into a one-layer image; they
now drop the links (`own_image_only`).

**Checked (desktop stereo):**
- the title-screen test passes;
- frame 4200 is byte-identical in both eyes (`89FDFF9DEFEA1038`, `33CB7D247CB2B0D0`);
- title screen, per frame: 818 draws drawn for both eyes, 856 waiting for eye-specific textures, about 75 on
  targets that are not two-eye, and 4.7 flushes of eye 1's held commands. The sun atlas's ~2000 draws are eye 0's
  only, as before.

## Stage M4b done: eye-specific textures too

A draw that samples an eye's own render target is drawn once as well. The texture's binding becomes a two-element
sampler array: eye 0's image, then eye 1's (the variant's `textureUnit<S><u>_mv[2]`).

**Eye 1's lookups:** they run with only eye 1's surface maps selected (`Eye1Lookups`), so eye 0's stream and pass
stay attached. Whatever a lookup records, such as an upload or a layout change, then lands before the draw that
reads it. Eye 1's held commands are flushed before the lookups, so none of them can trigger a flush midway.
- **The first version broke this.** It selected eye 1 fully for its lookups. An upload one of them made waited in
  eye 1's held stream, behind the multiview draw that sampled the texture. Frame 4200 differed in both eyes, in
  0.04% of pixels.
- **Draw batching:** batching (a submission every 2048 draws) counts a multiview draw as two, since it is both
  eyes' work. Counted as one, submissions came later in the frame, and the render thread waited about 2 ms a frame
  on the GPU.

**Checked (desktop stereo):**
- the title-screen test passes;
- frame 4200 is byte-identical in both eyes (`89FDFF9DEFEA1038`, `33CB7D247CB2B0D0`);
- in play, 2548 draws a frame are drawn for both eyes, about 70 are not (targets that are not two-eye), and there
  are 5 flushes a frame.

**Timing** (benchmark settings, 2 runs each, frames from 2400):

| | frame ms | render-thread CPU ms | ops ms | draw ops ms | render thread idle ms |
|---|---|---|---|---|---|
| default | 17.69 | 14.88 | 19.03 | 15.64 | 1.2-1.3 |
| multiview draws | 17.52 | 13.97 | 17.37 | 14.20 | 2.2 |

**What the timing shows:**
- **The saving:** multiview saves 1.4 ms a frame of draw ops and 0.9 ms of render-thread CPU.
- **The frame time** barely moves. The saving shows up as render-thread idle time instead (waiting for the game's
  commands): the frame is not limited by eye 1's draws.
- **The GPU** runs for 3.4 ms of each frame, by submission timestamps (`WWHD_VK_GPU_TIMESTAMPS=1`), so it is not
  the limit either.
- **What sets the frame time:** the game thread waits for the render thread at DrawDone, twice a frame, about 7 ms
  in total. It reaches 28.4 logic steps per second, below the 30 it runs at. The handover between the game thread
  and the render thread decides the frame time; that is the next thing to look at for 90 fps.

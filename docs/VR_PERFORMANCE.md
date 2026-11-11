# VR performance

## How a VR frame is made

- **Game thread.** The recompiled game runs its logic at 30 steps per second. Frame interpolation adds in-between
  frames (`interp.cpp`). Each frame's draw commands go to the render thread, and the game waits for them twice a
  frame (`GX2DrawDone`).
- **Render thread** (Vulkan, `gfx/vulkan`). It translates the commands, and the stereo renderer draws them for
  both eyes (`stereo.cpp`). At the game's swap, `vrvk::frame_begin` calls `xrWaitFrame` and `xrBeginFrame`, the TV
  present blits each eye's picture into its OpenXR swapchain image (`record_eyes`), and `vrvk::frame_end` releases
  the images and calls `xrEndFrame`. Each frame is submitted with the head views it was drawn with (the views ring
  in `vr.cpp`), so the compositor reprojects from the right pose.

The OpenXR call order is wait, begin, acquire and wait image, release, end, once per game frame, and each image is
acquired and released in the same frame. The headset's refresh rate paces the whole chain through `xrWaitFrame`.

## Measuring

Every 10 seconds while VR runs, the log gets three kinds of line:

- `[vr] timing`: frames per second to the headset, and the average `xrWaitFrame` time.
- `[vr] frame times`: percentiles rather than averages, over the frames shown:
  - the interval between frames (p50, p95, p99, max), and how many took more than 1.5 times the budget;
  - the render thread's work after `xrWaitFrame`;
  - `xrWaitFrame` itself;
  - missed display periods (the runtime showed an old frame again);
  - how far the game moved on per frame shown, in logic steps. Motion is even when every frame moves the same
    amount (1/3 of a step at 90 Hz).
- `[interp] paced`: how many of the planned in-between frames were drawn, and what one cost.
- `[vr] VR work on the game thread`: what VR adds to the game thread per frame shown, by part (camera, pad, Link's
  execute, arms, draw).

For more detail:

- `WWHD_PROFILE=1`: the render thread's time per operation, its CPU time, and the game thread's wait for it.
- `WWHD_VK_STATS=1`: swap phases, submission waits, and retirement cleanup split into completions, destroyed
  images and pool resets.
- `WWHD_VK_GPU_TIMESTAMPS=1`: GPU submission intervals.
- `WWHD_VK_TRACE_DESTROY=1`: the images destroyed during gameplay.

To repeat a measurement, start each run from the same state. The game overwrites `last_session.wwstate` when it
exits, so copy a saved one back before each run. Then start with `WWHD_AT_START=last`, use `WWHD_PRESS` only until
gameplay is in, stand still, and compare the `[vr] frame times` windows after the first one.

## What was found (PS VR2 at 90 Hz, RTX 5080, internal resolution 3x: 3840x2771 per eye)

Baseline in gameplay on Outset:

- 30 to 45 frames per second reached the headset against a budget of 11.11 ms.
- p50 frame interval 22 to 33 ms; every frame was over 1.5 times the budget.
- GPU submission intervals were about 4.6 ms per frame and `xrWaitFrame` about 0.2 ms, so neither the GPU nor the
  runtime was the limit.
- The render thread spent 21 to 25 ms per frame on its operations, and the game thread waited 19 ms per frame for
  it. Frame interpolation then dropped 83 to 100 % of its in-between frames, because each one cost 23 to 33 ms.

The causes, largest first:

1. **The desktop GamePad window.** Presenting it took 8.6 ms of every VR frame (the "secondary" swap phase), plus
   3.7 ms of submission waits. In VR the GamePad screen is already a panel in the headset. With the window off,
   the same scene went from 22.3 to 18.2 ms per frame (45 to 55 frames per second).
2. **Feedback copies destroyed and recreated every frame.** When the game samples a render target it is drawing
   to, the renderer copies it. Those copies are kept for reuse up to a fixed 128 MiB. One eye-sized copy at 3x is
   41 MiB, and the two eyes' colour and depth copies need about 250 MiB, so three of them were freed and
   allocated again every frame: 4.4 ms per frame in `vkFreeMemory` (`[vulkan retirement cleanup]`). With a larger
   budget, the image destruction during gameplay went to 0 and garbage cleanup from 4.4 to 0.17 ms per frame.
3. **Interpolation at 120 fps on a 90 Hz headset.** The in-between frames were planned as quarter steps for
   120 fps but shown at 90 Hz, so the game advanced by 0.25, 0.5 or 1 step from one frame to the next: judder,
   even when the frame rate was high (the "uneven" count). Interpolation now draws 90 fps on 88 to 117 Hz
   headsets: three frames per step, a third of a step each, one per refresh.

Not changed (measured, not the limit, or no safe change found):

- `xrWaitFrame` (0.2 ms) and the eye blits (under 1 ms).
- The GPU (about 4.6 ms per frame of submission intervals).
- Stereo draws. Each eye's commands are recorded separately; multiview would need a rewrite of the renderer.
- Outdoors on Outset there are about 6,500 draws per frame, and recording them takes 18 ms of render-thread CPU.
  That remains the main limit in the busiest scenes.

## Changes

| Change | Files | Why it helps |
|---|---|---|
| The desktop GamePad window is not presented while VR runs (`vr.desktopGamePad`, off by default; in the launcher's Graphics tab and in F1 > VR) | `backend.cpp` (`swap`), `vr_vulkan.cpp` (`skip_desktop_gamepad`), `vr_config.h` | removes about 8.6 ms of present and 3.7 ms of submission wait from every frame |
| Feedback-copy budget: 1/16 of the largest device-local heap, between 128 and 768 MiB; Android keeps 128 MiB; `WWHD_VK_FEEDBACK_BUDGET_MIB` overrides it for tests | `draw.cpp` (`feedback_retained_budget`) | no per-frame allocation and freeing of 41 MiB images |
| 90 fps interpolation on 90 Hz headsets; the virtual vsync clock counts twelfths of a vsync (it counted quarters) so three frames per step are whole ticks | `interp_pacing.h` (`cap_fps_vr`), `interp.cpp` (`vsync_granule_ticks`, `step_ticks`), `gx2_core.cpp` (`kTicksPerVsync`) | each frame shown advances the game by the same third of a step, one per refresh |
| Frame-time diagnostics: percentiles, missed periods, motion evenness; the game time each frame shows travels in the views ring | `vr_vulkan.cpp`, `vr.cpp`/`vr.h` (`store_views`, `views`), `vr_game.cpp` | separates frame rate from smoothness; no allocation per frame (buffers reserved once) |
| Retirement cleanup timing split; destroyed-image trace | `backend.cpp`, `surfaces.cpp` | found cause 2 |

## Tests

- `vr_test` checks 90 fps on a 90 Hz headset (120 and 240 fps settings both give 90), the thirds-of-a-step pass
  fractions, and 120 fps at 120 Hz or more.
- `interp_pacing_test`, `vsync_pacing_test`, `true60_steps_test` and `turbo_steps_test` pass with the new tick
  unit. The full suite passed 607 of 608 then; `bc_compute` failed on this RTX 5080 before these changes as well.
  It passes 608 of 608 since 2026-10-25, as `bc_compute` now allows for this GPU's own BC decoding.
- The GamePad window and feedback budget changes were measured in the headset (numbers above). The 90 fps change
  is built and unit-tested but has not yet been measured in the headset: check that `[interp] VR headset at 90 Hz:
  frame interpolation draws up to 90 fps` appears in the log, and that `game time per frame` is near 0.333 with
  few uneven frames.
- Linux: not built or run.

## Recommended settings for 90 Hz (hardware-dependent)

- The GamePad window on the desktop: off (the default).
- Frame interpolation for VR: on (the default). It matches the headset's rate.
- Internal resolution: start at 2x. At 3x the eye pictures are 3840x2771, and the render thread and GPU work grow
  with them. 3x fits only where the measured frame times stay inside 11.1 ms.
- The headset's own reprojection (SteamVR Motion Smoothing, Meta ASW) covers frames that miss their deadline. Each
  frame is submitted with the pose it was drawn with, so reprojection works from the right pose.

## Second round (2026-10-25): measured without a headset

**The proxy.** No headset was available. The desktop's full side-by-side 3D mode (`stereoMode=2`) goes through the
same stereo renderer as VR: both eyes, each guest draw recorded per eye, the shared sun atlas once. Settings:
- the player's internal resolution, 3x;
- frame interpolation asking for 120 fps (`WWHD_INTERP_FPS=120 WWHD_DISPLAY_HZ=120`);
- no vsync (`WWHD_VK_PRESENT_MODE=immediate`, as VR's mirror window).

The frame rate reached then measures how much work fits in a frame. Each run started from copies of the player's
last session on Outset, standing still. The profile, states and saves were copies; scripted runs need
`WWHD_SETTINGS=<settings.ini>`, because without it they ignore `settings.ini`. The CPU is a Ryzen 9 3900X and the GPU
an RTX 5080.

| Run | Frames/s | Per frame | Render-thread operations | Render-thread CPU | In-between frame |
|---|---|---|---|---|---|
| Mono, 3x | 85 (the 3-per-step cap at 28.4 steps/s) | 11.7 ms | 11.8 ms | 9.4 ms | 9.4 ms |
| Stereo, 3x, before | 57.6 | 17.3 ms | 19.0 ms | 15.9 ms | 15.4-16.0 ms |
| Stereo, 3x, after the fix below | 57.8 | 17.3 ms | 17.3 ms | 13.7 ms | 16.8 ms |
| Stereo, 1x, after | 57.6 | 17.3 ms | 17.6 ms | 14.2 ms | |

The stereo runs reproduce the headset's 55 to 60 fps.

**The limit is the CPU, not the GPU or the resolution.**
- The GPU waits are 0 ms.
- 1x and 3x give the same frame time.
- The frame is set by the render thread: it records and submits every draw on one thread.
- The game thread spends about 10 ms producing an in-between frame's commands, then waits about 6.7 ms in
  `GX2DrawDone` for the render thread.
- An in-between frame takes about 17 ms, so frame interpolation fits only one between two logic steps: 2 frames
  per step, 58 fps.
- 90 fps needs 3 frames per step, so every frame must take 11.1 ms or less.

**Where the draws go** (stereo, `WWHD_VK_TARGET_STATS=1`, draws per frame):

| Target | Draws |
|---|---|
| the sun's shadow atlas (3072x3072, the game's own cascades), shared by both eyes, drawn once | 3,030 |
| the scene, 3840x2160 | about 2,400 per eye |
| the GamePad screen, 2562x1440 | 142 per eye |
| half-size passes, 1920x1080 | 54 per eye |

That is 8,200 recorded draws per frame against 5,300 in mono. The GamePad screen's draws are left as they are: the
player asked for no changes to the GamePad screen or its mechanics in performance work. The second eye costs about 1.7 us per draw, nearly as
much as the first (1.8 us). Per draw (stereo, ms per frame), the time is spread over every phase:

| Phase | ms/frame |
|---|---|
| uniforms | 2.7 |
| register writes | 1.8 |
| recording | 1.7 |
| submission | 1.5 |
| vertices | 1.45 |
| shaders | 1.4 |
| render passes | 1.3 |
| textures | 1.3 |
| descriptors | 0.9 |

### Fixed: the invalidation index rebuilt at every invalidate in stereo

`invalidate_surface_cache` (`surfaces.cpp`) finds the surfaces a guest invalidate hits through an interval index. The
index was rebuilt whenever `g_surface_set_gen` changed. `stereo_select_eye` swaps the two eyes' surface maps and
bumped that counter at every eye switch, which is twice a draw. So nearly every one of the frame's 192 invalidates
rebuilt the index over all surfaces: 9.4 us each, 1.8 ms per frame, against 0.05 us in mono.

The index now belongs to an eye's surfaces, not to the map that holds them. The eye switch no longer bumps the
counter; adding or removing a surface still does.

- **Measured:** invalidates 1.81 to 0.02 ms per frame; render-thread CPU about 15.8 to 13.7 ms per frame.
- **Checked:** `WWHD_VK_INVALIDATE_VERIFY=1`, which compares the index with a full walk of every surface, made 167
  million checks in stereo with 0 misses.
- **The frame rate did not change:** the in-between frame stays over the 11.1 ms an extra frame needs.

### Tried, no gain (not changed)

- **The record worker (`WWHD_VK_RECORD_WORKER=1`):** render-thread CPU 13.7 to 12.9 ms, the same 57.8 fps. The
  render thread still prepares every draw; only the Vulkan calls move.
- **Draw batching:** batching off gave the same; 8,192 draws per batch was worse (55.6 fps).
- **Lower resolution:** not a fix. It does not change the frame time, and resolution is the player's choice.

### The second eye, measured (`WWHD_VK_TARGET_STATS=1`)

Every second-eye draw is replayed from the first eye's: the same pipeline and scale, both shader stages, 0 failed
replays. Time per stereo frame:

| Part | ms/frame | Per draw |
|---|---|---|
| eye 0's scene draws, about 2,640 (including what they prepare for eye 1) | 5.5 | 2.1 us |
| the shared sun atlas, 3,030 draws | 3.7 | 1.2 us |
| eye 1's draws, about 2,640 (replays) | 2.5 | 0.96 us |
| flushing eye 1's held commands into the command buffer | 1.0 | |

**Moving all of eye 1's work to another thread would save at most about 3.5 ms:** 17.3 to about 13.8 ms per
frame. That is still over the 11.1 ms a third frame per step needs, so the headset would still get 2 frames per
step, about 58 fps. The second eye's draws also share the upload allocator, the image layout tracking and the
render-pass state with the first eye's, so the change is large. It was not started: on its own it would not show.

### What still stands between this and 90 fps

The stereo frame needs about 6 ms less render-thread work per frame: 17 ms down to 11 ms. There is no single large
waste left. The cost is ordinary per-draw work, times 8,200 draws per frame on one thread. The next steps, largest
first:

1. **Several threads together.** No single part is large enough alone; reaching 11.1 ms needs more than one of:
   - eye 1 on its own thread: up to 3.5 ms;
   - the shared sun atlas recorded on a thread of its own: up to 3.7 ms;
   - eye 0's preparation (5.5 ms) split.

   Each needs the renderer's per-draw state (uploads, descriptor and draw-state caches, image layouts, render
   passes) owned per thread: a rewrite of how the Vulkan renderer records.
2. **Cheaper uniform packing** (2.7 ms): most draws only change buffer pointers or ALU constants (74 %).

**The game thread** (about 10 ms of commands per in-between frame) is also close to 11.1 ms, and VR adds its own
work there (the camera, arms, wrists). That was not measured: it runs only with a headset. Check `[vr] frame times`
and `[interp] paced` in the headset after these changes.

## Third round (2026-10-26): today's baseline and the roadmap

### The baseline

**In the headset** (PS VR2 at 90 Hz, RTX 5080, Ryzen 9 3900X, internal resolution 3x). The OpenXR swapchains are
3224x3288 per eye. These are the steady 10-second windows of two play sessions on 2026-10-25 (shield and Wind Waker
tests: Outset, normal play):

| Measure | Value |
|---|---|
| Frames to the headset | 60.1 to 60.3 per second |
| Frame interval | p50 16.67, p95 17.2 to 18.4, p99 20 to 24 ms |
| `xrWaitFrame` | p50 0.20, p99 0.4 ms |
| Render thread after `xrWaitFrame` | p50 4.5 to 8.9 ms |
| Missed display periods | 255 to 308 per 10 s |
| Game time per frame | exactly 0.500 step every frame (even at 90 Hz: 0.333); 97 to 99 % of frames uneven |
| `[interp] paced` | 50 % of in-between frames drawn: 1 of 2 planned per step, the in-between pass 15.3 to 17.2 ms |

**In the stereo proxy** (as in round two: `stereoMode=2`, 3x, 120 fps asked for, no vsync, from the same copies of
the last session on Outset, standing still):

| Measure | Value |
|---|---|
| Frame time | 17.3 to 17.9 ms, 56 to 57.8 fps |
| Render-thread CPU | 15.8 to 16.4 ms per frame (14.1 to 14.8 ms in other runs of the same build: identical runs differ by about 1 ms here) |
| Draws | 5,630 guest draws per frame |
| By target (`WWHD_VK_TARGET_STATS=1`, ms per frame) | eye 0 6.04, the shared sun atlas 4.06, eye 1 2.68, eye 1's flushes 1.08 |
| Draw phases, ms per frame | uniforms 2.94, recording 1.85, shaders 1.73, vertices 1.48, render passes 1.47, textures 1.43, submission 1.25, descriptors 0.97, indices 0.78, targets 0.69, pipelines 0.49 |
| Register writes | 1.9 ms per frame (41,900 writes) |
| The game thread's wait for the render thread (`GX2DrawDone`) | 8.4 ms per frame |
| GPU waits | 0 |

**What this confirms from rounds one and two:**
- The limit is the render thread's CPU work, about 16 to 17 ms per stereo frame against the 11.1 ms a third frame per
  step needs. It is not the GPU, the resolution or the runtime.
- No single phase is large: the cost is ordinary per-draw work, about 2.7 us a draw, times 5,600 draws.
- The per-target times are a little higher than round two's (eye 0 5.5 → 6.0 ms, atlas 3.7 → 4.1 ms), in the same
  scene. That is within this machine's run-to-run spread; nothing in the code has made them larger.

**New: 60 fps on a 90 Hz headset judders.** When only one in-between frame fits in a step, the pacer plans halves
of a step: 2 frames per step over 3 refreshes. The frames are then shown for 1 and 2 refreshes in turn (16.67 ms on
average, about 280 missed display periods every 10 s), each advancing the game half a step. So motion that the head
does not cause (Link walking, the camera following him, animations) moves at uneven speed. Head rotation itself is
reprojected from the pose each frame was drawn with. `[vr] frame times` counts this as 97 to 99 % uneven frames.

### Tried this round, no gain (not changed)

All in the stereo proxy (frame time, render-thread CPU):

| Switch | What it does | Result |
|---|---|---|
| `WWHD_VK_REUSE_VERTEX_SNAPSHOTS=1` | reuses an identical vertex snapshot within a submission | 17.4 ms, 14.6 ms; vertex copies unchanged (8.7 to 8.95 MiB per frame) |
| `WWHD_VK_VERTEX_COPY_WINDOW=1` | copies only the indexed window of a vertex buffer | 17.5 ms, 15.5 ms; vertex copies unchanged |
| `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=1` | reuses an identical uniform upload within a submission | 17.5 ms, 14.8 ms; uniforms 2.8 ms (from 2.9) |
| the same with `WWHD_VK_RECORD_WORKER=1` | Vulkan calls on a worker thread | 17.5 ms, 13.2 ms; the render thread idles 2.6 ms per frame |

The last one moves about 3 ms of CPU off the render thread and still shows no frame. Frames come in whole steps:
with 2 per step the frame is about 16.7 ms whatever it costs below that, and a third needs every frame under
11.1 ms.

**An open question:** the frame copies 8.95 MiB of vertex data, but only 1.84 MiB of it is distinct guest data
(`[prof] unique guest MiB/frame`). None of the switches above changes that, so the repeats are on another path. They
are most likely the eye and the atlas passes each copying animated vertices the buffer cache does not keep. The
vertex phase is 1.5 ms, so that is the most it could save.

### Changed this round

| Change | Files | Why |
|---|---|---|
| The VR work on the game thread is timed by part and logged with `[vr] frame times` as `[vr] VR work on the game thread: ... ms per frame shown (camera, pad, Link's execute, arms, draw)` | `vr.h`/`vr.cpp` (`GameWork`, `GameWorkTimer`), `vr_game.cpp` (each entry point), `vr_vulkan.cpp` | the game thread produces an in-between frame in about 10 ms, near the 11.1 ms budget, and what VR adds there had never been measured (it runs only with a headset). Two clock reads per call; the proxy's frame time is unchanged with it (17.3 to 17.4 ms) |

### Upper bounds, measured (2026-10-26, desktop stereo mode)

What the two largest Phase 2 ideas could save at most, measured by removing their work outright. `WWHD_VK_SKIP_SUN_ATLAS=1` (a diagnostic, never for play: the shadows are wrong) leaves out every draw into the sun atlas; mono has no second eye. Same scene and settings as the baseline:

| Run | Frame time | Frames/s | Render-thread CPU | Game thread's wait (`GX2DrawDone`) |
|---|---|---|---|---|
| Stereo, as today | 17.3 to 17.9 ms | 56 to 57.8 | about 16 ms | 8.4 ms |
| Stereo, no sun atlas | 12.2 ms | 82 | 11.2 ms | 7.0 to 7.3 ms |
| Stereo, no sun atlas, the record worker and uniform reuse on | **11.85 ms** | **84.4: the three-per-step cap** | 9.7 ms | 5.9 ms |
| Mono (no second eye) | 11.85 ms | 84.5: the cap | 9.6 to 10.0 ms | 5.4 ms |
| Mono, no sun atlas | 9.1 ms | 109 | 6.4 ms | 3.5 ms |

(The cap is three frames per logic step; these runs' logic ran at 28.1 to 28.4 steps per second. In the headset the logic runs at 30, so the same cap is 90 fps.)

**What this shows:**
- The sun atlas is the lever. Without its draws, stereo goes from 2 to nearly 3 frames per step. With the two existing switches (`WWHD_VK_RECORD_WORKER=1`, `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=1`) as well, it reaches the cap.
- Without the second eye (mono) the frame is the same 11.85 ms, so 2a (eye 1 on its own thread) is worth about as much as the atlas. But eye 1's draws are replays of eye 0's, and 2a is the larger change.
- The game thread is not the limit here: it waits 5.9 to 7.3 ms per frame for the render thread even then. With VR's own game-thread work on top it may become one, which is what the new `[vr] VR work on the game thread` line will show.

### Done: the record worker and uniform reuse on by default on Windows (roadmap #3, 2026-10-26)

**Fidelity: identical.** Three runs from the same state, desktop stereo mode, fixed seeds (`WWHD_TEST_FIXED_SEED=1`), interpolation off: two with the old defaults and one with both switches. Frames 3600 and 4200 were dumped for each eye (`frame_<n>.png`, and the new `frame_<n>_right.png`).
- Frame 4200 was byte-identical in all three runs, in both eyes.
- Frame 3600 already differed between the two runs with the old defaults (a timing-dependent detail of the scene). The run with the switches matched one of them byte for byte.

**Frame times** (`WWHD_VK_STATS=1`, which adds about 1.5 ms to every run alike):

| Run | Frame time | p95 interval | Render-thread CPU |
|---|---|---|---|
| Mono, old defaults | 13.3 to 13.5 ms | 13.4 to 13.6 ms | 10.2 to 10.9 ms |
| Mono, both on | 13.2 to 13.4 ms | 12.8 to 13.2 ms | 9.2 to 10.0 ms |
| Stereo, old defaults | 18.5 ms | 18.3 to 18.5 ms | 13.9 to 15.0 ms |
| Stereo, both on | 18.6 to 18.7 ms | 18.5 to 18.8 ms | 13.0 to 13.5 ms |

The worst frames and the count over 40 ms were the same with and without them, unlike on Android. On their own they add no frames: stereo still gets 2 per step. Their value is with #1: without the sun atlas's work they took stereo from 12.2 to 11.85 ms, the three-per-step cap. With the new defaults and no switches set, the stereo mode runs at 17.2 ms per frame, with render-thread CPU 12.6 to 13.2 ms and 2.9 to 3.1 ms of it idle.

`WWHD_VK_RECORD_WORKER=0` or Graphics > Advanced, and `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=0`, turn them off again. Other platforms keep the old defaults.

### #1, stage 1 done: the sun atlas's draws in a held stream of their own, same thread (2026-10-26)

`WWHD_VK_ATLAS_STREAM=1` (off by default until the threading stages need it). `stereo.cpp` `stereo_atlas_hold`/`stereo_atlas_emit`, as eye 1's held stream:
- At the first atlas draw, the renderer's render pass ends, and the atlas's pass and its own held command stream take over.
- Everything recorded meanwhile goes into that stream, in order.
- It is emitted whole into the renderer's stream before the next other draw. It is also emitted at every `stereo_flush` (submissions, clears and copies through `StereoInOrder`, the swap), at an eye switch, and before `peek_z` and the write-back, which bypass those.

So the GPU gets the same commands in the same order.

**Checked** (desktop stereo mode):
- Pictures: fixed seeds, interpolation off, both eyes.
  - Frame 4200 was byte-identical to before, with the record worker (the POD path) and without it (the exact path).
  - Frame 3600 matched one of its two known variants each time.
- Frame time: 17.3 to 17.6 ms with the stream, 17.3 to 17.7 ms without.

**What it showed** (`[vulkan stereo] sun atlas held stream`):
- All 3,029 atlas draws are held, but the stream is emitted 4 times a frame:
  - by 2 clears or copies inside the block;
  - by 1 submission (the game's mid-frame `GX2Flush`);
  - by the scene's first draw, which ends the block.
- So the atlas's block is draws with clears and a flush between them. Recording it on its own thread (stage 3) must take those too, or run as up to 3 parts.

### The roadmap to 90 fps

**The rule (asked for by the player, 2026-10-26): no change may reduce the game's graphical fidelity.**
- Performance work does the same rendering faster: threads, caching identical results, removing repeated copies.
- It never approximates or skips rendering in play: no shadows reused across frames, no effects at a lower rate, no lower resolution behind the player's back.
- A diagnostic may skip work to measure it, only behind an environment variable and never in play (as `WWHD_VK_SKIP_SUN_ATLAS`).

90 fps needs three frames per logic step, so:
- every frame's render-thread work must be at most about 10.5 ms, from about 16.5 ms now;
- the game thread's work per in-between frame must stay at most about 10.5 ms, with the VR work included.

Estimates marked *measured* come from the numbers above. The others are hypotheses until measured.

**Phase 1: measurement (in progress).**
1. *Done in code, to read in the headset:* the VR work on the game thread. Done when a headset run logs it in normal
   play, in combat and while conducting.
2. Where the repeated vertex copies come from (8.95 MiB copied, 1.84 MiB distinct). Done when each repeat is traced
   to its pass.
3. The game thread's own time per in-between frame in the headset (the proxy's is about 10 ms without VR). Done when
   it is logged per frame alongside the render thread's.

Phase 1 is complete when the headset logs give both threads' times per frame and the VR share.

**Phase 2: the largest render-thread costs.** Each would need confirming in the proxy:

| # | Change | Expected | Cost | Risk |
|---|---|---|---|---|
| 2a | Record eye 1 on its own thread (its draws are replays of eye 0's) | up to 3.5 ms (*measured* upper bound: eye 1 2.7 + flushes 1.1) | high: the upload allocator, image layouts and render-pass state become per thread | wrong or missing eye-1 draws; needs per-eye screenshot comparison |
| 2b | Record the shared sun atlas on its own thread | up to 4.1 ms (*measured* upper bound) | high, as 2a | shadow artefacts |
| 2c | *Ruled out by the rule above unless shown pixel-identical:* draw the sun atlas once per logic step, reusing it in the in-between frames | the in-between frames as the no-atlas runs above (*measured* upper bound: 12.2 ms, or 11.85 ms with the two switches); the logic frame keeps its cost | medium | shadows lag up to two-thirds of a step (22 ms) behind moving casters, and swim if the game's cascades follow the camera between frames. Behind a setting at most, after screenshot checks |
| 2d | Cheaper uniform packing: 74 % of draws change only buffer pointers or ALU constants | part of 2.9 ms (*hypothesis*: the reuse switch above took about 0.1 ms) | medium | wrong constants in a draw |

**Priority, from the upper bounds above:** the sun atlas first, recorded on its own thread (2b), with the record worker and uniform reuse switched on.
- 2b draws exactly the same shadows every frame, so it keeps fidelity.
- 2c would be smaller, but reused shadows can lag behind moving casters and slide with the camera. It is out unless it is shown to be pixel-identical.
- 2a and 2d come after.

**Phase 3: VR pipeline.**
- 3a. Repeated vertex copies (Phase 1 item 2): at most 1.5 ms.
- 3b. Multiview (both eyes in one pass): the largest saving in principle, but a rewrite of the stereo renderer.
  Not before 2a has been tried.
- 3c. The VR game-thread work, if Phase 1 shows it is a large part of the game thread's 10 ms.

**Phase 4: quality and smoothness controls.**
- 4a. An even 45 fps fallback for a 90 Hz headset when three frames per step do not fit. Two logic steps over three
  frames, each two-thirds of a step and shown for two refreshes, instead of 60 fps shown for 1 and 2 refreshes.
  - It removes the judder above, and the runtime's reprojection (SteamVR Motion Smoothing) fills the frames between.
  - It needs a new pass schedule in frame interpolation, as 40 fps has (3 steps over 4 frames).
  - It should be an option: some players may prefer more frames to even ones.
  - Cost: medium to high. Risk: flicker in the effects that interpolation special-cases, as 40 fps had.
- 4b. Internal resolution is already the player's choice. It does not change the frame time (CPU-bound), so it is not
  a lever here.

**Phase 5: validation.** Success means, in the headset:
- 90 frames per second to the headset in normal play, combat and effect-heavy scenes, and in the busiest scene
  (outdoors on Outset);
- frame interval p99 under 16.7 ms;
- missed display periods under 1 % of frames;
- game time per frame 0.333 with under 5 % of frames uneven.

The proxy must reach 85 fps or more (the three-per-step cap at its 28.4 steps per second).

**Where the atlas's draws lie** (`WWHD_VK_TARGET_STATS=1`, `[vulkan stereo] sun atlas order`): one run per frame, the frame's first 3,030 draws (draws 0 to 3,029). The scene's draws, about 2,600, all come after it. So the atlas is a separate block at the start of every frame, and nothing before it in the frame needs to wait for it.

**The next step:** 2b, the sun atlas recorded on its own thread. Its draws need their own share of the renderer's per-draw state: uploads, descriptor and draw-state caches, image layouts and the render pass. Eye 0 already writes and transitions the atlas first (`draw_stereo`), which gives the point where the two threads meet. Validate it in two ways:
- screenshots identical to today's, per eye;
- the desktop stereo mode reaching the three-per-step cap with the two switches on (about 11.85 ms, as measured without the atlas).

Separately, read the `[vr] VR work on the game thread` line in the next headset session.

## Fourth round (2026-10-26): the headset's instability, the regressions, and the frame-rate ceiling

Reported in the headset: the view shook and the arms flickered when the head turned, with or without multiview
draws, worse than it had been. Measured, and from the source. No headset was available for the last part of the
round, so nothing below is confirmed in the headset unless it says so.

### What changed since the stable build

The player named the last stable build: ff93f8d (2026-10-25, 22:41).
- **The VR code since then** (`vr/`, the interpolation, the arms, the camera) gained only timers: the `GameWorkTimer`s
  of round three, two clock reads a call. Nothing that moves the camera or the arms changed.
- **Everything else is in the renderer:**
  - b2f8ee65 switched the record worker and uniform reuse on by default on Windows;
  - this session's atlas-thread, eye-thread and multiview work is off by default, and its default-path cost is below.
- **The stable sessions' logs** (`vr-deps/swings3.log`, `lag2.log`, 2026-10-24) all say
  `[vulkan record worker] disabled`. Every headset session since b2f8ee65 says `enabled`.
- **Their frame pacing was no better:** 48 to 57 fps at 90 Hz, about 270 missed display periods per 10 s. So the uneven
  pacing was already there while it looked stable.

**Changed: the record worker and uniform reuse are off by default again, everywhere** (`record_worker.h`
`record_worker_default`, `draw.cpp` `reuseUniforms`, the overlay's default). The stable configuration is the default
until the headset shows which change caused the instability. `WWHD_VK_RECORD_WORKER=1` (or Graphics > Advanced) and
`WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=1` turn them on.

**Why the worker is the suspect, not proven:**
- **Ruled out in the source:** the headset images released before their commands are submitted. The TV present runs
  under `ProducerPresentationRecording`, which drains the worker and submits on the render thread before
  `vrvk::frame_end` releases the images and calls `xrEndFrame`.
- **Ruled out in the source:** uniform reuse comparing the wrong bytes. It compares a full CPU copy, uploads from that
  copy and starts over with each submission.
- **Possible:** a change of timing. The head pose of a frame is predicted for the render thread's last predicted display
  time plus one display period (`vr_game.cpp` `display_time`, `WWHD_VR_PREDICT_FRAMES=1`). At 60 fps on a 90 Hz
  headset frames are 1.5 periods apart, so that guess is right only some of the time. Each frame is still shown with
  the pose it was drawn with, so rotation is reprojected, but the pose error alternates, and near objects (the arms)
  show it most. The worker made the render thread's frames shorter, which changes how often the guess is wrong.

**Cost on the desktop** (stereo proxy, 2 runs each):

| | frame | render-thread CPU |
|---|---|---|
| worker and reuse on (before) | 17.4 ms | about 14.1 ms |
| off (now) | 17.56 ms | 16.25 ms |
| off, with `WWHD_VK_MULTIVIEW=1 WWHD_VK_MULTIVIEW_DRAWS=1` | 17.57 ms | 14.48 ms |

The frame time does not move: two frames a step either way. Multiview takes back most of the worker's saving.
**Checked:** frame 4200 byte-identical in both eyes with and without multiview draws (`89FDFF9DEFEA1038`,
`33CB7D247CB2B0D0`); the title-screen test passes.

### The default renderer's CPU regression, bisected

Render-thread CPU, the default renderer, two alternating passes, frames from 2400 on, each build as its commit made it
(the record worker on in all of them, as it was then):

| build | render-thread CPU | frame |
|---|---|---|
| b2f8ee65 (before this session) | 13.05 ms | 17.25 ms |
| e91cf6e4 (the atlas stages, their cost mostly removed) | 14.13 ms | 17.38 ms |
| ff75ae7a, 4cd516aa, e04e7b64 (eye thread, two-eye targets) | 13.84 to 13.89 ms | about 17.4 ms |
| d32681ae, 891c7d82 (multiview draws) | 14.14 to 14.22 ms | about 17.4 ms |

- **Where it came from:** the remaining 1.0 ms came with the atlas thread's groundwork (stages 2.1 to 3.3). The eye
  thread and multiview add nothing measurable to the default path.
- **Where it goes:** the draw op is 1.0 ms longer, spread over its phases: uniforms 0.31, render passes 0.22,
  submission 0.11, register writes 0.1, the others under 0.1 each. In the profile it is `draw` itself (+3.3 % of the
  thread's samples), not one new function: per-thread state lookups and the checks the draw threads need, paid on
  every one of 5,600 draws a frame.
- **Not fixed yet:** it moves the frame time by 0.1 ms in the proxy. The candidates are making the per-thread state
  compile to plain globals when no draw thread can start, and taking the stereo checks out of each draw.

### Why the frame rate is two frames a step

Measured with the `[gx2] DrawDone` trace (`WWHD_TRACE_DRAWDONE=1`) and the profiler:
- **The game thread** produces a frame's commands in about 9.9 ms. It calls `GX2DrawDone` twice a frame, both from one
  guest function (02035118's callers 0274C994 and 0274C908): right after the swap (waits 1.8 ms) and when the frame is
  queued, 11.7 ms in (waits 5.7 ms).
- **The render thread** needs about 15 ms of work a frame (14.85 ms of draws for 5,664 draws, 2.1 ms of register
  writes). The game waits for it; it is the limit.
- **The GPU** is busy about 3.4 ms of a frame (submission timestamps), so it is not the limit.
- **Frames come in whole steps:** two a step until a frame fits 11.1 ms. Without the `DrawDone` waits the frame would be
  at best about 15 ms: still two a step.
- **What moves the render thread down:**

  | configuration | render-thread CPU |
  |---|---|
  | default | 14.9 ms |
  | multiview | 13.8 ms |
  | multiview + the atlas thread | 12.4 ms |

  With both, the frame still waits about 4 ms at the atlas join, the shared cache locks and for commands, and the
  in-between pass stays about 15 ms.

### The frame-rate targets (70 to 90 fps) and the adaptive policy

Not implemented, on purpose:
- **What a 90 Hz headset shows evenly:** frames that each stay a whole number of refreshes, which means 90, 45 or 30 fps.
- **Rates in between (70, 75, 80, 85) alternate:** 1 and 2 refreshes, the same judder as today's 60 fps. A target among
  them would show more frames and not be smoother.
- **120 Hz (tried this round):** the headset accepted 120 Hz, but the game still made 60 fps. Its frames do not fit 16.7 ms
  (about 17.5 ms), so they missed refreshes (228 in 119 frames). Back at 90 Hz.
- **The even fallback is 45 fps** (roadmap 4a): two steps over three frames, each two refreshes. For even motion its frames
  must sit at thirds of a step.
  - **What it needs:** every step records its exact state for the next step's blends (`interp.cpp` `record_pass`; the
    effects, the sea, wood, particles, attachments and the camera stamp their histories with it). A frame at 2/3 of a
    step that also leaves the record for later is a new pass kind.
  - **The risk:** flicker in the effects that interpolation special-cases. Not to be done without the headset to judge it.
- **An adaptive policy** then has two choices on a 90 Hz headset: 90 when three frames fit, else 45. The pacer already
  draws as many in-between frames as fit (`plan_in_between`).

### For the next headset session

1. This build: do the view and the arms stay steady when the head turns, as at ff93f8d? If yes, the record worker or
   uniform reuse caused it. `WWHD_VK_RECORD_WORKER=1` alone, then `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=1` alone, says which.
2. The same with `WWHD_VK_MULTIVIEW=1 WWHD_VK_MULTIVIEW_DRAWS=1`.
3. If it still shakes: `WWHD_VR_PREDICT_FRAMES=2` (the pose predicted two periods ahead) and `WWHD_VR_TRACE_LAG=1` (the
   hands' lag and spikes, every 2 s).

### The pipeline, by thread (what each part does and when)

| Part | Thread | Runs | Waits for |
|---|---|---|---|
| Game logic (30 Hz step: actors, collision, input) | game | once per step, on the logic pass | its step slot (`interp.cpp` `paced_pass_start`) |
| VR input, the arms' IK (`vr_game.cpp` `filter_pad`, `link_arms` in Link's execute) | game | once per step | the head and controller poses predicted for `display_time()` |
| The head-tracked camera (`CameraOverride` in `camera_draw`) | game | every displayed frame (logic and in-between passes) | the head pose for `display_time()`: the render thread's last predicted display time + 1 period |
| In-between frames (hold passes: the game's draw only, models and camera blended at `pass_t`) | game | n per step, as many as fit (`plan_in_between`) | the step's schedule |
| GX2 commands (registers, draws, `GX2DrawDone`) | game, to the render thread's queue | every frame | `GX2DrawDone` twice a frame: at the swap, and when the frame is queued |
| The stereo renderer (eye 0, eye 1 replay or multiview, the sun atlas) | render | every frame, per draw | the atlas's thread at its join (with `WWHD_VK_ATLAS_THREAD=1`) |
| Vulkan recording and submission | render, or the record worker (`WWHD_VK_RECORD_WORKER=1`) | every frame | the worker's queue when it is full |
| The headset (`vrvk::frame_begin`: `xrWaitFrame`, `xrBeginFrame`; the eyes blitted into the swapchains; `frame_end`: release, `xrEndFrame`) | render | every frame, at the swap | `xrWaitFrame` (the headset's refresh) |
| Shader compiles, pipeline compiles, multiview twins | their own threads | when new | nothing on the frame's path |
| GPU | | about 3.4 ms of a frame | |

### Where a frame's time goes (desktop stereo proxy, 120 fps asked for, 2026-10-26)

Two runs each, frames from 2400 on; the in-between pass is the game thread's time for an in-between frame,
waiting included:

| Configuration | Frame | Render-thread CPU | Draw ops | In-between pass |
|---|---|---|---|---|
| default (worker off) | 17.39 ms | 15.79 ms | 15.76 ms | 15.8 ms |
| multiview draws | 17.31 ms | 13.99 ms | 14.89 ms | 14.2 ms |
| + the atlas's thread | 17.59 ms | 13.45 ms | 10.16 ms | 15.0 ms |
| + record worker and uniform reuse | 17.48 ms | 11.87 ms | 10.48 ms | 13.9 ms |

All stay at two frames a step.

**In the fastest configuration** (`WWHD_INTERP_PASS_STATS=1`, `WWHD_TRACE_DRAWDONE=1`, the thread sampler):

The game thread is not the limit:
- its CPU time is 3.75 to 4.0 ms on a logic pass and 0.83 to 0.96 ms on an in-between pass;
- an in-between pass still takes 14.3 ms, because the game waits at `GX2DrawDone`: 1.8 ms at the swap, 5.3 ms at
  the end of the frame.

The render thread is busy for about 15.6 ms a frame inside its operations, of which 12 ms is CPU. It is blocked for
the rest:

| What it waits on | Share of its samples | About |
|---|---|---|
| The atlas's thread at the join | 4.9 % | 0.85 ms |
| The record worker's full queue (`RecordWorker::publish`, from `stereo_flush`) | 4.0 % | 0.7 ms |
| The cache locks shared with the atlas's thread | 2.4 % | 0.4 ms |
| Retiring submissions and presenting | 1 to 2 % | about 0.6 ms |

It also idles about 2.5 ms a frame waiting for commands.

**Two waits serialize every frame:**
- **At the end of a frame,** `GX2DrawDone` makes the game thread wait until the render thread has executed everything
  queued.
- **At the start of the next,** the render thread waits for the game's first commands.

So the frame is the render thread's whole work, not what is left after the game thread's.

### What 90 fps needs (measured, then estimated)

- **Measured:** the fastest configuration's render-thread CPU, 12 ms, is already over the 11.1 ms of a 90 Hz frame,
  before any waiting. 90 fps needs the render thread's CPU at about 9 ms a frame, about 3 ms below the fastest
  configuration, and its blocking (about 2.6 ms) gone.
- **Estimated:** the frames must also overlap across `GX2DrawDone`. The game would go on with the next frame's commands
  while the render thread finishes the last one. That is safe only once the render thread no longer reads guest memory
  that the game rewrites after `GX2DrawDone`: the vertex data, uniform blocks and display lists the queued commands
  name. `GX2DrawDone` is there for exactly that (docs/VR_ATLAS_THREAD.md, stage 2.6b).
- **Not enough:** the existing switches. Each removes part of the render thread's work, but none removes the
  serialization, and together they stop at 13.9 ms a frame.

**The candidates, by measured size:**

| Candidate | Saves | Cost | Note |
|---|---|---|---|
| Per-draw CPU | the 5,600 draws a frame at about 2.6 us each | | the largest pool: uniforms 3.0 ms, render passes 2.1 ms, shaders 1.6 ms, vertices 1.5 ms, recording 1.3 ms, textures 1.3 ms, submission 1.2 ms, descriptors 1.1 ms |
| The atlas join and its locks | about 1.25 ms in the fastest configuration | | |
| The record worker's back-pressure | about 0.7 ms | | its queue fills on eye 1's held commands |
| Overlapping frames across `GX2DrawDone` | the 2.5 ms idle, and the end-of-frame wait | the largest change | needs the render thread to copy what the game rewrites, before the wait returns |

### The frame-rate target, as implemented

The pacer already adapts every step:
- **It plans** as many in-between frames as fit the step, from the recent durations of logic and in-between passes
  (`interp_pacing.h` `plan_in_between`, `next_pass`).
- **It is capped** at what the headset shows evenly (`cap_fps_vr`: 90 fps on 88 to 117 Hz, three frames a step).
- **It drops** to two frames a step when three do not fit, and goes back to three as soon as they do.
- **It recovers from hitches:** one hitch does not lock in-between frames out (`update_average`, `probe_estimate`).

**New:** every 300 steps the log says the target, what was drawn and why
(`[interp] target 90 fps (3 frames a step, 11.1 ms each): 2.00 frames a step drawn, 60 fps; logic pass ..., in-between
pass ...: the in-between pass is over its time`).

**What it does not do:** 70 to 85 fps, and an even 45 fps. Why:
- **On a 90 Hz headset** rates between 45 and 90 are shown unevenly (1 and 2 refreshes in turn), as 60 fps is today.
- **An even 45 fps** needs a pass that draws at 2/3 of a step and leaves the step's record for later. Every
  interpolated effect stamps its history by the record pass, so that is a change to all of them. It is designed above,
  not done without the headset to judge flicker.
- **A headset at 72, 80 or 120 Hz** would make other rates even. The cap already follows the headset's rate.

### To reproduce

All runs use `stereoMode=2`, the profile and saves copied from the last session on Outset (`run.ps1`) and:
- the environment `WWHD_PROFILE=1 WWHD_INTERP_FPS=120 WWHD_DISPLAY_HZ=120 WWHD_VK_PRESENT_MODE=immediate`;
- a 4,200-frame run, summed from frame 2400 on;
- a timing run averaging under 35 fps fails and is closed.

The switches:
- `WWHD_VK_MULTIVIEW=1 WWHD_VK_MULTIVIEW_DRAWS=1`: multiview draws;
- `WWHD_VK_ATLAS_STREAM=1 WWHD_VK_ATLAS_THREAD=1`: the atlas's thread;
- `WWHD_VK_RECORD_WORKER=1 WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=1`: the record worker and uniform reuse;
- `WWHD_INTERP_PASS_STATS=1`: the game thread's CPU time per pass;
- `WWHD_TRACE_DRAWDONE=1`: the `GX2DrawDone` calls;
- `WWHD_SAMPLE_THREADS=<prefix>` (with `WWHD_SAMPLE_AFTER` and `WWHD_SAMPLE_SECONDS`): the thread sampler, read with
  `prof_diff.py`.

**Checked:** pictures byte-identical in both eyes at frame 4200 for the default, multiview draws, and multiview draws
with the atlas's thread.

## Multiview and the atlas thread by default (2026-10-27)

Multiview draws (`WWHD_VK_MULTIVIEW`, `WWHD_VK_MULTIVIEW_DRAWS`) and the sun atlas's thread (`WWHD_VK_ATLAS_STREAM`,
`WWHD_VK_ATLAS_THREAD`) are on by default now, except on Android, where `=1` turns each on; multiview alone is on
by default on a Quest again (2026-10-31, docs/VR_ANDROID.md, "Standalone frame rate"). `=0` turns each one off
(`stereo_switches.h` `env_default_on`; `per_thread.h` `kPossible` follows the atlas thread's default).

Measured in the desktop stereo mode, as in round three (3x, 120 fps asked for, no vsync, from the same copies of the
last session on Outset, standing still), two runs each, frames from 2400 on:

| | Frame | Frames/s | Render-thread CPU | In ops | Idle | `DrawDone` wait | In-between pass |
|---|---|---|---|---|---|---|---|
| Before (both off) | 17.34 to 17.38 ms | 57.5 to 57.6 | 15.48 to 15.63 ms | 19.2 to 19.3 ms | 1.2 to 1.3 ms | 7.3 to 7.5 ms | 13.8 to 17.1 ms |
| After (both on) | 17.36 to 17.49 ms | 57.2 to 57.6 | 14.12 to 14.17 ms | 16.4 ms | 1.9 ms | 7.1 to 7.2 ms | 13.9 to 14.3 ms |

The render thread does about 1.4 ms less CPU work and spends about 2.9 ms less in its operations. The frame time does
not change: still two frames a step. Pictures were not compared this time (frame 4200 was byte-identical with both in
round four). Not yet seen in the headset.

### Tried after that, no gain (reverted, 2026-10-27)

Same desktop stereo runs, with multiview and the atlas thread on, alternating before and after:

- **The render thread's per-thread state in plain globals while the atlas thread runs** (told from the other draw
  threads by its stack range): render-thread CPU 14.0 to 14.2 ms before, 14.4 to 14.9 ms after; frame unchanged. The
  1.0 ms of round four was measured with the atlas thread off, when that state was already in globals: it is the per-draw
  checks, not thread-local reads.
- **The register writes forwarded to the atlas thread packed into one job per run, in a buffer grown without filling**:
  the register op 3.1 to 2.8 to 2.95 ms; render-thread CPU, the join (0.7 ms) and the frame unchanged. The atlas thread
  has 3,030 draws to make while the scene reaches its first draw that samples the atlas after about 1,070: the join
  waits for that. Lock waits are about 0.13 ms a frame.

What these show: the render thread now sheds about 1.5 ms through the switches, but the frame does not move while the
game thread and the render thread take turns at `GX2DrawDone`. Overlapping them is the change that would turn render-thread
savings into frames.

## PERF-07a: the swap's GX2DrawDone skipped when it protects nothing (2026-10-27)

The game calls `GX2DrawDone` twice a frame, both from 02035118: right after the swap (0274C994) and when the frame is
queued (0274C908). `GX2DrawDone` makes the game wait until the render thread has executed everything queued, so the game
may then rewrite the memory those commands read. `WWHD_TRACE_DRAWDONE=1` now also logs the commands queued before each
call. Before the swap's: register writes, a scan copy, 2 context switches, 2 invalidates, a flush and the swap.
None of them reads guest memory on the render thread. A context switch selects the render thread's shadow of the
context; an invalidate only marks surfaces dirty and hints the buffer cache's write watch.

**Changed** (`gx2_core.cpp`):
- Each queued command is classed when it is queued: "guarded" if the render thread reads guest memory for it or may
  write it back (draws, display lists, copies, clears, depth peeks, a full GPU wait), else not. Unknown commands are
  guarded.
- A `GX2DrawDone` with no guarded command queued since the last completed wait queues its `OP_DRAW_DONE` and returns
  without waiting. That is the swap's, every frame: the game goes on with its next frame while the render thread presents.
- `WWHD_GX2_SKIP_SAFE_DRAWDONE=0` always waits, as before.
- VR: the head pose for the frame being built is predicted one more display period ahead for each swap still queued
  (`gx2::swaps_in_flight`, `vr_game.cpp` `display_time`). The render thread predicts at each swap (`xrWaitFrame`), and
  the game may now build its next frame before that.

**Measured** (desktop stereo, the same runs as before, alternating, `WWHD_TRACE_DRAWDONE=1` in all four):

| | Frame | Frames/s | Render-thread CPU | The swap's wait | The frame end's wait | In-between pass |
|---|---|---|---|---|---|---|
| Before (`=0`) | 17.40 to 17.44 ms | 57.3 to 57.5 | 13.8 to 14.1 ms | 1.14 ms | 5.72 ms | 14.1 to 14.3 ms |
| After | 17.38 to 17.72 ms | 56.5 to 57.5 | 13.9 to 14.1 ms | 0 (skipped every frame) | 6.46 ms | 13.3 to 13.6 ms |

- The swap's wait is gone, but the time moves to the frame end's wait: the render thread is the limit, and the frame
  stays two a step.
- The in-between pass is about 0.8 ms shorter, still over the 11.1 ms a third frame needs.
- **Pictures:** frame 4200 byte-identical in both eyes with and without it (`89FDFF9DEFEA1038`, `33CB7D247CB2B0D0`).
- **Not yet seen in the headset**, where the swap's wait also held the game through `xrWaitFrame`.

## PERF-05: the record worker and uniform reuse on by default on Windows again (2026-10-27)

They were off from the fourth round, when the view shook and the arms flickered in the headset. Now they are on again
with multiview, the atlas thread and PERF-07a (`record_worker.h` `record_worker_default`, `draw.cpp` `reuseUniforms`,
the overlay's Graphics > Advanced default). `WWHD_VK_RECORD_WORKER=0` and `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=0` turn
them off. The player tested them in the headset (PERF-01: head turns and stick turns, empty hands and the sword) and
kept them.

## Desktop Linux: the core groups and uniform reuse as on Windows (2026-11-04)

Two Windows-only defaults are now on for desktop Linux on x86-64 too (Android and the Steam Frame unchanged):
- **The renderer's threads on the CPU's core groups** (`platform/core_groups.h`, `docs/VR_SCENE_THREADS.md`): the L3
  groups and cores read from `/sys/devices/system/cpu/cpu*/cache` and `topology`, the threads placed with
  `pthread_setaffinity_np`. `WWHD_VK_PLACE_THREADS=0` turns it off. On a CPU with one L3 for all its cores nothing is
  placed. The log's `[threads] core groups:` line shows the masks.
- **Uniform reuse** (PERF-05, `draw.cpp` `reuseUniforms`): `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS=0` turns it off.

Not yet measured on Linux.

## Linux frame pacing (2026-10-27)

Measured on Linux (CachyOS, Ryzen with amd-pstate-epp, RTX 5080, driver 615.78, Wayland), unattended, in desktop
stereo at the headset's frame schedule. No headset frames are measured here: an unworn PS VR2 keeps the session
unfocused and the runtime takes none.

**The method** (repeatable; the scripts are not in the repository):
- The same frozen state (a copy of `last_session.wwstate`, the Great Sea, Link on foot), settings and saves in a
  separate `WWHD_USER_DIR`, `WWHD_AT_START=last`, `WWHD_NO_CONTROLLERS=1`, `WWHD_AUDIO_VOLUME=0.02`.
- **The 90 Hz schedule without a headset:** `WWHD_INTERP_FPS=120 WWHD_INTERP_VR_HZ=90` (new test aid, `interp.cpp`:
  the headset's rate cap, 3 frames a step at 11.1 ms), `WWHD_VK_PRESENT_MODE=immediate`. The log then says
  `target 90 fps (3 frames a step, 11.1 ms each)`.
- Three scenarios, 80 s each, scripted from TV frame 2400: standing still, the camera turning (`WWHD_RSTICK`, the
  stand-in for head rotation), walking while it turns (`WWHD_STICK` + `WWHD_RSTICK`). One warm-up run first.
- `WWHD_VK_HANDOFF_TRACE`: the TV window's present-to-present intervals over the last 30 s of each run, and the fence,
  acquire and present waits.

**What it showed:**
- **No spikes:** the longest frame was about 22 ms (one 48 ms frame in six baseline runs). Fence waits p99 4.1 ms,
  acquires and presents under 0.1 ms: no synchronization stalls.
- **Uneven cadence:** frames 11 or 22 ms apart, 19 to 44 % of them on the long side. The pacer's own line says why:
  the in-between pass took 11.6 to 14.2 ms against 11.1, so 2.1 to 2.5 of the 3 frames a step were drawn.
- **Not the scheduler:** the render thread waited 1.4 ms a second for a CPU (`/proc/<pid>/task/*/schedstat`), so the
  nice boost Linux refuses without CAP_SYS_NICE (`[thread priority] ... setpriority error=13`) costs nothing here.
- **Not the GPU:** 2.72 GHz in P0, 27 to 37 % busy, 125 W, no clock changes (`nvidia-smi`).
- **Not the implicit Vulkan layers:** MangoHud, the frame generation, HDR and gamescope layers are listed but none is
  loaded (their environment switches are not set).

**Changed: the record worker is on by default on desktop Linux** (`record_worker.h`), as on Windows since PERF-05;
`WWHD_VK_RECORD_WORKER=0` or Graphics > Advanced turns it off. Android stays off.

| | standing still | the camera turning | walking, turning |
|---|---:|---:|---:|
| fps, worker off (2 runs) | 63.4 / 61.8 | 74.8 / 74.5 | 74.2 / 75.3 |
| fps, worker on (2 runs) | 74.4 / 80.4 | 76.6 / 75.3 | 75.4 / 77.4 |
| frames over 1.5x the budget, off | 760 / 816 | 422 / 425 | 436 / 392 |
| frames over 1.5x the budget, on | 398 / 216 | 360 / 401 | 401 / 342 |
| p99 / max interval (ms), off | 20.4 / 21.6 | 20.2 / 22.1 | 20.2 / 47.9 |
| p99 / max interval (ms), on | 20.5 / 21.7 | 20.5 / 21.8 | 20.4 / 23.3 |

It shortens the render thread's passes, so more steps get their third frame. p99 stays near 20 ms: the remaining long
frames are still the 2-frames-a-step slots, so 90 fps is not held in this scene.

**The 45 fps mode** (`WWHD_INTERP_FORCE_45=1`, the same scenarios): standing still every frame was 22.2 ms apart
(p99 24.2, max 25.7), even. Walking and turning it fell to 30 fps, 27 to 47 ms apart: its logic pass took 35 ms
(`target 45 fps ...: logic pass 35.0 ms`). Not checked further.

**Still to do in the headset:** the `[vr] frame times` lines (intervals, missed display periods, render thread after
`xrWaitFrame`) with the worker on, and how it looks (the fourth round's shaking was never tied to the worker).

### Round 2 (2026-10-27): the submission ring, the 45 fps lock, auto

**Changed: 6 submission slots on desktop Linux and Windows** (`backend.h` `submission_slots`, 4 on Android and macOS;
`WWHD_VK_SUBMISSION_SLOTS=2..16`). Windows makes the same submissions; measured on Linux only (Windows: compare
`render thread waits ms/frame: GPU` with `WWHD_PROFILE=1`, and `WWHD_VK_SUBMISSION_SLOTS=4` against the default). With about 4 submissions a frame, 4 slots reached back to the previous frame: every
GPU wait of the render thread (`WWHD_VK_HANDOFF_TRACE`, tagged by a local diagnostic) was for a slot 3 submissions
back, a median 3.2 ms a frame, while the GPU's own work was 3.4 ms a frame (`WWHD_VK_GPU_TIMESTAMPS=1`). More slots
let the render thread record a frame while the GPU draws the last: its GPU wait went to 0 and its time in operations
from 11.75 to 8.78 ms a frame.

| unlocked, record worker on | 4 slots | 6 slots | 8 slots |
|---|---:|---:|---:|
| standing still: fps / p99 ms / frames over 1.5x | 80.4 / 20.1 / 216 | 89.9 / 13.0 / 0 | 89.9 / 12.9 / 2 |
| the camera turning | 75.3 / 20.5 / 401 | 89.1 / 15.6 / 14 | 88.2 / 16.9 / 34 |
| walking, turning | 77.4 / 20.1 / 342 | 89.0 / 16.5 / 26 | 88.3 / 17.2 / 36 |
| memory (RSS) | 2.33 GiB | 2.92 GiB | 3.11 GiB |

Each slot keeps its own upload blocks (32 MiB each) and descriptor pools. 5 slots was not measured.

**Fixed: the 45 fps mode locked at 30 fps** (`interp_pacing.h` `one_frame_budget_40`). Without in-between frames a
logic pass is measured from one step to the next, a whole step, and the 45 (and 40) fps budget is 0 while that
average is a step or more: after the load of a scene took it there, no in-between frame was drawn again (1 run of 2:
`paced: 0% of in-between frames drawn`, "logic pass 35 ms"). The pacer's probe (after 10 s without in-between frames)
now gets its frame through when in-between passes take less than a step; the logic pass it ends brings the average
back. A device whose in-between passes take a step or more is left as before (the 40 fps simulations in
`interp_pacing_test` unchanged). After: 4 runs of 4 at an even 45 fps walking and turning (p50 22.25, p99 25 ms).

**Added: `vr.frameRate=auto`** (docs/VR_FRAME_RATE.md). With 4 slots it held an even 45 where unlocked could not hold
90; with 6, unlocked fits 97 to 99 % of its frames and auto switches back and forth, less even than unlocked. Not the
default.

**Test aids added:** `WWHD_INTERP_VR_HZ=90` (the headset's frame schedule on the desktop), `WWHD_INTERP_VR_MODE=0..3`
(vr.frameRate without a headset).

## Fifth round (2026-10-28): stalls, the planner, and the head pose

**Baseline, the player's headset (PS VR2 on SteamVR, Linux, RTX 5080, 3400x3468 per eye):**

| VR frame rate | interval p50 / p95 / p99 / max | missed display periods per 600 frames |
|---|---|---|
| 90 fps | 11.12 / 11.33 / 11.67 / 12.81 ms | 0 (xrWaitFrame waits 3.1 ms a frame: headroom) |
| 45 fps | 22.2 / 22.8 / 23.2 / 23-28 ms | 0-2 |
| 60 fps on the 90 Hz headset | 16.7 / 17.5-18.1 / 17.8-23.3 / up to 125 ms | about 300: uneven by its nature |

The jitter of that session was the 60 fps choice (one frame shown for one refresh, the next for two). VR's own work on
the game thread is 0.14 to 0.19 ms a frame.

**Pipeline warm-up across builds (changed).** Measured on the desktop with `WWHD_VK_STUTTER_TRACE`, the Dragon Roost
save, 80 s: the saved recipes were ignored after every build ("local recipes ignored: incompatible pipeline recipe
database"), because their fingerprint hashed every runtime source file and the commit. Gameplay or VR edits threw the
warm-up away. The identity (`cmake/PipelineRuntimeIdentity.cmake`) now hashes only the code that makes pipelines
(`runtime/src/gfx`, `runtime/src/gx2`, `runtime/include`, the Cemu translator, `CMakeLists.txt`), and the commit and
version are no longer part of it (`backend.cpp`). This is safe because the handoff (`take_warm_pipeline`) compares each
warmed pipeline's SPIR-V, descriptors and whole fixed state with the draw's, and builds afresh on any difference.
- Cold (recipes ignored): 310 guest pipelines compiled during the run, 17 ms in all, at most 0.4 ms each (the NVIDIA
  driver's own cache has them).
- Warm (recipes used): all 310 ready at start, 2 draws waited 0.2 ms in all.
- On this PC compiles are not what stalls: the long frames (134-146 ms, 6 or 7 a run) are the title's first frames and
  the stage load in both runs, and none came in the 66 s of play after it. The change matters most where the driver
  has no cache of its own (Android, Quest) and after each build here.
- Not changed: VR's multiview twins (`multiview_pipeline.cpp`) are built without a pipeline cache, on their own thread;
  until one is ready its draws draw each eye on their own, so they never stall. Giving them the shared cache needs its
  merges serialised with the builder; left for later.

**The planner after a hitch (measured, not changed).** A 150 ms hitch (`WWHD_INTERP_TEST_HITCH=2400:150`) at 90 fps with
a simulated 90 Hz headset (`WWHD_INTERP_VR_HZ=90`, `WWHD_INTERP_VR_MODE=5`, mailbox presentation): 62 fps in the half
second of the hitch, then 45 frames of 11.1 ms in every half second after. The averages recover within half a second.
The 10 s windows of the earlier session at 62 fps were not reproduced on the desktop and need a headset trace
(`WWHD_TRACE_STEPS`, `WWHD_VK_STUTTER_TRACE`).

**Switching the frame rate (measured, not changed).** 45 to 90 fps during play (`WWHD_INTERP_VR_MODE_AT=2400:5`): the
first half second after the switch drew 10 frames at 11.1 ms, every half second after drew 45. The estimates from the
old rate are replaced within a few passes, so resetting them on a switch would gain nothing measurable.

**One head pose a frame (changed).** The game thread located the head's views up to four times a frame for the same
predicted display time (the pad's input, the camera, the pad filter, the arms), each an `xrLocateViews` and an
`xrLocateSpace`. `game_views` (`vr_game.cpp`) shares them: the camera, which is what the headset shows, always locates
afresh and keeps its result; the others reuse the latest for that time and so agree with the view. The render thread
still locates its own. Expected: a few hundredths of a millisecond a frame. Needs the headset's "VR work on the game
thread" line to confirm.

**Test options added:** `WWHD_INTERP_VR_MODE_AT=step:mode` (the VR frame rate changed during a run) and
`WWHD_INTERP_TEST_HITCH=step:ms` (one slow logic pass), both in `interp.cpp`.

## Frames while the game is busy (2026-10-28)

**The problem.** The headset was given a frame only when the game finished one (`swap()`: `frame_begin`, the TV
present, `frame_end`). Whenever the render thread waited (a shader or pipeline made the first time it is needed) or
idled (the game thread loading a stage, or stalled), nothing went to the headset: SteamVR logged the game timing out
in every session (58 to 1,743 times a session, 5 sessions on 2026-10-27/28) and the view froze or greyed. The desktop
window had a fallback (`present_completed_frame_while_pipeline_pending` shows the last frame again); the headset did
not.

**The change** (`vr_vulkan.cpp` `headset_frame_due`, `headset_frame`; `renderer.h`; `gx2_core.cpp`; `backend.cpp`).
Once the next frame is half a display period late (16.7 ms at 90 Hz), the render thread gives the headset its last
frame again: `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` with the last frame's layers. They name the same
swapchains, whose last released images OpenXR shows, at the poses they were drawn for, so the runtime reprojects them
to the head's movement. Nothing is drawn or copied. It runs from two places: the render thread's wait for commands
(a timed wait instead of an open one while a session runs) and the wait for a shader or pipeline (with the desktop's
repeat). It is never due while one of the game's frames is under way, so those are not delayed, and the graphics
queue is taken only if free (`try_lock`: another thread may hold it while waiting for the render thread). The 10 s
`[vr] timing` line counts them ("frames shown again while the game had none ready").

**Not checked:** in the headset. Expected: SteamVR's timeouts in loads and first-use stalls replaced by frames at the
headset's rate; the world holds still for the stall, the head tracking does not.

**Fixed (2026-10-28): it fired in normal play.** Half a period late was 1.5 periods from the last frame. At 45 fps
the game's frames come 2 periods apart, so the last frame was shown again before nearly every new one: 151 to 212
every 10 s in a headset session, with 309 missed display periods in 163 frames and intervals of p50 14.2 ms instead of
22.2.
- Each repeat took a display slot (`xrWaitFrame`), so the next real frame came a period later and its head pose was
  predicted for the wrong time.
- During a stick turn, the repeat showed the world at the last turn's angle. The runtime reprojects head movement, not
  the stick's turn.
- The world jerked and swam with head and stick turns (reported in the headset: "practically unplayable").
- At 60 fps on 90 Hz the frames are exactly 1.5 periods apart, and a few went the same way.

Now it waits for the longer of 3 display periods and twice the game's recent interval between its own frames (a running
average, `g_gameFrameInterval`, taken in `frame_end`; gaps over 200 ms not counted). That is about 44 ms at 45 fps and
33 ms at 60 and 90 fps, so it fires only in a real stall, which lasts hundreds of milliseconds. Expected in the log:
0 frames shown again in normal play.

## Draws left out while their pipeline is made (2026-10-28)

**The problem.** A draw whose pipeline is new (its shaders' SPIR-V compiled and the pipeline built on worker
threads) made the render thread wait for it (`resolve_pending_pipeline`, wait). The SPIR-V compile is about 2 ms
typically and up to about 110 ms for a large shader; in the headset that wait was a freeze. The one freeze in the
2026-10-28 16:35 session (125 ms, logic 29.5 steps/s) came at the session's first sword draw, with its glow, while new
pipelines appeared.

**The change** (`draw.cpp` `skip_while_made`). In a VR session the draw's pipeline is checked without waiting; still
being made, the draw is left out of this frame (it appears a frame or two later, once made, and is then drawn as
always). The decision holds for the other eye in the same frame, so the eyes never differ. The desktop keeps every
draw. Logged every 15 s of frames while it happens ("draws left out so far while their pipeline was made").

**Trade-off:** something appearing for the first time (a new effect, an enemy) may be missing for a frame or two, once
per new pipeline: the SPIR-V and pipelines are cached on disk, so it does not repeat in later sessions.

**Turned off (2026-10-28), after it crashed the game in VR at its start** (the record worker, replaying the
recorded commands, crashed in the driver). Cause: a draw whose pipeline is pending is resolved by the record worker
when it replays the draw (`record::emit_owned`); `skip_while_made` resolved the same pipeline on the render thread at
the same time, taking its result, under a lock (`CacheLock`) that is only taken while a second draw thread runs, never
against the record worker.

**On again (2026-10-31), only looking.** `skip_while_made` no longer resolves anything:
- `PipelineDemandTicket::ready()` says whether the result is there without taking it (the ticket's own mutex).
- `Pipeline::watch`: a second handle to the same compile, set with `pendingCompile` when the pipeline is created
  (before any deferred bind exists) and reset only by the draw threads once they see it ready. The record worker
  changes `pendingCompile`, never `watch`, so the draw threads never read what the worker writes; `watch` also keeps
  the ticket alive while they look. Set only while skipping is on.
- Once made, the draw goes the usual way, and whoever binds it (the render thread, or the worker's deferred bind) takes
  the result, exactly as without this.

On by default in a VR session; `WWHD_VK_SKIP_PENDING_DRAWS=0` turns it off, `=1` also outside VR (the desktop stereo
mode, for tests).

**Measured** (desktop stereo with `=1`, the audit route below: Outset, walking on with the camera turning, a cold
pipeline cache so 25 new pipelines are made in play, `WWHD_VK_STUTTER_TRACE`):

| | Worst frame | Over 33 ms | Over 16.7 ms | Pipeline waits | Draws left out |
|---|---|---|---|---|---|
| Waiting (before) | 58.7 ms | 2 | 11 | 25 | 0 |
| Left out, run 1 | 22.8 ms | 0 | 5 | 0 | 6 |
| Left out, run 2 | 17.1 ms | 0 | 3 | 0 | 116 |
| Warm cache, left out (nothing to leave out) | 17.5 ms | 0 | 2 | 0 | 0 |

- No crash in any run, with the record worker on (its default).
- **Pictures:** frames 3000, 3600 and 4200 of both eyes and the GamePad byte-identical with and without (fixed seeds,
  interpolation off, 117 draws left out in that run): what was left out is drawn once made.
- Not yet seen in the headset.

## Thread hand-offs and frame-time spikes, audited (desktop stereo, 2026-10-31)

Desktop stereo on Linux (RTX), the headset's picture size (`WWHD_ASPECT=1.3859 WWHD_RES_SCALE=3`) and frame schedule
(`WWHD_INTERP_FPS=120 WWHD_INTERP_VR_HZ=90`), `WWHD_VR=0`; the Last session of `vr-test/return.wwstate` (the Great Sea),
warped to Outset (`WWHD_TEST_WARP_AT=2000:sea:44:0`), walking onto the island with the camera turning
(`WWHD_STICK=2400-12000:0:-0.8 WWHD_RSTICK=3000-12000:0.25:0`), 150 s, about 10,000 frames after frame 3000. The
desktop view is the game's third-person camera: about 900 to 1,500 draws a frame, and 90 fps held (median 11.13 ms,
p99 12.1 ms). The headset's busy first-person scenes (2,000 to 7,000 draws) are render-thread bound; not measured here.

**Spikes**
1. **New pipelines (the largest cause).** The worst frames (48.8 and 58.7 ms) were the draw threads waiting for the
   shader-compile thread, one new pipeline after another (up to nine waits in a frame, up to 9.3 ms each). Start-up
   preparation had made the 824 saved pipelines; the route needed 139 more. With those cached, the same route peaked at
   18.5 ms (11 frames over 16.7 ms became 1). Addressed: draws are left out while made (above).
2. **Two SPIR-V compiles at once.** The game's compiles took 2 to 6 times their CPU time in wall time (e.g. 5.9 ms for
   0.9 ms of CPU) whenever the renderer's own shaders compiled at the same moment on another thread. The lock is ours:
   every compile takes one `glslangMutex` (`shader_compiler.cpp`). The renderer's own compiles (no frame: the
   multiview twins of the game's shaders, built on their builder thread at a pipeline's first two-eye draw; the
   surfaces' and the presenter's shaders) were never kept: 460 to 567 of them in every run, warm cache or not, each
   holding the lock. Addressed (below, "The renderer's own shaders kept on disk").
3. **Each stall becomes half a second of uneven frames.** After both spikes, 3 to 4 frames alternated about 21 and
   12 ms while the game caught up its 30 Hz logic steps. Not yet addressed.
4. With a warm cache, a few 13 to 18 ms frames stayed at the same place (stepping onto the island), with no compiles:
   other first-visit work (textures or surfaces), not traced.

**Hand-offs**
1. **The frame end's `GX2DrawDone` (`0274C908`)** made the game thread wait 2.3 ms every frame (never skipped: the
   draws queued before it read guest memory), while the render thread waited 4.3 ms a frame for commands. The two take
   turns more than they overlap; where the render thread is over budget a frame costs about the game's build plus the
   render thread's tail. To remove it: draws that no longer read guest memory once queued (their data copied when
   queued), so that this wait is skipped like the swap's (PERF-07a), and the game builds the next frame meanwhile.
2. **The record worker is not a bottleneck:** the render thread waited for room in its queue once a frame, 0.19 ms
   (p99 0.32 ms); the worker was idle 89 % of the time (replays 11 %).
3. Acquire and present waits: under 0.05 ms a frame.

## The renderer's own shaders kept on disk (2026-10-31)

The renderer's own SPIR-V compiles (`compile_shader` with no frame: the multiview twins, the surfaces', the presenter's,
the clouds' and the overlay's shaders) were compiled again in every session: 460 to 567 on the audit route, warm cache
or not, each taking the compile lock (`glslangMutex`) the game's new shaders wait for. A twin can only be known at its
pipeline's first two-eye draw (which bindings are per view), so they cannot simply be built before play; their SPIR-V is
now kept instead:
- `shader_compiler.cpp`: before the lock, a compile with no frame is looked up by its source's 128-bit hash and length,
  its stage and the full-precision choice; after a compile it is added. Its own mutex, so the multiview builder thread
  and the render thread can both use it.
- `utility-spirv.bin` next to `spirv.bin` (the same place and the same off switch: `WWHD_VK_SHADER_CACHE`,
  `WWHD_SHADER_CACHE`): one zlib payload, a checksum, and glslang's version (another glslang starts empty). Loaded at
  start with the game's shader cache (`[vulkan shader disk] N of the renderer's own compiled shaders loaded`), saved
  with it: in play on a background thread at most every 10 s, at exit synchronously.
- The game's own shaders keep their cache (`spirv.bin`); a game shader is never taken from this one.

**Measured** (desktop stereo, the audit route, the same user folder twice):

| | Renderer's own compiles | Game SPIR-V compiles | Worst frame | Over 33 ms |
|---|---|---|---|---|
| First session (no file) | 460 | 20 | 64.3 ms | 2 |
| Next session (460 loaded, 5.4 MB) | **0** | 0 | 24.1 ms | 0 |

- **Pictures:** frames 3000 and 4200 with and without the file, the same folder otherwise: the GamePad identical, each
  eye identical but for 1 or 2 pixels of the rupee counter's sparkle, off by 1 (as in every run, "Batched same-state
  runs").
- `shader_compiler_test`: compiled once, saved, taken from the cache without a compile, and a game shader never taken
  from it.

## Log lines off the calling thread (2026-10-28)

**Before:** every `LOG` call did all its work on the thread that logged, under one lock: two formats, the log file's
write (redacted, a system call), and an unbuffered console write (more system calls; a terminal or a pipe whose reader
is slow blocks it). The render thread logs too (`[vulkan ...]`, `[vr] timing`, frame statistics), so a slow disk or
console could hold up a frame, and threads that logged at once waited for each other.

**Now** (`core.cpp` `log_msg`, `log_flush`): the thread that logs formats the line once, keeps it in the crash ring
(as before: crash logs read the last 200 lines from it) and queues it; a writer thread of its own writes the queued
lines, in order, to the log file and the console (Android: logcat). The queue is bounded: past 8 MiB the thread that
logs waits for room (a flood of lines, never in normal play), so nothing is lost or reordered. `log_flush()` writes
what is queued and waits for it (2 s at most): `atexit`, `fatal()`, and every deliberate `_Exit` (the quit, the
renderer's error exits, the game's `exit` / `_Exit`, the story notebook's install, a code mod's restart, macOS's quit).
A crash handler does not wait for the thread (a signal handler must not): the lines still queued when it crashed are in
the crash log's ring, not in `wwhd.log`.

**Checked:** the full test suite (the crash-log and runtime-error tests read the log output); a desktop run that quits
through `_Exit`: the last lines logged before it are in both the log file and the console. **Not measured:** the gain
per frame (a line cost microseconds; the stalls it removes are a slow disk or console's).

## Where a VR frame's time goes (2026-10-29)

Reported: 60 fps in the headset at the 90 fps setting, while desktop stereo reaches 90.

**The render path.** Each eye's scene is drawn into the game's TV target at the internal resolution times the view's
shape. At 3x with PS VR2's aspect of 1.3859 (`aspect.cpp`, "screen targets x1.2828 tall"), that is 3840x2771 per eye,
about 10.6 million pixels. `record_eyes` (`vr_vulkan.cpp`) then blits it once into the OpenXR swapchain image at the
runtime's recommended size times `vr.renderScale`: 3224x3288 at 1.0, which is about the same pixel count. There is no
supersampled stage. The only conversion is the shape, since the target has the game's pixel aspect and the swapchain
the runtime's. Multiview draws both eyes in one pass for most draws (`docs/VR_MULTIVIEW.md`).

**GPU baseline at the headset's size**, unattended desktop stereo: `WWHD_ASPECT=1.3859 WWHD_RES_SCALE=3`, 90 Hz
schedule, mailbox presentation, Great Sea, `WWHD_VK_GPU_PASS_TIMESTAMPS=1`, `WWHD_VK_TARGET_STATS=1`:
- **90 fps,** every frame drawn ("3.00 frames a step").
- **GPU submissions about 3.94 to 4.01 ms a frame** for both eyes, against 11.11 ms.
- **The largest scopes, per frame:**
  - a 2880x2079 full-screen pass: 1.28 ms (5 scopes, 6 draws);
  - the main 3840x2771 scene: 0.34 ms (about 515 to 600 draws);
  - two more 3840x2771 passes: 0.24 and 0.22 ms;
  - everything else under 0.17 ms.
- **The internal resolution makes no difference in the headset:** 2x (2560x1848 per eye) also ran at 60 fps.

So rendering cost is not what holds the headset at 60, and lowering the resolution, ambient occlusion or shadows would
lose detail for nothing. The difference is in the VR submission itself, which desktop stereo does not run:
- the swapchain images acquired and waited for (`xrWaitSwapchainImage`);
- the graphics queue shared with the runtime;
- the images released and `xrEndFrame`.

An unworn PS VR2 takes no frames, so this needs a headset session. The log now has a line every 10 s:
`[vr] submission per frame: eye images acquired p50 / p99; graphics queue taken p50 / p99; released + xrEndFrame
p50 / p99`.

## The desktop window not waited for in VR (2026-10-29)

**Measured in the headset** (PS VR2 at 90 Hz, 2x, `vr.renderScale` 0.9, Great Sea, `WWHD_PROFILE=1 WWHD_VK_STATS=1`), at a
steady 60 fps (2 frames a step):
- Handing the frame to the runtime: eye images acquired p99 under 0.2 ms, graphics queue 0, release + `xrEndFrame`
  p99 under 0.95 ms. Neither the runtime nor the GPU (0.00 ms waited for) is the limit.
- Render thread: 12.0 to 13.6 ms of ops a frame (draws 5.9 to 8.1, registers 1.6 to 2.9, swap 2.3 to 4.3), idle about
  5 ms waiting for the game thread. Of the swap, **1.0 to 2.9 ms was the wait for the desktop TV window's image**
  (`[vulkan waits TV] acquire`, mailbox on a 60 Hz monitor), growing over the session.
- Game thread: waits 2.0 to 3.0 ms a frame for the render thread (DrawDone); in-between pass 13.6 to 14.3 ms, logic
  pass 11.1 to 11.3 ms; VR's own work 0.3 ms.

Both threads are just over the 11.1 ms a frame that 90 fps needs. The eye copies are recorded inside the TV present,
after its image is acquired, so the desktop window's wait held every headset frame.

**Changed** (`backend.cpp`): while a VR frame is drawn, the TV window's image is acquired without waiting. Without one
ready, the present returns, and `swap` records the eye copies and submits them before the frame ends (the path a
standalone headset already took, now on every platform). The headset's pictures are the same; the desktop mirror skips
the frames it had no image for. The same path also keeps the headset fed while the desktop window is minimized.

**Measured in the headset** (same settings, about 9 minutes):
- `[vulkan waits TV] acquire` was 0.00 to 0.01 ms a frame for the whole session.
- **Scenes of about 1,000 to 1,300 draws a frame**:
  - before: a steady 60 fps (render thread 12.0 to 13.2 ms of ops, in-between pass 13.6 to 14.3 ms);
  - now: **90 fps for about 70 s**, with 2.99 frames a step drawn ("on target"):
    - the headset was shown 85.7 frames/s, frame interval p50 11.13 / p99 12.26 ms, 1 missed period in 899 frames;
    - render thread 9.4 to 12.1 ms of ops, in-between pass 11.2 ms.
- **Busier scenes, 1,900 to 2,500 draws a frame:** 60 to 88 fps.
  - The render thread is the limit there, with 12.4 to 15.6 ms of ops and under 1 ms idle a frame.
  - Draws take 7.5 to 9.8 ms of that, register writes (40,000 to 52,000 a frame) 3.0 to 4.3 ms.

The scenes were not the same in the two runs, so the comparison is by draws a frame, not by place.

**What decides 90 now:** the number of draws a frame. Below about 1,300 the render thread fits in 11.1 ms. At
2,300 it needs about 13.5 ms. The per-draw phases that grow with the count are, at 2,300 draws:
- pass 1.4 to 2.8 ms;
- uniforms about 1.2 ms;
- vertex about 1.1 ms;
- shader about 1.0 ms;
- textures about 1.0 ms;
- the register ops beside them.

The drop to "target 60 fps" at the end of a session comes after the VR session stops: the desktop's 60 Hz monitor
paces it then.

## Where a busy frame's render-thread time goes (2026-10-29, clean profile)

Headset run with `WWHD_PROFILE=1` only (no `WWHD_VK_STATS`: it turns off the ALU-constant register fast path), and a new
draw phase, `multiview`: from the last stage binding through `multiview_draw`. The old `pass` phase was mostly that.

**Per frame, by draws a frame** (the render thread's ops; the busy scenes have under 1 ms idle):

| Draws a frame | fps | ops | draws | registers | multiview | uniforms | vertex | shader | textures | pass |
|---|---|---|---|---|---|---|---|---|---|---|
| about 7,000 (the start area) | 54 to 57 | 18.9 to 21.1 | 12.7 to 14.5 | 3.6 to 4.1 | 2.3 to 3.9 | 1.9 to 2.2 | 1.4 to 1.7 | 1.1 to 1.3 | 1.2 to 1.4 | 0.25 |
| 2,000 to 2,450 | 60 to 84 | 12.4 to 15.5 | 8.0 to 10.5 | 2.4 to 3.4 | 1.5 to 2.3 | 1.0 to 1.4 | 1.1 to 1.3 | 1.0 to 1.3 | 0.8 to 1.0 | 0.2 |
| 1,000 to 1,400 | 87 to 90 | 10.3 to 12.4 | 4.7 to 7.2 | 1.6 to 2.1 | 0.7 to 1.6 | 0.6 to 0.9 | 0.8 to 1.1 | 0.6 to 0.9 | 0.5 to 0.8 | 0.15 |

- Starting render passes is cheap: about 54 a frame in every scene, 0.25 ms.
- **The multiview setup is the largest single phase,** about 1 us per two-eye draw. `multiview_draw` repeats, for every
  two-eye draw:
  - eye 1's target lookup (`resolve_replay_targets`);
  - eye 1's texture, sampler and view lookups;
  - eye 1's shifted uniform copies and uploads;
  - fresh descriptor-set writes.

  It does this even for continued draws (only buffer pointers or ALU constants changed), whose eye-1 targets, textures
  and twin pipeline are the previous draw's.
- `WWHD_VK_STATS` costs little: about 10 % of the register op.
- Windows where the swap op takes 6 to 9 ms come only when the headset shows no frames (the runtime not focused, or the
  session ending). The TV window is then waited for at the desktop's 60 Hz, as before, since no VR frame is drawn.

**Next:**
1. Split the multiview phase (lookups, uniforms, descriptor sets).
2. Then keep eye 1's lookups for continued draws, keyed on everything they read: the state version, the surface epoch
   and the texture registers. The output is the same by construction.
3. Busy scenes need 1.3 to 4.4 ms less to reach 90. The multiview setup is about 1.5 to 2.3 ms of that, so this is a
   first step, not the whole way.

### Tried, no gain (reverted): eye 1's texture lookups reused between two-eye draws (2026-10-29)

`multiview_draw` (`draw.cpp`, `Eye1LookupMemo`) keeps the previous two-eye draw's eye 1 texture, sampler and view
lookups. It uses them for the next draw while nothing they read has changed:
- the frame;
- `gx2::full_state_version()`, which moves with every register outside the continued groups (the texture, sampler and
  target registers among them);
- the surface epoch, after the kept textures' guest writes are uploaded, as for a continued draw;
- the eyes' size;
- the shaders;
- eye 0's targets;
- the stages' eye textures.

These are the keys a continued draw reuses eye 0's lookups under, so the images bound are the ones a lookup would find.

It had `WWHD_VK_MV_REUSE_LOOKUPS=0` to turn it off and `WWHD_VK_MV_REUSE_VERIFY=1` to check it. It was reverted after
the measurement below. The multiview phase's split into timing phases stayed: it is now four phases: `mv-setup`, `mv-lookups`, `mv-uniforms` and `mv-sets` (the descriptor sets and
the rest).

**Measured in the headset: no gain.**
- Draws reused their lookups 25 to 75 % of the time (about 250 to 2,200 a frame).
- The multiview phase per two-eye draw was unchanged:
  - before: 550 to 1,200 ns at 1,000 to 2,600 draws a frame (2.3 to 2.6 ms at 7,000);
  - after: 535 to 1,400 ns (2.5 ms at 7,000).
- Its parts at 1,000 to 2,600 draws:

  | Part | ms a frame |
  |---|---|
  | `mv-setup` | 0.13 to 1.4 |
  | `mv-lookups` | 0.3 to 0.7 |
  | `mv-uniforms` | 0.05 to 0.14 |
  | `mv-sets` | 0.2 to 0.44 |

- `mv-setup` is the largest and swings tenfold at the same draw count. It holds the emission of eye 1's held commands
  (`stereo_flush`, about 4 a frame), which is eye 1's own drawing done there, not setup overhead.

### Auto in the headset (2026-10-29)

At an even 45, frames are uneven 0 to 4 in 450 per 10 s (from up to 24 % at the full rate). The tries at 90 in the busy
scenes drew 44 to 92 % of their frames and went back to 45.

### The judder at the full rate (2026-10-29)

In the busy scenes the frames a step change between 3 and 2, so frames of a third of a step (11.1 ms) mix with frames
of half a step that miss a refresh (22 ms). The clean run had:
- 95 to 158 missed display periods every 10 s;
- up to 24 % of frames uneven (192 of 801).

That is the judder seen at the higher rates. `vr.frameRate=auto` is made for this: it stays at 90 while 97 % of the
frames fit, else draws an even 45 (each frame two refreshes) and tries 90 again after 10 s.

## Function-level profile in the headset (2026-10-29)

`WWHD_SAMPLE_THREADS=captures/vrprof WWHD_SAMPLE_AFTER=45 WWHD_SAMPLE_SECONDS=180`, built with `-g` (C++ only; the
game's C code keeps its flags), Capped 90, 3x, about 37 s of play, read with `tools/bench/thread_profile.py`. The sampler
stops the threads 1,000 times a second, so frame rates in this run are lower than in play.

**Render thread, inclusive** (about 55 % of its time busy):

| Part | Share |
|---|---|
| waiting for the game thread's commands (`wait_for`) | 21 % |
| `xrWaitFrame` (`begin_frame`) / `xrEndFrame` | 8.5 % / 3.5 % |
| draws (`draw_stereo`) | 43.5 % |
| of them, `bind_stage` | 10.8 % |
| of them, `multiview_draw` (with `stereo_flush`, 3.1 %) | 8.6 % |
| of them, `resolve_texture` | 3.2 % |
| of them, `snapshot` (upload copies) | 2.4 % |
| of them, `stage_descriptor_set` (the driver's descriptor updates) | 2.4 % |
| of them, `resolve_targets` | 2.3 % |
| register writes (`apply_regs` and the op dispatch) | about 5 % |
| waits for the record worker's queue (`record_worker.cpp` 37 and 56; `maxQueued` 4) | 4.4 % |
| hash-map lookups (the buffer cache's `bufcache::Key` among them) | about 3 % |
| heap allocations and frees | about 2 % |

**Game thread:**
- 44 % asleep in pacing (`GX2WaitForVsync`, `wait_for_late_flip`, `paced_pass_start`);
- 27 % in `GX2DrawDone` at the frame's end, waiting for the render thread;
- about 20 % the game's own code.

No single part is the 2 to 4 ms the busy scenes are over by. The cost is spread over the per-draw path.

## Batched same-state runs (issue #1, 2026-10-30)

On by default with the Vulkan renderer; `WWHD_VK_BATCH_RUNS=0` turns it off. Measured in the desktop stereo mode only, not
yet in the headset.

### What the runs are

Measured with `WWHD_PROFILE=1` in the desktop stereo mode, from the busy grass view of 2026-10-29 (a copy of
`last_session.wwstate.regs-bak`, 1,850 draws a frame):

| Shader pair | Draws a frame | What changes between them | What it is |
|---|---|---|---|
| VS `3B7D4A00` / PS `3B7D5900` | 252 in 3 runs of 84 | vertex-shader uniform block addresses | grass, the sun atlas's three cascades (the atlas's thread) |
| VS `3B7D9800`, VS `3B7D6D00` | 84 + 84 | vertex-shader uniform block addresses | the same grass in two scene passes |
| VS `3B7CB100`, VS `3B7C8700` | 64 + 64 | no registers; only the draws' index ranges | (not identified) |
| VS `17F0AA00`, `17F27900`, ... | 4 to 9 a run | ALU constants | short runs (particles and the like) |

The grass comes from `dGrass_packet_c::draw` (`decomp/wwhd_src/d/d_grass.cpp`): per pass one material, texture and render
state, then one `GX2DrawIndexedEx` per tuft (same indices, one instance), each with its own model packet's uniform block.
Its vertex shader is in the translator's remapped uniform mode: the tuft's entries are packed into the support block
(`ufBlock { ivec4 uf_remappedVS[10]; }`) with the camera's projection.

No torches or fire were in view in the saved states at hand; the particle-like runs seen were all short (ALU constants).

**Split skip test** (`WWHD_TEST_SKIP_BATCHABLE=2`: only the draws continuing a run the render thread draws; `=3`: only
the others, the sun atlas's), 90 Hz schedule (`WWHD_INTERP_FPS=120 WWHD_INTERP_VR_HZ=90`), render thread idle per frame:

| Run | Draws left out | Render thread idle |
|---|---|---|
| none | 0 | 2.36 ms |
| all (`=1`) | 632 | 3.56 ms |
| render thread's (`=2`) | 371 | 3.43 ms |
| sun atlas's (`=3`) | 249 | 2.37 ms |

The sun atlas's run draws cost the render thread nothing measurable (they are recorded on the atlas's thread, which
keeps up), so batching them would not help the render thread.

### A separate thread for the effects' animation: no

- The skip test left all of the game thread's work in place (grass sway and particle calculation, the draws' command
  writing) and only left draws out: the frame rate went from about 62 to 80 in the headset.
- The game thread waits most of its frame (function-level profile above: 44 % in pacing, 27 % in `GX2DrawDone`
  waiting for the render thread, about 20 % its own code).
- Sway and particle calculation run once per logic step (30 Hz); the in-between passes only blend (`interp_fx.cpp`).
- Another thread would remove no draw, and the draws are the cost.

### How it works

- **gx2 holds the run** (`gx2_core.cpp`, `held_*`): a draw that repeats the last draw's arguments with only
  vertex-shader uniform block addresses changed in between (the state groups' dirty bits, then the block words) starts
  a held run; the draws after it join it while every register write changes nothing but those addresses (checked at
  each write). Anything else first draws the held run. A run is held only from its second draw on, so draws that begin
  no run cost a compare of their arguments.
- **The renderer draws it as one instanced draw** (`draw.cpp`, `batch_*`; `gfx/draw_batch.h`): instance k is piece k.
  The vertex shader's batched variant (`batch_glsl.cpp`, made and compiled on a thread of its own the first time) turns
  each batched uniform block, and the support block when its remapped entries come from one, into an array indexed by
  `gl_InstanceIndex`, and reads the guest's instance ID as 0 (as each draw did). Each piece's support block is piece 0's
  bytes with its own remapped entries, copied as `pack_uniforms_into` copies them, eye-shifted for the eye drawn.
- **Multiview stays:** the camera projection's eye rows are the same in every piece, so eye 1's copy is the same rows
  applied to each piece (`stereo::EyeRows` `repeat`/`stride`); the multiview variant composes with the batched one.
- **Exact fallback:** the renderer refuses a run (and gx2 draws it draw by draw, each piece with its own block
  addresses, as before) when it is not drawn on the render thread (the sun atlas, the scene's or eye's thread), the
  variant is still being made or failed, a bound block holds a camera projection, an attribute is read per instance, or
  the stage changes uniforms in place (AO's vertex shader, the sun shadow mask).
- Diagnostics: `[vulkan batch]` every 600 frames (runs drawn, why others were not); `WWHD_VK_BATCH_STATS=1` (what ended
  the runs, by vertex program, their drawing time); `WWHD_VK_BATCH_REFUSE=1` (every run drawn draw by draw, for A/Bs);
  `WWHD_VK_BATCH_DUMP=dir` (the GLSL of vertex shaders refused for their shader).

### Measured (desktop stereo, 90 Hz schedule, the grass view)

- **Pictures:** fixed seeds, interpolation off, frames 2400, 3000, 3600 and 4200 of both eyes and the GamePad, against
  b5bf0b44 (run twice, identical both times):
  - the grass view (168 draws a frame batched): every picture byte-identical;
  - two other saved states (32 and 58 draws a frame batched, the same grass passes): identical but for one or two
    pixels of the rupee counter's sparkle, off by 1, at a different frame in each run. The same pixels vary with every
    run drawn draw by draw (`WWHD_VK_BATCH_REFUSE=1`: no instanced draw at all), so it is the sparkle's timing (holding a
    run moves when the draws reach the GPU), not the batched draws.
- **The runs' own time** (`WWHD_VK_BATCH_STATS=1`): 168 draws in 4 runs a frame took 0.375 ms drawn draw by draw and
  0.05 ms batched.
- **The render thread:** idle 2.37 ms (b5bf0b44, median of three runs) to 2.46 ms; CPU 8.00 to 7.90 ms. A small gain
  here: these draws were already cheap on the continued-draw path, and the view's largest runs are the sun atlas's.
- **Not batched yet:** the short ALU-constant runs. (The runs that change only their index ranges: below.)

## Index-range runs and unseen GamePad draws (2026-10-30)

In a busy VR area the main scene drew about 3,500 draws a frame; 2,688 were two shader pairs (VS `3B8ABA00` / PS
`3B8AD600`, VS `3B8A9100` / PS `3B8AA000`) in 224 runs of 6 indexed triangle strips each, with no register changed
between them and each piece taking the indices right after the last one's.

- **Index-range runs** (gx2_core.cpp): consecutive indexed draws of strips or lists with the same primitive, index
  type and base vertex, one instance, contiguous indices and no register change between them are drawn as one draw.
  Lists are one range already; strips are stitched into a scratch index ring (8 MiB of host guest memory) with
  zero-area joining triangles that keep each piece's winding. A run with a restart index, or too large for the ring,
  is drawn piece by piece. `WWHD_VK_INDEX_RUNS=0` turns it off; the log gives the runs and the draws in them every
  600 frames.
- **Unseen GamePad draws:** in VR, with the GamePad screen's panel hidden and no GamePad window on the desktop, the
  draws into the colour buffer the game copies to the GamePad (scan target 4) are left out (about 150 a frame); its
  clears and copies still run, and the panel shows the next picture when it opens. Not when the TV's picture comes
  from the same buffer. `WWHD_VK_SKIP_UNSEEN_GAMEPAD=0` draws them.
- **First play test** (Linux, SteamVR, Auto at 45 fps, the same area): 448 runs a frame drawn as one draw each in
  place of 1,792 draws (4 of each run's 6 pieces: the other 2 do not join yet); the main scene's draws overlapping the
  sun atlas from about 1,500 to about 830; the render thread's time in ops from 15 to 17.4 ms to 11.2 to 12.6 ms a
  frame. Its CPU time did not fall (16 to 18.5 ms) and the frame rate stayed at 45: not yet explained. The GamePad
  window was open on the desktop, so no GamePad draws were left out.
- **Draw runs generalised** (2026-10-31): the pieces' indices may be anywhere and their base vertices differ (copied
  into the ring, rebased as 32-bit indices from the smallest base), and non-indexed lists whose vertices follow on are
  runs too. The grass's runs now join all 6 pieces: 2,688 draws a frame drawn as 448.
- **`WWHD_VK_INDEX_RUN_STATS=1`**: every 600 frames, the shader pairs with the most draws after one of their own not
  drawn as one (neither a merged run nor an instanced same-state run), with a signature (a hash of each program's first
  256 bytes, the same wherever it is loaded; the primitive, index size and elements a draw; how often the draw repeats;
  which uniform blocks changed) and the registers changed between two of them. In play: the grass merged; what remains
  in the busy and medium areas is single-quad sprites (4-index strips) and small strips whose draws change only the
  vertex buffer, fetch shader and uniform block addresses (`E8C0`, `A225`, `E7FC`, `E25E`): a candidate for a rule that
  batches address-only changes (not built). The "in ops" estimate reads low with runs on (a run's pieces are timed
  as cheap collections, its draw inside another op): compare the render thread's CPU time.
- **Headset, 2026-10-31** (Auto drop share 90 %): 65 % of the readings at 90 fps, 17 % at 45, 78.8 fps on average; the
  grass area still 14 to 17 ms of render-thread CPU a frame, at 45.

### Copied run indices kept (2026-10-31)

**Found** in a 19-minute headset play run (Unlocked FPS at 90 Hz, `WWHD_PROFILE=1`): views of 4,000 to 6,100 draws a
frame (on foot, about a quarter of the play) sat at 60 fps (the pacing gives a logic step 3 frames or 2, so about
13 ms frames show as 60), while everything up to 2,800 draws held about 89. 3,000 of those draws were two shader
pairs in runs of 6 with nothing changed between them, merged into 500 draws a frame, every one with its indices
copied into the scratch ring. Each copy went to a new ring address, so the buffer cache missed every one of them every
frame (0.08 MiB of index uploads a frame there, none in the light views): the range armed (write-protected), converted
and uploaded under the cache's shared lock, and the ring's next lap faulting on the armed pages. Buffer cache lock
waits rose from about 0.25 to 1 ms a frame, the drawing threads' draw from about 4.6 to 6.8 us, and the render thread
waited 3.9 ms a frame at the flush join for scene thread A.

**The change** (`gather_indices`): a run's indices are gathered into a scratch buffer first; if the same run (its
pieces' counts, index addresses and base vertices) was copied before, with the same bytes, and the ring has moved on
less than half its size since, the earlier copy's address is given again and nothing is written. The cache then finds
its converted indices there. Exact: the bytes are compared, and the cache still checks the copy's pages for writes; a
reused copy is as far from being overwritten as a new one would be (a new copy lasts a whole ring, a reused one at
least half). `[gx2] draw runs per frame` counts the reused copies; `WWHD_VK_INDEX_RUN_REUSE=0` turns it off.

**Checked:** frame 4200 identical to the references in both eyes in `return`, `busy` and `start` (their 2 copied runs
a frame all reused). Not measured where it matters: no saved state reaches those views yet, and the desktop timing
rig's `return` run currently stops at frame ~1,350 with a game assert (`d_com_inf_game.cpp:1301`,
`i_phase->id != 1`), with this change on or off and with either settings file (not seen with this morning's build).

### Weather quads (2026-11-07)

**Found** in a rainy area that held 60 to 80 fps against 88 to 89 elsewhere (PS VR2, 90 Hz, `WWHD_PROFILE=1`):
- about 2.8 times the draws of the light areas (1,850 to 1,970 multiview draws a frame against 685), the GPU busy 3.5 to
  4.4 ms a frame and idle 9 to 13 ms;
- the scene's thread taking 950 to 990 draws a frame at 6.8 us each, the render thread waiting 5.1 to 5.9 ms a frame
  for it;
- `WWHD_VK_INDEX_RUN_STATS=1`: the biggest group not drawn as one was one shader pair, `VS 003B8614`, 680 to 890
  draws a frame, each a 4-index strip (one quad), with attribute buffer 0's address (`E8C0`) and the fetch shader's
  address (`A225`) changed before every draw and vertex block 4 / pixel block 2 (`E7FC` / `E25E`) before one in four
  or five. The next groups were the same shape with the blocks changed every draw (110 to 137, 107 to 131 a frame).
- Their signatures (a hash of the programs' first 256 bytes) match variants of `Common/Shaders/Prim/wii_pipeline.sharcfb`
  (the HD engine's GX pipeline emulation; the archive does not name its variants).
- The decomp (`decomp/wwhd_src/d/d_kankyo_wether.cpp`) shows the weather packets drawing one quad a particle, each
  with its own vertex buffer and its own fetch shader copy: rain streaks (`renderStreaks`, `mRainCount / 2` quads),
  rain splashes (`renderSplashes`, 4 quads a splash after one texture-uniform update: the one in four), snow, the
  floating sparkles (`housi`), poison mist, cloud shadows, waves, stars, the sun; the sky clouds (`drawVrkumo`, 3
  layers of up to 100) the same with their own blocks. The rain is the 003B8614 group.

**The change** (`weather_quads.cpp`, `gx2_core.cpp` weather runs; on with Vulkan, `WWHD_VK_WEATHER_QUADS=0` turns it off):
- hooks bracket the packets' quad-drawing functions (`0257E9DC`, `0257EF00`, `0257F804`, `0257FB4C`, `02580870`,
  `02580B98`, `02581060`, `025817B0`, `02581C34`, `025820F4`, `02582650`, and `drawVrkumo` `02575B6C`) with
  `OP_WEATHER_QUADS` 1 / 0; nothing outside them changes;
- between them, consecutive draws with the same arguments (a strip or list of up to 64 indices, one instance, base
  vertex 0) join a run while only attribute buffer 0's address and the fetch shader's address change between them,
  and the fetch shader's bytes are the first's;
- the run's vertex buffers are copied one after another into a 4 MiB scratch ring (each padded to the stride),
  attribute buffer 0 and the fetch shader point at the copy and the first piece's, and the indices are stitched with
  each piece's base vertex (`gather_indices`, as the draw runs'); the run is drawn at once as one draw and the registers
  are put back;
- anything else between them (another register, a block's address, another op, another shape) draws the run first;
  a run of one is drawn as it was. So the rain streaks become one draw and each splash's 4 quads one; the quads whose
  blocks change every draw (sparkles, mist, sky clouds) are drawn as before (per-piece blocks would need the batched
  shader variants for both stages: not built).
- `[gx2] weather quads per frame` every 600 frames: runs drawn as one, the quads in them, quads drawn alone, the brackets and draws in them; `[gx2]   not joined` why the others were not (shape, base vertex, vertex buffer, arguments, size or stride, fetch shader bytes) and what ended the runs.
- The markers are recorded into display lists with the draws they bracket (the game records its weather draws and
  replays them; a first build left them out while a list recorded, and nothing joined).

**Measured in the headset** (PS VR2, 90 Hz, the same rainy area, `WWHD_PROFILE=1 WWHD_VK_INDEX_RUN_STATS=1`):

| | Before | After |
|---|---|---|
| weather draws a frame (the brackets) | 1,325 to 1,366, one by one | 425 to 509 (1,074 to 1,125 quads in 238 to 251 runs) |
| `VS 003B8614` not drawn as one | about 900 a frame | gone from the list |
| frame interval p50 / p95 | 13.4 / about 20 ms | 11.15 to 11.36 / 12.6 to 19.0 ms |
| frames over 1.5 times the budget | 199 of 685 | 12 to 66 of 815 to 878 |
| render thread after `xrWaitFrame` p50 | 1.5 to 1.6 ms | 0.47 to 0.50 ms (the scene's thread no longer needed) |

- No quad was refused by the shape or fetch shader checks; runs end at the render state written between quad groups
  (`A200`, the depth control), most at each splash's four quads. The rain looked the same (checked in the headset).
- Left as before: the quads whose blocks change every draw (sparkles, mist, sky clouds: about 240 to 300 a frame
  drawn alone).

## Continued draws: recent states and moved fetch shaders (issue #1, 2026-10-30)

The continued-draw fast path (draw.cpp, `fastpath_config`) reused only the previous draw's resolved state, and any
change outside the continued register groups ended it. In the heavy views most draws resolved in full.

- **Recent states.** A measurement in the headset hashed the registers that end continuations: up to 71% of the full
  draws had the state of the draw two back (A, B, A, B), and every one resolved the same. Each eye now keeps its last
  three resolved states. The register machines (the render thread's and the atlas thread's replay machine) log each
  change that ends a continuation, with the value it replaced (`gx2::ChangeLog`, replay_machine.h). A state is continued
  again when every register changed since it holds the value it had then: one pass over the log, no hashing. The
  hashing measurement itself cost about 1.3 ms a frame in the headset and is removed.
- **Moved fetch shaders.** On the render thread the remaining misses were all `SQ_PGM_START_FS`: the game writes its
  fetch shaders anew for each draw, so the address changes while the shader does not (the same key, the same
  `LatteFetchShader`). The fetch shader's two registers are left out of the comparison and the fetch shader is looked
  up instead (for the game's own `WWFA0001` fetch shaders a 16-byte header read). The shaders and the pipeline follow
  from the fetch shader and its key, not its address. The previous draw's state is continued the same way.
- `WWHD_VK_FASTPATH_RECENT=0` turns both off; the `[vulkan fastpath]` line (`WWHD_VK_STATS=1`) shows the share continued
  through the change log.

Measured on the desktop (stereo, 90 Hz schedule):

| View | Continued before | After | Render thread |
|---|---|---|---|
| Title scene, render thread (640 draws a frame) | 26% | 79% | CPU 6.0 → 5.3 ms |
| Title scene, atlas thread (306) | 0% | 72% | |
| The headset's last heavy spot (2,000–2,250 draws) | 17% | 67% | draws 6.65 → 6.18 ms |

- **Exact:** verify mode (`WWHD_VK_FASTPATH_VERIFY=1`, every continued draw also resolved in full and compared): 0
  mismatches in about 2.5 million continued draws (title scene and the gameplay route). Pictures (title scene 15 frames,
  gameplay route 12) byte-identical with it on and off, and with the earlier reference runs.
- **What is left in the heavy spot** (ms a frame, of 6.2 in draws): uniforms 0.94 (packing 0.42), vertex 0.81, the
  draw's set-up up to the shader lookups 0.79, textures 0.57, submit 0.53, multiview set-up and lookups 0.84; register
  writes 1.07 besides. No single part dominates.

## xrEndFrame on its own thread (2026-11-02)

**Reported:** a Quest 3 (standalone) player at about 30 fps at every refresh rate (72, 90, 108, 144 Hz). Their log:
`released + xrEndFrame p50 13.4 to 16.9 ms` in every scene, 0.2 ms in menus and loading screens. The render thread's own
work is about 1 ms. So the runtime waited for the GPU inside the frame's end, and the render thread waited with it:
the next frame's draws started only after the GPU had finished this one, so CPU and GPU time added up (about 33 ms).

**Changed** (`vr_vulkan.cpp`, `end_thread`): `frame_end` copies what the frame's end
needs (views, quads, which swapchains were written) into a job and a thread of its own releases the images and calls
`xrEndFrame`, under the graphics queue's lock as before. The render thread goes on to the next frame's draws.
- **OpenXR's order is kept:** `frame_begin` waits for the thread before `xrWaitFrame` (and before polling events), so
  every `xrEndFrame` comes before the next `xrWaitFrame` / `xrBeginFrame`, and every image is released before it is
  acquired again. `stop` and `headset_frame` (the frame shown again) wait for it too.
- **The pictures are the same:** nothing drawn changes, only when the frame's end is called and from which thread.
- **Limit:** the thread holds the graphics queue's lock while the runtime waits, so the record worker's submissions of
  the next frame wait for it (the render thread records meanwhile). How much overlaps depends on how soon the next
  frame needs a submission to finish.
- **On by default** since 2026-11-03 (`WWHD_VR_END_FRAME_THREAD=0` keeps the frame's end on the render thread).
  PS VR2: a first run with the thread at normal priority jittered (it was put off behind the game's boosted threads);
  with it boosted like the render thread, 87 to 89 fps once warmed up, and the render thread never waited for it.
  Running the frame's end on the render thread instead added about 0.8 ms to its swap step (2.11 to 2.88 ms a frame).
  Not yet run on a Quest, where it is meant to overlap the runtime's 13 to 17 ms wait.

**The log** (every 10 s): the submission line now also gives the images released alone (`released alone p50 / p99`),
which tells the release apart from `xrEndFrame`; with the thread on, `[vr] end-frame thread: the render thread waited
for it in N of M frames, p50 / p99 / max`. The `render thread after xrWaitFrame` time no longer includes the frame's
end when the thread is on.

## PC VR at 3x by default (2026-11-04)

A Quest 3 player on Virtual Desktop drew at the desktop's default 1x internal resolution: 1230x1335 eye pictures for
the headset's 2752x2940, the RTX 5080 busy 5 % of the time. PC VR (Windows and Linux on x64) now draws at 3x unless the
player chose a resolution: when a session starts without `resScale` in settings.ini (or `WWHD_RES_SCALE`), it is set to
3x (`vr_vulkan.cpp` `apply_vr_res_default`, logged `[vr] internal resolution 3x for PC VR`); when the session ends the
desktop's comes back, unless one was saved meanwhile (F1 > Graphics). Standalone headsets (Android, arm64) keep their
own. The VR launcher shows 3x as the default on PC.

## The desktop mirror's present off the render thread (2026-11-07)

**Found** (render-thread audit, PS VR2 at 90 Hz): of the render thread's frame swap, the desktop window's
`vkQueuePresentKHR` took 0.21 to 0.24 ms a frame (`[vulkan waits TV] present`), in and out of the fights, under the
graphics queue's lock that the frame-end thread and the record worker also take. Acquiring the window's image (0.00
ms, never waited for in VR) and composing into it (0.03 ms) are cheap.

**Changed** (`backend.cpp`, `MirrorThread`; PC only): while a VR frame is drawn, that present is made by a thread of its
own on a second queue of the graphics family (priority 0.5 against the game's 1.0), which only it uses: no lock is
shared. The render thread acquires, composes, records the eyes and submits as before, then hands the image over (a
sequence number with C++20 atomic wait and notify) and goes on.
- `busy`, one atomic, is all the two share. While it is set the thread owns the swapchain's present: a VR frame then
  leaves the mirror out (no wait; the eyes go as when the window has no image, `vrvk::eyes_pending` in `swap`), and
  anything else that touches the swapchain first waits for the present to return (`mirror_quiesce`: swapchain
  changes, surface retries, device idle, a TV present outside VR; rare).
- The present's result is applied by the render thread at the next TV present (`mirror_collect`: resize, first
  picture), as `queue_present` applied it.
- Not for the paced or special paths: held presents (even pacing), generated frames, present timing
  (`VK_GOOGLE_display_timing` observation), captures, and the record worker's native presentation
  (`WWHD_R5_NATIVE_PRESENT=1`, whose submit is not on the render thread). Those keep the render thread's present.
- The HDR metadata call moves out of the queue lock (it names the swapchain, not the queue), before the hand-over.
- `WWHD_VR_MIRROR_THREAD=0`: no second queue, the render thread presents as before. A family with one queue keeps
  the old path (logged).
- With `WWHD_VK_STATS=1`: `[vulkan mirror thread] N presents/frame, M ms/frame there (off the render thread); K
  frames/frame left out while one ran`.

The headset's pictures are unchanged; the desktop mirror may skip a frame when its previous present is still running.

**In the headset** (no stats): ran the whole session without a present error or a swapchain change; the frame pacing is in "The helper threads' eye 1 target lookup kept by its registers" below.

## Tried, dropped: keyed states for repeated enemies (2026-11-07)

**The idea:** a save with several enemies of a type ran its fights at 72.1 fps on average (2,100 to 2,600 draws a
frame; 88.1 outside them; PS VR2 at 90 Hz, `WWHD_PROFILE=1 WWHD_VK_STATS=1`). The scene's drawing threads drew at 8.1
to 8.5 us a draw, A resolving 120 to 140 draws a frame in full. The guess: the second and later enemies of a type
repeat the first one's states 10 to 40 states back, past the recent states' ring.

**Built:** a key of the logged registers in `gx2::ChangeLog` (an XOR of a 64-bit mix of each changed register's value
now and at the last reset; leaving out the fetch shader's registers, the stencil reference byte, and the vertex and
pixel shaders' texture and sampler words, whose unread units keep what earlier materials left; the units the shaders
sample were compared instead), and 128 kept resolved states an eye a thread by that key, after the recent states.

**Measured in the headset:** exact (verify mode: 0 mismatches in about 8 million checked draws), but **the repeats are
not there**. Of A's 100 or so full draws a frame, 2 to 5 had the result (shaders, pipeline, targets, textures,
samplers) of any of the 128 kept states; keyed hits stayed at 0 to 0.6 % of the draws. The fights add draws (about
900 a frame), not misses: the share continued is the same in and out of them. Its lookup and keeping cost about 0.15
us a draw, on the render thread too in its first build; the player found that run the worst yet (render thread CPU
about 1.2 ms a frame more outside the fights in the one comparable window). **Dropped.** Not to try again: a larger
or keyed cache of resolved states (nothing for it to find).

## The helper threads' eye 1 target lookup kept by its registers (2026-11-07)

A continued draw still looks up eye 1's targets in `multiview_draw` (`mv-lookups`, 1.34 us a draw on the drawing
threads in the fights above). The lookup's one-entry memo was keyed on the full-state version, which every draw
continued from a recent state moves. On the scene's drawing threads it now keeps 8 entries by the target, output and
other groups' keys (`ChangeLog::groupHash`, kept per register group in the drawing threads' replay machines only), with
the pixel shader, scissor, surface epoch, eye size and AO and UI modes as before (`resolve_replay_targets`). Those
groups hold every register the lookup reads (listed there). Measured with the keyed states above:
`mv-lookups` 1.34 to 0.80 us a draw. **The render thread is unchanged:** its log keeps no key and it keeps the
one-entry memo by the version (no work added there). `WWHD_VK_REPLAY_TARGETS_KEYED=0` uses the version on every thread.

**In the headset with the mirror thread below** (no stats, the same route): 1.5 % of the frames missed their display
period (9.9 % in the first run above), 2.6 % uneven, 11 of 14 ten-second windows at 89 to 90 fps, two at about 80
(the fights); the player found it better. Stats and the profiler were off in that run and on in the others, so the
share of each change is not separated.

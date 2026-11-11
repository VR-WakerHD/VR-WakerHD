# The right eye on a CPU thread of its own (roadmap 2a)

In stereo every scene draw is drawn twice: eye 0 resolves everything (shaders, pipeline, targets, textures, uniforms)
and hands its result to eye 1 (`StereoShaders`), whose draw is a replay with its own targets and eye-shifted uniforms.
Eye 1's commands are already held in a stream of their own (`stereo.cpp` `eyeCommands`) and emitted at
`stereo_flush`. This plan records eye 1's draws on a second thread while the render thread goes on with eye 0's.

It builds on the sun atlas thread (`docs/VR_ATLAS_THREAD.md`). These pieces already exist per thread:
- the replay register machine and its forwarded writes;
- the pass state, upload arena, descriptor pool and draw counters;
- `stereoEye` and the eye maps;
- the cache locks;
- the held streams.

## What it could save (stage E1, measured)

Desktop stereo, Outset, the 120 Hz proxy, with the atlas thread on (`WWHD_VK_TARGET_STATS=1`, ms per frame):

| eye 0's draws (with what they prepare for eye 1) | the shared sun atlas (queued) | eye 1's draws | eye 1's flushes |
|---|---|---|---|
| 6.91 | 0.42 | 2.58 | 0.01 |

The most a thread of eye 1's own could give the render thread is about 2.6 ms a frame. The hand-over costs part of
that: about 2,640 draws a frame, against 3,030 for the atlas, whose hand-over cost about 0.3 us a draw.

## How it differs from the atlas

- **Interleaved, not a block.** Eye 1's draw N follows eye 0's draw N. The queue is per draw for the whole frame, and
  each job carries eye 0's hand-over (`StereoShaders`, a few hundred bytes).
- **Flushes inside draws.** `stereo_release_texture` flushes eye 1's held commands when eye 0 is about to change a
  texture eye 1 sampled. It runs inside cache locks, so the render thread cannot wait there. Instead such a flush
  becomes a cut: eye 1's commands so far and eye 0's commands so far each end a segment, and the segments are
  emitted in order (eye 0's, eye 1's, eye 0's next, ...) at the next real join.
- **Eye 0's textures.** Eye 1 samples eye 0's textures (`stereo_eye0_texture`) as eye 0 left them at draw N, but eye
  0 runs ahead. The same marks as the atlas's overlap catch eye 0 changing a texture eye 1 has not drawn with yet. The
  same remedy applies: changes eye 1 must not see go into the segment after the cut.

## Stages (each about 2 hours, each leaving the game runnable and byte-identical)

1. **E1. Measure and list** what eye 1's draw touches: done above, together with this plan.
2. **E2. Segmented held streams on one thread.** `stereo_release_flush` cuts instead of flushing when asked (a
   switch), and the segments are emitted at the next `stereo_flush`. Validate byte-identical pictures and timing
   before any threading.
3. **E3. A generic draw thread.** Factor the atlas thread's worker (batches, register forwarding, per-thread state,
   errors) into one class both threads use. The atlas thread must stay byte-identical.
4. **E4. Eye 1 on the thread, one draw at a time** (as atlas stage 3.1). Each job carries a copy of eye 0's
   hand-over, and eye 1's pass becomes the thread's. Validate byte-identical, with the thread check.
5. **E5. Eye 1 overlapping eye 0:** no wait per draw; joins at `stereo_flush`, submissions, in-order operations, the
   swap and idle; cuts for in-draw flushes; the marks for eye 0's textures. Measure.
6. **E6. Three threads together** (render, atlas, eye 1): the order of their streams at the joins, lock contention,
   timing.
7. **E7. Hardening** (long runs, save states, scene changes; a headset session where possible) and the default.

Expected outcome: 1.5 to 2 ms of the render thread's frame, on top of the atlas thread's 1.2 ms. The risks are the
in-draw flushes and eye 0's textures, which is why the cut (E2) is validated alone first.

## Stage E2 done: segmented held streams

`WWHD_VK_EYE_SEGMENTS=1` (off by default; the eye thread will turn it on) holds both eyes' commands, in pairs of
segments:
- **Eye 0's go to its segment** instead of straight into the renderer's stream. Eye 1's go to its segment as before.
- **A flush from inside a draw** (`stereo_release_flush`: eye 0 is about to change a texture eye 1's held commands
  sample) cuts: each eye's pass ends at the end of its segment, and the next pair begins. Nothing is emitted, so the
  cut needs no wait for another thread.
- **`stereo_flush` emits every pair in order,** eye 0's segment then eye 1's: the same GPU order as flushing at every
  cut.
- **With the atlas thread** eye 0's scene commands are in its segments instead of the scene stream. The atlas's join
  emits the before-atlas and atlas streams, and the segments, all after the atlas in the guest's order, wait for the
  next `stereo_flush`.

**Checked (desktop stereo):**
- frame 4200 byte-identical in both eyes: with segments alone, with `WWHD_VK_THREAD_CHECK=1` (its timing), and with the
  atlas thread on;
- render thread CPU 13.71 against 13.72 ms (the atlas thread on in both): holding eye 0 costs nothing measurable;
- this scene makes no in-draw flushes (0 cuts a frame), so the cut itself is exercised from stage E5 on, where every
  change eye 0 makes to a shared texture cuts.

## Stage E3 done: one draw thread for both

`gfx/vulkan/draw_thread.h`/`.cpp`: the atlas thread's worker, made general. It owns:
- the batched job queue (no allocation per job once warm; woken only when idle) and errors rethrown at the next wait;
- the replay register machine, the draw counters and the pass state;
- `begin(stream)` (load the machine, start forwarding, record into a held stream) and `finish()` (pass ended, every job
  done, counters added).

A derived class sets the rest of its per-thread state (upload arena, descriptor pool, markers) in `setup`, and does
its own jobs in `execute`. Register writes reach every running draw thread through one fan-out, since the GX2 machine
has a single forward. The cache locks are on while any draw thread runs. The atlas thread now derives from it.

`draw_threads::on_draw_thread()` marks any draw thread. The guards that kept the atlas's thread from presenting,
opening GPU timestamp scopes or submitting a draw batch now use it, so they cover eye 1's thread too.

**Checked (desktop stereo):** the atlas thread's frame 4200 byte-identical in both eyes (its thread-check run hit the
known alternate moment, `7B5C…`/`8305…`, as the check's slower timing has before); render thread CPU 13.86 against
13.72 ms before (noise); the smoke test passes.

**A correction to the earlier stages' notes:** `WWHD_VK_THREAD_CHECK=1` logs the first use of each area from a second
thread. It did so for every area the atlas's thread uses, as expected, since stages 2.2 to 2.9 made them safe. It is
an inventory, not a race detector, so "no violations" in the earlier notes said more than it shows. Its message now
says the areas were made safe, not "not yet safe".

## Stage E4 done: eye 1 on its thread, one draw at a time

`WWHD_VK_EYE_THREAD=1` (off by default; it turns the eyes' held segments on) draws eye 1 on a draw thread of its own
(`eye_thread.h`/`.cpp`, derived from `DrawThread`):
- **Eye 0's hand-over** (`StereoShaders`) goes into a slot of a ring of 512 instead of the thread-local one. The job
  carries the slot, so nothing is copied; the slots' vectors keep their capacity.
- **The thread's own state:** `stereoEye` 1, its own upload arena and descriptor pool (`R.eyeUploadBlocks`,
  `R.eyeDescriptorPool`, with the atlas's lifecycle), its own pass, and eye 1's current held segment.
- **Cuts** are queued to it: its pass ends at the end of its segment, and it goes on in the next. `stereo_flush`
  finishes it before emitting the segments.
- **Eye 1's part of an invalidate** (its own surfaces) is done there, in order with its draws.
- **The guards** that keep draw threads from presenting, opening GPU timestamp scopes and submitting draw batches
  cover it (`draw_threads::on_draw_thread()`), and an in-draw flush on a draw thread does nothing: its own commands
  and changes are in one stream, in order.

**Checked (desktop stereo):** frame 4200 byte-identical in both eyes with the eye thread alone and with the atlas thread
too. The render thread waits for each of about 2,500 draws a frame, so the frame takes about 62 ms in the 120 Hz proxy.
That is the hand-over's cost, which stage E5 removes, like the atlas's stage 3.1 (38 ms).

## Stage E5 (in progress): eye 1 overlapping eye 0

`WWHD_VK_EYE_THREAD=1` no longer waits for each draw:
- eye 0's changes to textures eye 1 may share cut the segments while the thread is behind, and are marked with eye 0's
  draw number;
- the thread checks the marks and switches itself off, logged, if it would use a texture eye 0 changed later;
- draw batching's submissions and the render thread running out of commands join it;
- the hand-over ring blocks when full.

**Bugs found and fixed on the way:**
- **A draw thread began the command buffer itself** right after a submission joined it, in the renderer's own stream,
  while the render thread recorded. `DrawThread::begin` now begins it first, on the render thread.
- **A cut inside a scope recording elsewhere** (the before-atlas stream) moved the scope's commands into eye 0's next
  segment, and the scope's exit put eye 0 back in the closed one. A change recorded before the atlas no longer cuts,
  a cut keeps an active redirect, and the scope restores the attached stream.
- **Draw threads ran the "present the last frame while a pipeline compiles" helpers** at three call sites the earlier
  guards missed. Their first step flushed the render thread's recorder from the other thread. Both helpers, and the
  capacity-pressure flush, now do nothing on a draw thread.
- **`aoPrivateReplay` was a plain global:** eye 1's ambient-occlusion replay flipped it on in the middle of eye 0's
  draws, which then rendered into the private AO images. That was the random darkening of the left eye that showed
  as flicker when new scenery appeared. It, and the kept AO images, are now per thread.
- **The eye thread's own render pass** is closed on the render thread before it starts, if the render thread drew
  eye 1 since the last flush.
- **Eye 1's shifted uniform copies** are now reserved by eye 0 in its own arena, so they are in the descriptor set's
  buffer and the replay is not refused.

**Checked (desktop stereo):** the title-screen flicker test (71 frames' left/right brightness) passes with the eye
thread alone and with both threads; frame 4200 byte-identical with defaults; 608 of 608 tests.

**Not yet good:** the eye thread makes the frame slower.

| desktop stereo, 120 Hz proxy | frame | render thread CPU |
|---|---|---|
| default | 17.4 ms | 14.3 ms |
| eye thread | 28.4 ms | 21.5 ms |
| eye thread and atlas thread | 19.6 ms | 14.4 ms |

On the thread, 2,621 of its 2,622 draws a frame are replays. Skipping the shader, pipeline and sampler locks (a
timing-only experiment) changed nothing, and handing draws over 16 at a time instead of one at a time changed nothing.
The cause is being profiled.

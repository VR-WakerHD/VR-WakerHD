# Recording the scene's draws on more than one thread (plan)

The plan for holding 90 fps in the busy scenes (docs/VR_PERFORMANCE.md, 2026-10-29). It is not started. Every stage
below is off by default, behind `WWHD_VK_SCENE_THREAD=1`, and leaves the game running as before.

## Why

In the headset (PS VR2, 90 Hz) the frame rate depends on the scene's draws a frame. The render thread is busy all
frame in the busy scenes; the GPU, the runtime and the game thread are not the limit:

| Draws a frame | fps | Render thread ops |
|---|---|---|
| 1,000 to 1,400 | 87 to 90 | 10.3 to 12.4 ms |
| 2,000 to 2,500 | 60 to 84 | 12.4 to 15.5 ms |
| about 7,000 (the start area) | 54 to 57 | 18.9 to 21.1 ms |

90 fps needs every frame's render-thread work under about 10.5 ms. The function-level profile shows the cost spread
over the per-draw path. No single part is the 2 to 4 ms the busy scenes are over by:
- `bind_stage` 10.8 % of the render thread's time;
- `multiview_draw` 8.6 %;
- textures, uploads, descriptor sets and targets 2 to 3 % each;
- register writes about 5 %.

Doing the same per-draw work on two threads is the change sized for that gap. The picture stays exactly the same: the
same draws, recorded into the same order of commands.

## What already exists (from the sun atlas's thread, docs/VR_ATLAS_THREAD.md)

The atlas's 500 to 3,000 draws a frame are prepared and recorded on a draw thread of their own since 2026-10-27, on by
default and confirmed in the headset. That work made most of the renderer safe for a second thread:

| Part | Made safe by |
|---|---|
| scratch space, counters, the upload arena, descriptor pools, bind state | per-thread state (`per_thread.h`) |
| shader, pipeline, sampler, surface, pass and buffer caches | `draw_threads::CacheLock` (locks only while a second thread runs) |
| the register file and its counters (shader generation, state version, dirty groups) | `ReplayMachine`, fed with every register write (`g_forward`) |
| commands | held streams per thread, emitted in order when the threads join |
| surfaces both threads use | the overlap rules (`atlas_overlap.h`): marks, joins, and a switch-off on a conflict |
| guest flushes, clears, copies, invalidates, the swap | join points |

## What is new for the scene

The atlas is one block that writes one image the scene reads only after a join. The scene's draws depend on each
other: a pass reads targets an earlier pass wrote (the scene colour for bloom and the heat haze, depth for the
ambient occlusion, the eyes' targets for the final copies). Splitting it needs four things the atlas did not:

1. **Image layouts across a split.** `transition_image` decides a barrier from each surface's current layout. A
   thread recording a later part of the scene does not know the layouts the earlier part will leave. Each part
   records with a layout view of its own:
   - the first use of a surface in the part notes the layout it needs;
   - the part's own changes go into its view, not the surface;
   - at the join, between one part's stream and the next, a barrier takes each surface from the earlier part's last
     layout to the later part's first need, and the surfaces take the last part's layouts.

   This is how a render graph records in parallel. The GPU runs the streams in the frame's order, so the pictures do
   not change.
2. **Eye 1's held commands per thread.** Multiview draws both eyes in one draw for most draws. Eye 1's own draws are
   held and flushed into the stream about 4 times a frame (`stereo_flush`). Each thread needs its own held eye 1
   stream.
3. **Where to split.** At render-pass boundaries, about 54 a frame, so no pass is recorded on two threads. From the
   last frame's draws per pass, the draw thread takes the passes after the split point that gives it about half the
   scene's draws. Clears, copies, guest flushes and the swap end a part, as for the atlas.
4. **Guest data between the parts.** The atlas's rule carries over: the game never rewrites memory queued GPU work
   still reads before its next `GX2DrawDone`. Both threads may upload the same guest texture or buffer data; that
   costs time, never a different picture. Invalidates are applied on the render thread at once, as now.

## Expected gain

At 2,300 draws a frame (render thread about 13.5 ms):
- **Moves to the draw thread:** about half the scene's draw work, 4 to 4.5 ms.
- **Stays on the render thread:**
  - the register writes (3 ms; the draw thread replays them too);
  - its half of the draws;
  - the swap;
  - the hand-over.
- **New costs:**
  - forwarding the register writes, about 0.3 to 0.5 ms (measured for the atlas);
  - the joins, about 0.5 ms;
  - the cache locks while both run, up to about 1 ms. The atlas's forced locks cost 1.3 ms on one thread; this needs
    measuring.

Net: about 2.5 to 3.5 ms less, so most of the busy scenes fit in 10.5 ms. The start area (7,000 draws) would go from
about 56 to about 70 fps. It needs a third thread, or stage S6 below, for 90.

## Stages

Each stage is checked before the next, and the default renderer is timed before and after it. The atlas's stages added
2 ms to the default renderer before that was found; this plan measures it at every stage instead.

| Stage | What | Done when |
|---|---|---|
| S0 (done) | **Measure the scene's shape.** `WWHD_VK_SCENE_STATS=1` logs per frame: draws per pass, which passes read which earlier targets, layout changes at pass starts, eye 1 flushes per pass. | A busy scene's numbers show where a split leaves the fewest joins and fix-ups. |
| S1 (done, as a split inside the pass) | **Layout views, on one thread.** The render thread records the scene's second part into a held stream with its own layout view, and joins with the fix-up barriers. No second thread yet. | Pictures identical; no Vulkan validation errors; the default renderer's time unchanged. |
| S2 (done: larger parts; eye switches keep the part) | **Eye 1's held commands per part.** | As S1, with multiview draws in both parts. |
| S3 (done) | **The second part on a draw thread, one at a time.** The render thread waits for it (no overlap), as the atlas's stage 3.1. | As S1; the thread check (`WWHD_VK_THREAD_CHECK=1`) lists no unsafe sharing. |
| S4 (done) | **Overlap.** The render thread records the first part while the draw thread records the second; they join at the part's end. Conflicts switch the overlap off for the session and are logged, as for the atlas. | Render-thread time down in a busy scene; pictures identical. |
| S5 | **The split chosen each frame** from the last frame's draws per pass. Hardening: scene changes, menus, save states, the telescope, cutscenes. | Long headset sessions without a conflict switch-off or a crash. |
| S6 | (if needed) **Register writes for the draw thread's part applied only there.** The render thread forwards them and takes the draw thread's register file back at the join. A third thread for the start area. | The start area's render thread under 10.5 ms. |

## S0 done: the scene's shape (2026-10-29)

`WWHD_VK_SCENE_STATS=1` (`scene_stats.h`/`.cpp`; hooks in `draw.cpp` at a pass's start, each draw and each sampled
texture, in `transition_image`, `stereo_flush` and `swap`). Run in desktop stereo at 3x from two frozen Great Sea
states, about 850 to 1,050 render-thread draws a frame (the sun atlas's draws are on its own thread and not counted).

**Per frame:**
- 51 render passes, most of them one full-screen draw each: depth downsamples, bloom and blur chains (10 mip levels),
  the final copies;
- 309 layout changes, about 6 a pass, nearly all in the post-processing passes;
- 12 eye 1 flushes;
- 16 passes sample an earlier pass's target (23 such reads, about 10 passes back on average).

**The draws sit in a few geometry passes** (3840x2160, both eyes in one multiview pass):

| Pass | Draws | Reads |
|---|---|---|
| 0 | 139 to 160 | nothing earlier |
| 19 (1920x1080) | 29 to 77 | nothing earlier |
| 20 | 95 to 103 | the depth downsamples (5, 7) |
| 26 | 40 to 49 | the depth downsamples |
| 27 | 194 to 409 | the depth downsamples |
| 49 | 64 | nothing earlier |
| 50 (2562x1440) | 151 | nothing earlier |

Pass 27 is the one that grows with the scene: it held from 194 to 409 draws across these frames while the others
stayed nearly fixed. In the busy scenes (2,000 to 7,000 draws) most of the frame's draws will be in it.

**The half-draws split between passes** comes before pass 27 and leaves the second part with about 2 surfaces the first
part wrote (the depth downsamples it samples) and almost no layout changes on them. But a split between passes cannot
halve a frame whose draws are mostly in one pass.

**What this changes in the plan:** the split goes inside the large geometry passes, chiefly 27. Inside a pass the
targets are fixed and there are only 2 to 4 layout changes (textures made ready for sampling). The second part's draws
continue the same open pass: its stream is emitted right after the first part's, before the pass ends. There are no
layout fix-ups on the targets, and the textures' layout changes can be made by the render thread before it hands the
part over.
- S1 becomes: the second half of a large pass's draws recorded into a held stream with its own draw-state cache,
  continuing the pass. Still on one thread.
- A split between passes stays possible for the post-processing that follows, but it is not where the time is.

**Pictures:** both eyes byte-identical with the statistics on and off, at frame 4200:
- from the first state: `90b7a867a73a4cfa` / `a37d407f7c2e9435`, the same in two runs;
- from the second: `e11e8626f58c2d5c` / `a4ea65341fc98399`.

The picture runs use a script outside the repository (fixed seed, interpolation off, desktop stereo, a separate user
folder with a frozen state and save, `WWHD_DUMP_FRAMES=4200`).

**Still to see:** the shape of a busy scene (2,000 to 7,000 draws). The frozen states are lighter Great Sea spots; one
headset run with `WWHD_VK_SCENE_STATS=1` in the start area would show it.

## S1 done: a part of the scene in a held stream, on one thread (2026-10-29)

`WWHD_VK_SCENE_SPLIT=1` (`stereo_switches.h` `kSceneSplit`; off by default).

**Where the part begins** (`draw.cpp` `scene_split_*`):
- in the two-eye pass that had the most draws in the frame before (at least 64);
- before its middle draw;
- only with eye 0 selected, from the renderer's own stream: nothing else held (the atlas's stream, the scene's while
  the atlas's thread runs, the eyes' segments) and no redirect active (`stereo_part_hold`).

**While it holds** (`stereo.cpp`):
- eye 0's commands go into the part's held stream (`attach_eye_stream`) and the pass stays open;
- the part records as a second thread would. `g_partEpoch` moves at its start, so:
  - the draw-state cache begins empty: pipeline, index and vertex buffers, viewport, scissor, blend and stencil
    values, descriptor sets, all bound again;
  - no draw continues one from before the part (`continued_reason`).

**When it is emitted:**
- when eye 0's pass ends in its stream (`end_encoder` calls `stereo_part_pass_ended`);
- before anything that must come after it (`stereo_atlas_emit` / `stereo_atlas_join` for any reason but a draw):
  - a `stereo_flush`: eye 1's held commands, submissions, in-order operations, the atlas's block, the swap;
  - an eye switch;
  - `peek_z` and the write-back.

  `stereo_part_join` ends eye 0's pass inside the part first.
- Either way it goes into the renderer's stream through `emit_held`, as the atlas's held stream does.

**Measured** (desktop stereo, the two frozen Great Sea states):
- one part a frame, always emitted at its pass's end (a flush ended one in 100 frames, once);
- the part held 92 to 165 draws, 11 to 17 % of the frame's: half of the main geometry pass (27).

**Pictures:** frame 4200 byte-identical in both eyes with the part on and off:
- the first state `90b7a867a73a4cfa` / `a37d407f7c2e9435`;
- the second `e11e8626f58c2d5c` / `a4ea65341fc98399`.

**Not measured:** the default renderer's time. With the switch off the hooks are constant tests, and the draw-state
cache and continued draw each gained one `uint64_t` compare. A timing A/B needs desktop stereo timing runs, which are
outside the picture runs allowed for these stages.

**For the threads:** half of one pass is too small a share to gain much, since the second thread would get 11 to 17 %
of these frames' draws. The parts must cover more:
- from the split to the end of the largest pass;
- the later geometry passes as parts of their own: 49 and 50, the HUD and the final copies, about 215 draws;
- the split point chosen so the second thread gets about half the frame's draws.

That is S2's first item, before eye 1's per-part held commands.

## S2 done: parts for the frame's second half (2026-10-29)

**Where the parts begin** (`draw.cpp` `scene_split_*`, from the frame before's passes):
- before the draw at which half of the frame's draws are done, if that draw is in a two-eye pass;
- at the start of every later two-eye pass that had at least 16 draws, right after it begins
  (`scene_split_pass_begin`).

A part still ends with its pass.

**An eye switch no longer ends a part.** Eye 1's draws go into eye 1's held stream as always, and eye 0's continue the
part once eye 0 is selected again. A `stereo_flush` (eye 1's held commands emitted) still ends it first.

**Measured** (desktop stereo, both frozen states):
- about 3 parts a frame, holding 390 to 468 draws: 47 to 48 % of the frame's;
- the main pass's part ends at its pass's end;
- the later passes' parts (the HUD and final copies, passes 49 and 50) end at a flush, twice a frame: eye 1's held
  commands are emitted there.

**Pictures:** frame 4200 byte-identical in both eyes from both states (`90b7a867a73a4cfa` / `a37d407f7c2e9435`,
`e11e8626f58c2d5c` / `a4ea65341fc98399`).

**For S3 (the part on a second thread):** eye 1's own draws inside a part go into eye 1's shared held stream. A part
recorded on another thread cannot write that stream, so S3's first form joins the threads before any draw that is not
drawn for both eyes. That is about 5 such draws a frame, mostly outside the main pass. A held eye 1 stream per part
comes only if that join costs too much.

## S3 done: the parts recorded on a thread of their own, one draw at a time (2026-10-29)

`WWHD_VK_SCENE_THREAD=1` (off by default; it turns `WWHD_VK_SCENE_SPLIT` on too, and per-thread state in
`per_thread.h`). The scene's thread (`draw.cpp` `scene_thread`) is a `DrawThread`, as the atlas's:
- a register machine of its own, loaded at each part's start, with every register write forwarded;
- its own pass state, counters and held stream;
- eye 1's thread's upload arena and descriptor pool (`R.eyeUploadBlocks`, `R.eyeDescriptorPool`, now also made with
  this switch). The two threads never run together: with `WWHD_VK_EYE_THREAD=1` the scene's thread stays off.

**A part on the thread:**
- **Its start** (S2's points): only while eye 1 holds no commands. The thread takes a copy of eye 0's open pass, the
  part's stream (`stereo_part_hold` calls `scene_thread::begin`).
- **Each draw** (`draw_stereo`):
  - The render thread hands it over and waits.
  - The thread draws eye 0's half, both eyes in one multiview draw where it can. `multiview_draw` now runs there; it
    did not on draw threads.
  - A draw not drawn for both eyes has its eye 1 half drawn on the render thread from the thread's preparation
    (`stereoShadersIn`), into eye 1's held stream as always.
- **Its end** (the threads join, `stereo_part_join`):
  - before a draw while eye 1 holds commands;
  - when the render thread would end eye 0's pass (`end_encoder`);
  - for S1's other reasons: a `stereo_flush`, `peek_z`, the write-back.

  The thread ends eye 0's pass at the end of the part, the render thread marks its copy of that pass closed, and the
  part is emitted as in S1. The thread's own pass changes inside a part stay in it, so a part can now span several
  passes.

**Measured** (desktop stereo, both frozen states):
- about 3 parts a frame, 47 to 48 % of the frame's draws on the thread;
- the parts end at a flush twice a frame and when the render thread ends eye 0's pass once a frame, never at their
  own pass's end.

**Pictures:** frame 4200 byte-identical in both eyes:
- from both states with the thread on (`90b7a867a73a4cfa` / `a37d407f7c2e9435`, `e11e8626f58c2d5c` /
  `a4ea65341fc98399`);
- with the switch off.

**No gain yet, by design:** the render thread waits for every draw. The cache locks are also on while the thread runs
(`set_second_thread_active`), so this is slower than without it. S4 lets the render thread go on while the thread
records.

**What S4 has to solve:**
- The render thread may not record anything while the thread records: today everything it would record either joins
  first or goes into the part's stream in order, and with an overlap that order is lost.
- The eye 1 halves need the thread's preparation of their draw. With an overlap the render thread must wait for that
  one draw, or the eye 1 half moves to the thread with a held eye 1 stream per part.
- The draws' statistics (`scene_split_draw`) and the multiview counters are written by both threads.

## S4 done: the parts recorded beside the render thread (2026-10-29)

With `WWHD_VK_SCENE_THREAD=1` the render thread hands each draw of a part over and goes on (`scene_thread::draw`, no
wait).

**On the scene's thread:**
- It draws both halves of a draw. One not drawn for both eyes has its eye 1 half drawn there too, with eye 1's pass
  state, into the part's eye 1 stream (`partEye1Commands`). At the join that stream goes into eye 1's held stream, which
  is empty: a part starts only without eye 1 commands.
- After such a draw, the part's later draws there are two draws as well (`multiview_draw` refuses while the part's
  eye 1 stream holds commands), so eye 1's commands stay in order. The render thread ends the part before its next
  draw (`eye1Drawn`).

**The render thread records nothing while the threads overlap:**
- Everything it would record joins them first: `dispatch_recorder` tests `g_partOverlapping`, one load on the hot path.
- It also joins before the commands that change state draws read outside the registers: `OP_SET_PROJ_REGS` (the
  layouts' stereo tag), `OP_LAYOUT_CONTENT`, `OP_LAYOUT_ROOT`, and the clouds' begin and end
  (`render::before_draw_state`, a new backend hook).
- It joins when it runs out of commands (`before_idle`).

  A `GX2DrawDone` joins them through its flush and write-back, before the game may rewrite the memory the part's draws
  read.

**A first version without the layout joins drew the HUD wrong:** frame 4200 differed in 116,215 pixels, all in the HUD
(the hearts, the buttons, the rupees). The 3D scene was identical. The render thread had already applied later
layouts' state when the thread drew earlier HUD draws. In S3 the render thread waited at every draw, which hid it.
`scratchpad` `pngdiff.py` (standard library only) shows where two pictures differ.

**Measured** (desktop stereo, both states):
- 2 parts a frame, 216 to 230 draws on the thread: 25 to 26 % of the frame's;
- both end for "other" reasons: the layouts' state at the HUD, and the render thread's own recording. The HUD passes'
  parts end at their first layout command, so only the main pass's part is long.

**Pictures:** frame 4200 byte-identical in both eyes:
- from both states with the overlap on (`90b7a867a73a4cfa` / `a37d407f7c2e9435`, `e11e8626f58c2d5c` /
  `a4ea65341fc98399`);
- with the switch off.

**Not measured yet:** the render thread's time. This is the first stage that can gain. While the scene's thread records
a part, the render thread applies the part's register writes and hands its draws over. The register writes are about
3 ms a frame in the busy scenes, and the thread meanwhile prepares the draws (about 5 us each). It needs a headset run,
or a desktop timing run, with `WWHD_VK_SCENE_THREAD=1 WWHD_PROFILE=1` against the switch off, in a busy scene.

**Next, by measured value:**
1. The HUD's parts: hand the layout state over with each draw instead of joining. That is about 215 more draws a frame
   on the thread.
2. Start the main pass's part earlier than its middle. The thread's share is what the render thread gains, as long as
   the thread keeps up.

## S4 measured in the headset: no gain (2026-10-29)

Headset run with `WWHD_VK_SCENE_THREAD=1 WWHD_PROFILE=1`. The player switched rates during it: 120 Hz at half, then
90 Hz at the full rate, so frame rates are not comparable. The render thread's ops a frame are, against the clean run
without the thread (`WWHD_PROFILE=1` only), by draws a frame:

| Draws a frame | Without | With S4 | Draw ops | Register ops | Other ops (joins) |
|---|---|---|---|---|---|
| 900 to 1,499 | 11.99 ms | 11.30 ms | 6.47 to 5.25 | 1.88 to 2.04 | 0.14 to 1.79 |
| 1,500 to 1,999 | 12.80 ms | 12.90 ms | 7.07 to 6.19 | 2.44 to 2.64 | 0.14 to 2.47 |
| 2,000 to 2,699 | 13.79 ms | 15.60 ms | 9.00 to 6.84 | 3.12 to 3.65 | 0.14 to 3.28 |

The thread took 30 to 34 % of the draws.

**The two halves of the run, separately** (120 Hz at half first, then 90 Hz at the full rate):

| | 120 Hz, capped at 60 | 90 Hz, capped at 90 |
|---|---|---|
| Frames shown | 59.6 to 60.0 a second | 89.8 to 89.9 in the lighter scenes; 65 to 79 in the busy ones |
| Uneven frames per 10 s | 0 to 3 of 600 | 1 to 2 of 900 light; 213 to 383 of 700 to 800 busy |
| Frame interval p99 | 18.9 to 20.2 ms | 12.5 ms light; 18.7 to 23 ms busy |
| Render-thread ops, 900 to 1,499 draws | 11.61 ms | 11.24 ms (11.99 without the thread) |
| Render-thread ops, 1,500 to 1,999 draws | 13.06 ms (one window) | 12.89 ms (12.80 without) |
| Render-thread ops, 2,000 to 2,699 draws | 15.96 ms (16 windows), joins 3.35 ms | 13.68 ms (3 windows; 13.79 without) |

- **At 120 Hz capped at 60** the frames were even, about 60 a second with no judder. That holds even in the busy
  scenes, where the render thread's 16 ms stayed just under the 16.7 ms a 60 fps frame has.
- **At 90 Hz** the scene's thread changed the render thread's time by under 1 ms in each range, within the run-to-run
  spread. The busy scenes still judder at 65 to 79 fps, as without it.
- The joins cost more at 120 Hz (3.35 ms in the busy range). The ops a frame at different caps are not directly
  comparable, though: the waits inside them depend on the frame's time.

**Why:**
- The render thread hands the part's draws over quickly, but at the next join (the HUD's layout commands, every frame)
  it waits for the thread to finish them. That wait is the "other" ops, 1.8 to 3.3 ms.
- The thread prepares each draw no faster than the render thread did. The cache locks it needs are on for both threads
  while it runs, and the register writes are forwarded (register ops +0.2 to +0.5 ms).
- So the render thread gains only what it does in parallel: the part's register writes. In the busy scenes the locks
  and forwarding cost more than that.

**What it means for the plan:** a second thread pays only if the render thread also prepares draws while it records,
for example the render thread recording the draws after a part while the thread records the part. That is the
conflict model the atlas's thread needed (`atlas_overlap.h`). It is harder for the scene, because its later draws
sample the earlier ones' targets: the post-processing samples the main pass. The overlap would have to end at the
first such draw, and S0's numbers say that comes right after the main pass.

S4 stays off by default (`WWHD_VK_SCENE_THREAD`); the default renderer is unchanged.

## Why S4 gains nothing, diagnosed (2026-10-29)

Desktop stereo picture runs from the first frozen state. They are diagnostics only: `[vulkan scene thread] per frame`
in the log, with `scene_thread`'s join, emit and draw timers. Pictures byte-identical in every run.

| What | Result |
|---|---|
| How the parts reach the renderer's stream | all queued to the record worker whole (2 a frame, 0 replayed); emitting takes 0.01 ms |
| The render thread's wait at the joins | 1.21 to 1.30 ms a frame |
| The scene thread's time per draw | 6.63 to 7.12 us (its draws: the main pass's second half) |
| The render thread's own draws in the same pass, before the split | 6.71 to 7.70 us |
| The scene thread with its priority raised (`host::boost_thread_priority`, now done) | unchanged: 6.71 to 6.97 us |
| Both threads kept on one L3 group (`WWHD_VK_SCENE_THREAD_SAME_L3=1`, mask `0x3F`) | unchanged: 6.63 to 7.12 us |
| The scene thread, sampled (`WWHD_SAMPLE_THREADS`, `-g` build) | idle 95.5 % (its job queue); lock waits about 0.5 %; its draw time spread over the usual draw path |

**The scene thread is not slower.** A main-pass draw costs about 7 us on either thread. (An earlier 3.8 us figure, from
S2's parts on the render thread, included the HUD's cheaper draws.)

**What limits S4 is the render thread's work while a part runs.** It applies the part's register writes and hands the
draws over, about 1.2 us a draw, while the scene thread takes about 7 us a draw. So it runs ahead, then waits for the
backlog at the join. It gains only the register work it did meanwhile; the forwarding, the handover and the locks take
that back.

**The fix this points to: the scene thread does the scene, the render thread only decodes.** If the part covers the
scene from its first draw to the swap, the frame takes:
- the render thread: the command stream and register writes (about 3 to 3.5 ms a frame in the busy scenes) plus the
  hand-overs;
- the scene thread: the draws, about the render thread's draw time today (8 to 10 ms in the busy scenes).

They run at once, so a busy frame's 13.5 ms of render-thread work becomes about 10 to 10.5 ms on the slower of the
two. That is under the 11.1 ms of 90 fps, and S0 to S4's groundwork mostly carries it. What it needs (S5):
1. The part starts at the scene's first draw, not halfway.
2. No joins inside the scene: the commands that join today go to the scene thread as jobs, in order. That covers the
   layouts' state (`OP_SET_PROJ_REGS`' tag, `OP_LAYOUT_CONTENT`, `OP_LAYOUT_ROOT`), the clouds' begin and end, and the
   clears and copies. The render thread no longer touches the state they change while a part runs.
3. Eye 1's halves stay on the scene thread (S4).
4. The joins that stay are the swap, `GX2DrawDone` and the guest's flushes, a few a frame.

## S5 built: the scene's thread records the scene; no gain (2026-10-29)

With `WWHD_VK_SCENE_THREAD=1`:
- **Parts start at the scene's first draw a part can begin at,** and again after each join. Before about draw 190 the
  atlas's thread overlaps the scene, and no part starts.
- **Handed over instead of joining:**
  - the layouts' state (`render::hand_draw_state`: `OP_SET_PROJ_REGS`' tag, `OP_LAYOUT_CONTENT`, `OP_LAYOUT_ROOT`);
    the machine's full state change for a photo pane stays on the render thread and is forwarded after it;
  - the clears, surface copies and the copies to scan (`scene_thread::clear_color`, `clear_depth_stencil`,
    `copy_surface`, `copy_to_scan`). Their guest structs are copied with the job, and the thread runs each as
    `stereo_pair` would, with an in-order scope of its own (`stereo_thread_in_order`);
  - eye 1's held halves: the thread flushes them into the part itself where `stereo_flush` would
    (`scene_thread::flush_eye1`: from `multiview_draw`, `stereo_release_flush` and before an in-order operation).
    They stay POD (`DeferredCommands::append_held`), so the part still goes to the record worker whole.
- **One join a frame:** the guest's flush at the frame's end.

**Pictures:** frame 4200 byte-identical in both eyes at each step.

**Measured** (desktop stereo, the first frozen state, `WWHD_PROFILE=1`):

| | Without the thread | S5 |
|---|---|---|
| Render thread in ops | 8.06 to 8.67 ms | 10.66 to 10.68 ms |
| Render thread CPU | 5.99 to 6.77 ms | 4.30 to 5.08 ms |
| Of it: the guest flush op (the join) | 0.03 ms | 4.38 ms |
| Draws | 4.97 to 5.63 ms, all of them | 1.45 ms, the hand-overs and the first 190; plus 4.0 to 4.3 ms on the thread (711 draws, 6.0 us) |
| Register writes | 1.93 ms | 2.6 ms (forwarded) |
| The cache locks (`WWHD_VK_LOCK_ALWAYS=1` on one thread) | +0.5 ms | |

- The scene thread's draws together cost about what they did on the render thread, plus the locks: it is not slower.
- **Of its 4.1 ms, 2.9 ms come while the render thread waits at the join.** The render thread hands the part over in
  about 1.3 ms, about 1.8 us a draw with its register writes, then reaches the flush and waits.
- The thread is not blocked meanwhile: sampled, it is idle 85.8 % of the time in its job queue and in its locks under
  1 %.

**Why a reader/drawer pipeline cannot gain here:** reading the commands and applying the registers is about a third of a
draw's cost. While one thread reads and the other draws, the drawing thread is the frame's length. The pipeline saves at
most the reading, and the forwarding and locks cost about that much.

**What would gain:** two threads drawing at once. The render thread reads and hands over, and two drawing threads record
alternate parts of the scene, each in its own stream, emitted in order. The frame's drawing then takes about half as long
(711 draws: about 2.2 ms instead of 4.3).

The two drawing threads' parts then overlap, and that needs the conflict rules the atlas's thread has (`atlas_overlap.h`):
- a surface one part changes (an upload, a layout change, a render into it) that the other part uses;
- the second part's draws sampling the first part's targets.

Inside the main geometry pass, its two halves only draw into the pass's own targets and sample settled textures, so
they conflict little. Across passes (the post-processing samples the main pass) they conflict at once. The candidate is
therefore: the main pass's draws split between the two drawing threads, the rest of the frame on one of them.

S5 stays off by default (`WWHD_VK_SCENE_THREAD`).

## S5b in the headset: no gain, and the camera shakes (2026-10-29)

**S5b** (`WWHD_VK_SCENE_THREAD=1`; `WWHD_VK_SCENE_HELD=1` keeps S5's held stream): while a part runs, the scene's
thread records into the renderer's own stream, published to the record worker as it goes. Eye 1's held commands go to
the worker after it (`stereo_part_append_eye1`). Pictures not checked: the run stopped before frame 4200.

**Measured in the headset** (90 Hz, Capped 90, `WWHD_PROFILE=1`):

| | Default build | S5b |
|---|---|---|
| Light (1,150 to 1,330 draws) | 7.7 to 9.2 ms render-thread CPU, 89 to 90 fps | 11.1 to 11.5 ms in ops, 77 to 88 fps |
| Busy (about 2,500 draws) | about 60 fps | 60 fps; the thread 574 to 911 draws at 5.7 to 7.3 us, the render thread waiting 3.5 to 5.6 ms at the flush |

The scene's thread draws no faster than the render thread did; the render thread hands over and waits. In the light
scenes the hand-over and the wait cost more than the drawing they move, and the frames miss 11.1 ms.

**The scene's thread only in the busy frames:** used after a frame of `WWHD_VK_SCENE_THREAD_MIN_DRAWS` scene draws
(1,400 by default; dropped again under nine tenths of it). Two headset runs: the light scenes at 89.9 to 90 fps again
(253 of 278 windows at 89 fps or more), the busy ones at 60 as before.

**A shaking camera, most likely SteamVR's, not the thread's.** Three runs with the switch on shook and blurred when
moving. In the last of them the runtime ended the session ("the runtime ended the session (exiting)"), and the game went
on on the desktop. Every run after that was smooth: the switch off, then the switch on with the same build (and only a
logging counter added). The game's own numbers did not follow what the headset showed:

| Run | Camera | Frames shown unevenly (`[vr] frame times`) | Missed display periods |
|---|---|---|---|
| Switch on, before the session ended | shaky | 157 of 821 | 79 |
| Switch off, after | smooth | 104 of 846, 145 of 825, 114 of 662 | 54, 75, 57 |
| Switch on, after | smooth | 351 of 711, 187 of 804, 275 of 758 | 175, 96, 140 |

Ruled out on the way:
- **The head pose's prediction** (`vr_game.cpp` `display_time`, counted by `[vr] camera pose predicted ahead`): one
  frame ahead in all but 3 of about 3,300 camera frames, with the switch on. It does not jump with the swap's timing.
- **The game's GX2DrawDone:** it goes through the guest flush, which joins the scene's thread first.
- **Uniform reuse:** per thread, and compared byte for byte.

If the camera shakes again with the frame rate held, restart SteamVR first.

**S6 can go ahead:** the shake was not the thread's.

## S6 plan: two drawing threads in the main pass (not started)

### The idea

The render thread reads the commands and hands the draws over, as in S5. **The main geometry pass's draws go to two
drawing threads at once:** the scene's thread A as now, and a second thread B. The rest of the frame stays on A.

A frame then runs as:

| Render thread | Thread A | Thread B |
|---|---|---|
| reads and hands over | the scene up to the main pass's middle (A1) | |
| hands the main pass's second half to B | (A1 still recording) | the main pass's second half |
| hands the rest of the frame to A | waits until B is done, then the rest (A2) | |

The four streams go into the renderer's in this order: before-B (below), A1, B, A2. The GPU gets exactly today's commands
in today's order, with two more pass breaks: A1 ends the main pass, B begins it again with LOAD and ends it, A2 begins
what follows.

**The gain:** A1 and B overlap. The main pass's draws take about half as long. In the lighter scenes the main pass is
200 to 400 of 850 to 1,050 draws a frame (S0). In the busy scenes it is the pass that grows, but its share there is not
measured yet.

| If the main pass is | Its draws, at about 6 us | Saved by halving |
|---|---|---|
| 1,000 of 2,300 draws | 6 ms | about 3 ms |
| 1,500 of 2,300 draws | 9 ms | about 4.5 ms |

That would bring a busy frame from about 13.5 ms to about 9 to 10.5 ms, under the 11.1 ms of 90 fps.

### Why this split, and not another

- **Inside the main pass the draws are independent of each other:** they draw into the pass's own targets, in the
  GPU's order, and sample textures that are settled for the frame.
- **The passes after it sample its targets** (the post-processing, S0), so splitting there would conflict at once.

### What B starts with

- **Registers:** a register machine loaded from the render thread's registers at the switch (`DrawThread::begin` does
  this), with the writes forwarded to B only while B's span runs. When the render thread switches back to A, A's
  machine is loaded again from the render thread's registers (a `kLoadRegs` job and a full state change) and
  forwarding goes back to A.
- **No open pass:** B's first draw begins the main pass again with LOAD, from the same targets. A's last A1 job ends A's
  pass, so there is no open pass to hand over.
- **Its own upload arena and descriptor pool:** a third pair beside the renderer's and eye 1's thread's (A's), made per
  submission as they are.
- **Its own eye 1 stream, and its own draw-state cache and continued draws** (as A's, S3).

### Where B's span ends

The render thread switches back to A:
- at the predicted end of the main pass (last frame's draw count for it);
- at once on any command that is not a draw or a register write: a layout state, a clear, a copy, a flush, the swap.
  Inside the main pass there are none.

B's span is then only draws and register writes.

### The conflict rules (the hard part, after `atlas_overlap.h`)

B is earlier on the CPU but later on the GPU than A1's last draws. Everything B does to shared state must stay right for
A1, and the other way round.

1. **Game textures B is the first to prepare** (upload, layout for sampling), surfaces nobody rendered (`!gpuWritten`):
   their commands go to a **before-B stream, emitted before A1.** A1 then finds them ready, as the atlas's thread finds
   the scene's preparations (S3.2 of VR_ATLAS_THREAD.md). That is valid because their guest data is fixed until the
   frame's `GX2DrawDone`.
2. **Anything else B would change** (a rendered surface's layout, a render into a target other than the main pass's, a
   surface's image): **B waits for A1 to finish first** (a flag A sets at its last A1 job). From there B is sequential
   after A1, and may do anything A1 could.
3. **The main pass's own targets:** both threads only render into them, as attachments, without a layout change.
   Unchanged by either: no rule needed.
4. **Eye 1's halves in B:** B waits for A1 first (rule 2), then takes A1's eye 1 stream ahead of its own, so eye 1's
   commands stay in draw order through B's flushes.
5. **What A1 changes that B uses:** A1 is earlier on the GPU, so B seeing A1's changes is the unsplit order. What B
   decided before A1's change, on a game texture, is rule 1. On anything else B has waited (rule 2).
6. **The caches** (shaders, pipelines, samplers, surfaces, buffers): the existing `CacheLock`s, now with three threads.
   The statistics both threads write (`multiviewWhy`, the split's counters) become per thread or atomic.
7. **Checks:** a thread-check run (`WWHD_VK_THREAD_CHECK=1`) for areas both use, and a debug mode where B waits for A1
   at every draw (sequential, the same streams), which must give the same pictures.

### S6.0 done: the main pass's share, in the headset (2026-10-29)

`WWHD_VK_SCENE_STATS=1` in the headset (90 Hz at the full rate). The statistics count the render thread's scene draws
(900 to 1,410 a frame on average in this run). The profiler's draw ops (2,500 to 2,700 in the busy windows, 6,760 in
the start area) also count the hand-overs of the atlas's draws to its thread.

| Frame | Scene draws | The main geometry | The first pass |
|---|---|---|---|
| busy (58 passes) | 1,685 | passes 28 to 34, all two-eye: 933 draws (55 %); the largest one 432 | pass 0: 322 (19 %) |
| lighter | 947 | pass 27: 243 (26 %) | pass 0: 194 |
| lighter | 1,130 | passes 27 to 29: 470 (42 %) | pass 0: 171 |

- **In the busy frame the main geometry is seven consecutive two-eye passes,** broken by layout changes between them
  (textures made ready on their first use), not one pass. S6's split covers that run, so B crosses those breaks. Rule 1
  covers the ones for game textures; anything else waits for A1 (rule 2).
- **Its share, 55 %, is above S6.0's third.** Halving it saves about 933 / 2 x 6 us = 2.8 ms of the busy frame's draw
  time.

**A second cost found on the way, the record worker's burst:**
- Today the record worker replays the renderer's stream into Vulkan calls while the frame is built.
- A part held until its join reaches the worker in one piece: the worker waits all part long, then has the whole part
  to do before the present. In S5 the swap op went from 0.8 to 1.6 ms.
- While a part runs the render thread records nothing (S5's joins), so the scene's thread could record into the
  renderer's own stream instead, published to the worker as it goes. That is a smaller change, worth doing before S6
  (S5b): it removes the burst, and S6's streams can then be published in order as each is finished.

### S6.1 built: thread B, idle (2026-10-29)

With `WWHD_VK_SCENE_TWO=1` (and `WWHD_VK_SCENE_THREAD=1`):
- **Thread B** (`scene_thread::worker_b`, the same `Worker` as the scene's thread, made with its own arena and pool):
  made when the scene's thread is first enabled, then idle.
- **Its upload arena and descriptor pool** (`scene2UploadBlocks`, `scene2DescriptorPool`): made, reset and handed on
  with each submission, as eye 1's are.
- **Its held streams** (`stereo_part_b_stream`, `stereo_part_b_eye1_stream`) and **their emission**
  (`stereo_part_emit_b`): its eye 0 commands to the record worker after the renderer's, its eye 1 halves after the
  scene's thread's in the part's eye 1 stream. Not called yet (S6.2).

**A change to the plan: no before-B stream for now.** Since S5b, A1 records into the renderer's own stream, published to
the record worker as it goes, so nothing can be emitted before it. Rule 1 therefore becomes rule 2 for now: a game
texture B would be the first to prepare makes B wait for A1, as anything else it changes does. Safe; it may stall B.
S6.3 measures the stalls; if they cost, A1 is held again (S5's mode) and the before-B stream added.

Builds. Pictures not checked (the S5b check is still open too).

### S6.2 and S6.3 built: B draws the main run's later half (2026-10-29)

With `WWHD_VK_SCENE_TWO=1` (and `WWHD_VK_SCENE_THREAD=1`); `WWHD_VK_SCENE_TWO_SEQUENTIAL=1` is S6.2's mode (B starts
only after A1), the default S6.3's (A1 and B overlap). Built only: no pictures, no runs yet (they come with the tuning).

**Where B's span is** (`draw.cpp` `scene_split_frame_end`): every scene draw gets the frame's number for it on the
render thread (`sceneSplit.drawIndex`, handed over with the draw). Each pass is marked by its first draw's number, the
render thread's and A's in one list, B's in another; at the frame's end they are put in order and the longest run of
consecutive two-eye passes is the main geometry run, `[first, end)`. The next frame's part switches to B at the run's
middle (at least 64 draws on each side) and back at its end, or at once at anything not a draw (a layout state, a
clear, a copy, the part's join).

**The switch to B** (`switch_to_b`): A's jobs so far handed over, forwarding to A stopped, B begun (its machine loaded
from the render thread's registers, forwarding to it), a fresh pass. **The switch back** (`switch_back`): B ends its
passes and its eye 1 halves follow its eye 0 commands (`kEndB`), it finishes without the render thread waiting
(`finish_async`), and signals done. A gets `kEndA1` (its passes end, its eye 1 halves follow, A1 is done; it waits for
B, then B's stream follows its own: `stereo_part_emit_b`), then the render thread's registers as they are now, then
A2's draws. The GPU order is A1, A1's eye 1 halves, B, B's eye 1 halves, A2.

**What B may change beside A1** (`atlas_overlap.h` `before_change`, `scene_thread::b_before_change`):
- **rendering into a target**: both only draw into the main pass's targets (rule 3);
- **anything else** (a layout, an upload, an image destroyed): B waits for A1 first.
  - With no cache lock held (`draw_threads::held_locks`), it waits there: A1 never waits for B.
  - Inside a lock A1 may want, waiting could never end. The draw is undone instead: B's two streams are cut back to
    before it (`DeferredCommands::mark` / `rollback`, `PodCommands::truncate`), its passes restored, its caches
    cleared, and it is drawn again after A1 (`Worker::draw_b`).
  - If the draw cannot be undone (a sealed or owned command since its start), the change goes ahead after A1's
    commands, and the split is off for the session, with a log line (a conflict).

So rule 1 (B's first preparations of game textures) is rule 2 here, as S6.1 said.

**Thread state:** each drawing thread has its own part epoch (`part_epoch`, `t_partEpochSalt`): B's draws bind
everything at its span's start and after an undone draw, and A2's after B's commands. B's eye 1 holds have a
generation of their own (`Surface::eyeHeldB`), so A's flushes do not hide B's holds.

**Not checked yet, to look at in the tuning:**
- **Pictures**, in both modes, from both frozen states.
- **Undoing a draw by throwing from inside the surface functions**: code that catches everything there without
  throwing again would hide it; a draw's partial changes to shared CPU state before the hook (a surface made, a cache
  entry) are kept.
- **Plain fields both threads write**: `stereoFlushGeneration`, the multiview statistics (`multiviewWhy`), a
  surface's own fields under no lock. The counters are diagnostics; the surfaces' are the risk.
- **The prediction** when the scene changes between frames (the run moves): the switch is late or early, which
  costs only speed.
- **The log line:** `[vulkan scene thread] second drawing thread ...` every 600 frames: spans, B's draws, the run's
  size, B's waits (in place, undone draws), A's wait for B, conflicts.

### S6 in the headset, and the first tuning (2026-10-29)

**First run** (S6.3, the busy-frames rule): no errors, no conflicts. When used, B drew about 300 draws a frame, but
almost every span stopped early to wait for A1 (0.38 draws undone a frame): B waited 1.75 ms a frame, A waited 1.35 ms
for B. The threads mostly took turns, and the busy views stayed at 61 to 69 fps (60 before). Missed display periods
12.2 %, as without the thread (12.4 %).

**Tuning 1: B's textures made ready ahead** (`scene_thread::prepare_b_textures`, as the atlas's thread's `begin`): B
notes each texture it samples (`surfaces.cpp` `sampled_texture`, `scene_thread::note_texture`); at the next part's start
the render thread makes them ready (their uploads if they changed, their layouts for sampling) before the part, so B
finds them settled. The log line now counts B's waits by what it waited before (a layout, an upload, an image
destroyed, other) and the textures made ready. Built; not run yet.

**Tuning 1 in the headset:** no errors, no gain. B still waited about once a span, always before a layout (0.18 to 0.22
a frame), with 3 to 11 textures made ready a frame: the layout it waits for is not one of last frame's sampled
textures. Reported as shaky everywhere. The game's own numbers show nothing of it: the camera's pose predicted one frame
ahead throughout, frame intervals as in the other builds.

**Tuning 2: B's own layout state** (a race found by reading the code): the scene's thread applies the layout state
handed to it (`stereo::layoutDrawDepth`, `stereo::layoutPhotograph`, `aspect::content_clip`) in order with its draws,
while B draws beside it and read the same globals, so B could see them earlier or later than its draws' place. The
draws' eye offset depends on them (`draw.cpp` `cameraEye`, the eye 1 shift, `shaders.cpp`): a 3D draw taken for a
layout's is drawn without its stereo shift. Now the render thread follows what it hands over (`scene_thread::g_handed`)
and gives B a copy at the switch (`kPass`); B reads its copy (`stereo::t_layoutOverride`, `layout_draw_depth`,
`layout_photograph`, `aspect::set_thread_content_clip`). Built; not run yet.

### Stages

| Stage | What | Done when |
|---|---|---|
| S6.0 (done: 55 % in the busy frame) | **The main pass's share in a busy scene:** `WWHD_VK_SCENE_STATS=1` in the headset, in the start area and the busy sea scenes. | The busy scenes' main-pass draws per frame are known; if under about a third, stop here. |
| S6.1 | **Thread B,** idle: its arena, pool, streams; the four-stream emission (before-B, A1, B, A2). | Builds; pictures identical with B off. |
| S6.2 | **B in the main pass, sequential:** A2's start waits for B, and B waits for A1 before every draw. | Pictures identical; no gain expected. |
| S6.3 | **A1 and B overlapping,** rules 1 to 5. | Pictures identical from both frozen states; the overlap's share and the waits logged. |
| S6.4 | **Measured,** desktop then headset. | The busy scenes' render-thread time and the headset's frame rate against S5's and the default. |

### Cost and risk

- **Cost:** high. Three drawing streams and their order, a third arena and pool, the before-B stream, and the wait
  between A and B. The registers' handover between A and B is the part most likely to go wrong.
- **Risk:**
  - **Wrong pictures** from a missed rule. Rule 2's "wait for A1 first" is the safety net, and the pictures are checked
    at every stage.
  - **A deadlock between A and B:** A waits for B only after A1, and B waits only for A1, never for A2.
  - **No gain** if the main pass is a small share of the busy scenes. That is why S6.0 comes first.

## Cost and risk

- **Cost:** high. The atlas's thread took about ten stages over two days. This reuses most of that groundwork, but
  S1 (layout views) and S2 (eye 1 per part) are new parts of the renderer's core.
- **Risk:**
  - **Wrong layouts:** flickering, black or garbage textures, or a GPU fault. S1 proves the layout views on one thread
    before any threading.
  - **Wrong or missing draws:** a part emitted out of order, or a draw seeing another part's state. The thread check
    and the overlap rules catch these.
  - **Deadlocks:** a join while a cache lock is held (counted, and it switches off, as for the atlas).
  - **A slower default renderer:** measured at every stage.
- **Rollback:** every stage sits behind `WWHD_VK_SCENE_THREAD=1` until the headset sessions say it is ready.

## Checking

The atlas's stages were checked with byte-identical frame pictures in both eyes (fixed seeds, interpolation off, frame
4200), the test suite, and desktop stereo timing runs. Under the current rule, tests are run in the headset by the
player only. So each stage here needs one of:
- the player's go-ahead to run those automated picture comparisons (they are the only check that shows a 1-pixel
  difference);
- or a headset check by the player at each stage: no flicker, no wrong textures, no missing effects, in the busy
  scenes, the start area, and with the menus.

## Before starting

- Take the eye 1 lookup reuse out (it gave no gain).
- Build `build/game` without `-g` again.

## S6 retested, and S7: the render thread draws beside the thread (2026-10-31)

**S6 retested** (desktop stereo, 90 Hz schedule, Capped 90). S6 as built never starts on the desktop views (its threshold
is 1,400 main-scene draws). Forced on every frame (`WWHD_VK_SCENE_THREAD_MIN_DRAWS=0`) it lowers the render thread's CPU
time by 1.4 to 3 ms but leaves it less spare time: the render thread waits about 1.9 ms a frame for the scene's thread,
which draws more slowly (6.1 against 4.6 us a draw). In the heaviest saved view (`vr-test/return.wwstate`, 2,630
draws) it misses 90: 12.0 ms frames, the longest 22 ms. Note: `WWHD_VK_SCENE_SPLIT` and `WWHD_VK_SCENE_THREAD` turn the
batched same-state runs off (`batch_can_hold`), so every S1 to S6 run is also a run without batching.

**S7** (`WWHD_VK_SCENE_FRONT=1`, off by default; not with `WWHD_VK_SCENE_THREAD`): S6's second drawing thread draws the
front of the frame's longest stretch of draws into the same targets (last frame's), while the render thread reads on;
at the span's end the render thread draws the rest of the stretch itself, into the part, beside the thread. The GPU
gets the thread's commands, then the part's: the guest's order. No scene thread, and batching stays on (a held run is
drawn piece by piece only while the thread's span runs).
- **The overlap's rules** are the atlas's thread's (`atlas_overlap.h`): the thread comes first on the GPU, so the render
  thread marks what it uses and changes (`Surface::frontRenderUse`, `frontRenderChange`), the thread marks what it uses
  (`frontUse`), and a change one makes to what the other used in this overlap is a conflict: S7 off for the session,
  logged. Rendering into the stretch's targets is both threads' and is not a change.
- **The threads join** (`front_join`, from the part's emission) at a draw into other targets or sampling the stretch's,
  a flush (submissions, in-order operations, the swap) and anything else that emits the part. Not at an invalidate (as
  in S5 and S6: the game invalidates between the stretch's draws). Eye 1's held halves go into the part without a join
  (`stereo_release_flush`), as in the atlas's overlap. The render thread's own pass ending in the part does not end it.
- **The split** starts at the stretch's first draw it can (eye 1 holding nothing, no atlas overlap) and gives the thread
  `WWHD_VK_SCENE_FRONT_SHARE` of what is left (0.5 to start, then balanced: less after a join that waited more than
  0.1 ms, more otherwise, within 0.15 to 0.85); stretches under `WWHD_VK_SCENE_FRONT_MIN_DRAWS` (256) are not split.
- `[vulkan scene front]` every 600 frames: the stretch, spans, both threads' draws, the wait, the share, the joins, why
  a span did not start, conflicts.
- A build fix found on the way: on Windows an inline constant built from other inline constants (`kSceneHooks =
  kSceneSplit || kSceneFront`) read them still false (each inline variable's initializer is its own COMDAT, run in no
  set order); the S7 switches read the environment themselves.

**Measured** (desktop stereo, 90 Hz schedule, `WWHD_VR_FRAME_RATE=full`, `WWHD_PROFILE=1`, gameplay windows' medians):

| View | Setup | Render thread CPU | Spare (idle) | Longest frame |
|---|---|---|---|---|
| `return` (2,630 draws) | off (2 runs) | 8.20 to 8.33 ms | 2.00 to 2.06 ms | 12.8 to 13.0 ms |
| | S7 (2 runs) | 8.20 to 8.33 ms | 1.93 to 1.97 ms | 14.8 to 15.0 ms |
| | S6 forced | 5.21 ms | 1.01 ms | 22.4 ms (12.0 ms frames) |
| grass (1,880 draws) | off (2 runs) | 6.25 to 6.64 ms | 3.84 to 4.24 ms | 12.7 to 15.0 ms |
| | S7 | 6.25 ms | 4.06 ms | 15.0 ms |

- S7 works as designed: in `return` 380 to 400 draws a frame on the thread, 290 to 310 on the render thread beside it,
  the render thread waiting 0.08 ms; no errors. **But no gain:** the render thread's draws cost it about 0.32 us each
  here (`[prof] draw phases`; 0.85 to 0.9 ms a frame for 2,630 draws), so moving 400 of them saves about 0.13 ms, which
  the hand-overs and the forwarded register writes take back. The render thread's time is not in its draws in these
  views: of 8.3 ms CPU, 3.0 ms are ops (register writes 1.2 ms, draws 0.9, swap 0.6).
- **One conflict** in one of the two `return` runs: the render thread changed a 960x540 surface (at 216FE000) the
  thread had used in that overlap; S7 switched itself off for the rest of the session, as designed (that frame may
  have been wrong).
- Steps 2 (the thread's per-draw cost) and 3 (the split's balance; its first form is above) were not taken further:
  with 0.1 ms of draw work to move they cannot gain.

S7 stays off by default (`WWHD_VK_SCENE_FRONT`); S6's switches are unchanged. Not tried in the headset.

## Why S6 was slow, and the fix: B's layout changes deferred (2026-10-31)

**A correction first.** The render-thread profiler's draw time (`[prof] draw phases`, "0.33 us/draw") times only the
draw *command*: most draws are held in same-state or index runs (`held_or_draw`) and drawn later, in `held_before` or
`index_run_flush`, outside the op's timer. That is where the 5 ms of render-thread CPU outside "ops" goes. The
conclusion above that "the render thread's draws cost it about 0.32 us" is wrong: a main-pass draw costs several us on
any thread (6.5 to 7.5 us on the scene threads, measured per phase now: `drawing threads' draw phases` in the scene
thread's log, every phase 20 to 40 times the render thread's sampled figure for this reason, not one hot spot).

**What made S6 slow.** In every frame the second drawing thread (B) waited for A1 once, 2.8 ms, and A then waited for B
2.3 ms: A1, B and A2 ran one after the other. B waits for A1 before changing anything A1 may still use. The changes it
made, found with a log of the surface and the layout it wanted (`waits for A1 before`):
1. reading a 480x270 two-eye target the scene draws into at draw 117 and samples with mip levels: B rebuilt its mip
   chain (`with_mip_chain`), a copy from the target (layout 2 -> transfer source);
2. reading other targets A1 had drawn (2 -> read), and drawing into targets A1 had sampled (read -> colour or depth
   attachment);
3. once in a while, after reading a surface, changing it again in the same span (a 960x540 at 216FE000, the surface
   S7's conflict was on).

B's commands always reach the GPU after A1's, so each change is in the right place there; only the CPU side (the
surface's tracked layout and use, which A1 may still read) made B wait.

**The fix** (only with `WWHD_VK_SCENE_TWO=1`; nothing changes otherwise):
- Before A1's end, B leaves out a layout change to read, colour attachment or depth attachment, the mip chain's
  rebuild (when the chain image exists at the size it keeps) and the copy from a newer format alias
  (`scene_thread::b_defer_layout`, `b_defer_chain`, `b_defer_adopt`; `Surface::bPreRead` marks the span). Its draws
  bind the surface as if the change were made; its pass reuse check reads the deferred layout (`b_layout`).
- At A1's end the scene's thread, once B is done, makes them (`b_commit_reads`): between A1's commands and B's, from
  the surfaces' final state after A1. That is also more exact than before, when B read the surfaces' state while A1
  was still recording.
- If B changes such a surface otherwise later in its span, it waits for A1's end (or undoes the draw, as before) and
  makes the deferred change first into a small stream emitted before its commands (`stereo_part_b_pre_stream`); its
  own change follows in its commands.
- A surface in neither eye's map (a scan, a temporary, a mip chain image) waits as before.
- Logged: `surfaces' reads made ready at A1's end` per frame.

**Pictures:** frame 4200 byte-identical in both eyes with S6 forced on and off, from three states: `return` (B drawing
about 420 draws a frame, no conflicts) `def8fb89850a89a6` / `8a41fa4e5c545454`; `busy` `099925164e039fc7` /
`dd164564183b9b85`; `start` `98722a577f2e80f1` / `19b8846e15d3f29f`.

**Measured** (desktop stereo, Capped 90, `WWHD_VK_SCENE_THREAD_MIN_DRAWS=0`, two runs each, gameplay medians):

| View | Setup | Frame | Render thread CPU | Spare (idle) | Longest frame | B waited / A waited |
|---|---|---|---|---|---|---|
| `return` (2,630 draws) | off | 11.12 ms | 8.20 to 8.27 ms | 1.90 to 2.00 ms | 13.1 to 15.3 ms | |
| | S6 before the fix | 12.00 ms | 5.21 ms | 1.01 ms | 22.4 ms | 2.78 / 2.35 ms |
| | S6 fixed | 11.12 ms | 5.21 to 5.40 ms | 1.52 ms | 15.3 to 15.9 ms | 0.35 / 0.10 ms |
| grass (1,880 draws) | off | 11.12 ms | 6.51 ms | 4.08 to 4.12 ms | 14.5 to 14.6 ms | |
| | S6 before the fix | 11.12 ms | 4.04 ms | 2.63 ms | 15.7 ms | |
| | S6 fixed | 11.12 ms | 4.17 to 4.30 ms | 3.76 to 3.80 ms | 15.2 to 15.7 ms | 0.31 / 0.09 ms |

S6 now holds 90 in the heaviest view (it did not) and its spare time rose by 0.5 to 1.2 ms, but it is still 0.3 to 0.5
ms short of off. Why:
- **A2 still waits for B.** B's commands go between A1's and A2's, so A2 starts only when B is done: after the part's
  start the scene takes max(A1, B) + A2, about 410 + 250 draws at 7.4 us. The render thread waits about 4 ms for it at
  the frame's last join. A1 and B are already even; only a third drawing thread or A2 overlapping B (the S7 model,
  harder here: the post-processing in A2 samples what B draws) would shorten it.
- **The threads draw more slowly than the render thread did.** Moving about 1,070 draws off the render thread saves it
  about 3 ms of CPU (about 2.8 us a draw, the hand-over included), while the threads take 7.4 us a draw (6.5 us when
  they ran one after the other). Not the L3 groups (`WWHD_VK_SCENE_THREAD_SAME_L3=1`: unchanged). Still to find: the
  render thread's fast paths the threads lose (continued draws, kept states reset at each span:
  `t_partEpochSalt`, `forget_continued`) and lock contention.

S6 stays off by default. Not tried in the headset.

## Why the drawing threads draw more slowly (2026-10-31)

Measured the same way on both sides (`WWHD_PROF_RENDER_PHASES=1`: the render thread's draws timed by phase as the
drawing threads' are; with `WWHD_VK_SCENE_SPLIT=1` only the part's draws), the same part draws in `return` cost:

| Where | A draw | Phase sum (sampled) |
|---|---|---|
| render thread, the split without threads | 4.07 us | 4.85 us |
| render thread, locks always taken (`WWHD_VK_LOCK_ALWAYS=1`) | 4.82 us | 5.44 us |
| drawing threads A and B, S6 | 7.47 us | 8.51 us |

Every phase is slower on the threads; textures (+0.9 us), the multiview setup and eye 1 lookups (+0.6 us each) and
vertex data (+0.4 us) most. What it is not: the continued-draw fast path (`WWHD_VK_STATS=1`, now labelled by thread:
the threads together continue about as many draws, about 1,000 a frame, as the render thread alone), nor eye 1 flushes
(3.8 a frame either way). What it is, from the thread sampler (`WWHD_SAMPLE_THREADS`, the build now with line tables:
`CMAKE_CXX_FLAGS=-gline-tables-only`, code unchanged) set against the work the render thread gave up, about 2.3 ms a
frame more on the threads:
- **Locks**, about 0.6 ms: the surface lock was taken 25,600 times a frame (about 10 a draw) and the buffer cache's
  8,300 (`WWHD_VK_LOCK_COUNT=1` counts them, the surface lock by call site), with 900 and 430 waits a frame
  (0.5 and 0.35 ms). Uncontended they cost the render thread 0.75 us a draw.
- **Copying**, about 0.4 ms: 12 whole-register-file loads a frame (256 KiB each, copied into the batch and again into the
  thread's machine, which also compared all 65,536 registers): the game's context loads, forwarded while a thread
  draws.
- **Descriptor sets**, about 0.2 ms: the render thread keeps its sets across submissions (99% reused); the threads
  wrote about 3,450 a frame anew (allocation and `vkUpdateDescriptorSets` in the driver).
- **Register writes replayed**, about 0.4 ms: 21,000 writes a frame forwarded and applied again on the threads (the
  design: each thread keeps its own machine).

Fixed, every one keeping the result exact:
- `upload_surface` returns without the surface lock where it would return at once under it (a GPU-drawn texture, one
  without an image, one whose check this frame has finished: `Surface::uploadCheckedFrame`, set as the check ends);
  `stereo_shared_texture` rules a texture out by its own state before the lock (and checks again under it);
  `stereo_eye0_texture` tests the eye before the lock. Surface locks: 25,600 to 17,000 a frame.
- Kept descriptor sets per scene drawing thread, in pools of its own (a pool it drops goes to the render thread to be
  destroyed): A reuses 1,800 a frame, B 1,550, none written.
- A context load reaches a drawing thread as the 256-register blocks restore may change (`load_blocks`: a few KiB);
  `ReplayMachine::load_regs` skips blocks of 16 registers that are the same. Whole-file loads: 12 to 1 a frame;
  `WWHD_VK_REG_BLOCKS_VERIFY=1` sends the whole file after each and compares: 6,600 checked a window, 0 registers
  differed.

Result: a thread draw 7.47 to about 7.2 us (`return`) and 7.4 to 7.1 us (grass); the frame's spare time moved within
the runs' noise (off itself measured 1.63 to 1.90 ms spare in `return` on different runs). The threads still draw
about 1.5 times as slowly as the render thread with locks: the rest is spread thin (no function over 0.1 ms a frame
beyond those above), the cost of three cores sharing the renderer's data. Pictures: frame 4200 identical with S6
forced on against off in `return`, `busy` and `start` (one `busy` run with S6 off came out at another game moment,
Link's pose and the clouds different; its rerun matched).

What is left is the shape, not the cost of a draw: A2 waits for B, so the render thread waits about 3.7 ms at the
frame's last join in `return`. With the threads at 1.5 times the render thread's cost a draw, splitting the scene in
two at best breaks even. S6 stayed off by default (until the next section).

## S6 with batched runs: faster than S6 off, on by default (2026-10-31)

**The cause of most of what was left:** S1 to S6 turned the batched same-state runs off for the whole frame
(`batch_can_hold`), and in these views those runs are about a third of the scene's draws (the main geometry run's
134-draw runs of one shader pair). Measured with S6 off, batching is worth about 1 ms a frame (`WWHD_VK_BATCH_RUNS=0`:
spare time 1.91 to 0.88 ms in `return`, 4.19 to 3.24 ms in grass). S6 had reached S6 off's spare time without it.

**The change:** runs are held with the scene's thread on (only the split without it, S1 to S3's diagnostics, still holds
none). `draw_stereo` offers a held run to the drawing threads first (`t_sceneRun`, through `draw_stereo_one`, so the
part's start, the atlas's emission and the draw numbering go as for any draw); `scene_thread::draw` hands it to A or B
whole (`kDrawRun`: each piece's 16 vertex-shader uniform block addresses as the job's words), and the thread draws it as
the render thread would: one instanced draw of the batched variant per chunk (`batch_accept`, then `draw_one` per chunk:
both eyes, B's undoable draw), or piece by piece with each piece's addresses (`Worker::draw_run`, as `batch_draw_run`
on the atlas's thread). A run that cannot be handed over (no part, or the part must end first) has nothing drawn of it
there, and the render thread batches it as before. A run counts as its pieces in the draw numbering (B's span).

Also in this round: a CacheLock does not lock a mutex its thread already holds through one (surface locks 17,000 to
8,400 a frame; the cost per draw did not move: what is left is contention). `WWHD_VK_SCENE_TAIL=1` (off) ends the part
where B's span ends and draws the rest on the render thread: within noise.

**Measured** (desktop stereo, Capped 90, `WWHD_VK_SCENE_THREAD_MIN_DRAWS=0`, three runs each, gameplay medians):

| Setup | `return` spare | grass spare | Longest frame (median) |
|---|---|---|---|
| S6 off | 1.85 to 1.94 ms | 4.13 to 4.25 ms | 12.4 to 12.6 ms |
| S6 off, no batching | 0.88 ms | 3.24 ms | 12.5 to 12.9 ms |
| S6, no batching (before) | 1.88 to 1.94 ms | 4.14 to 4.33 ms | 12.5 to 13.4 ms |
| S6 with batched runs | 2.76 to 2.97 ms | 4.58 to 4.79 ms | 12.9 to 13.2 ms |

In `return` 12 runs a frame go to the threads, drawn as 12 instanced draws (556 draws); in grass 6 (332). Pictures:
frame 4200 identical to S6 off in both eyes in `return`, `busy` and `start`.

**In the headset** (PS VR2, Capped 90 at 90 Hz, the default build: S6 on at its usual threshold of 1,400 draws):

| Run | Average | At or above 90 | Heavy views (2,300+ draws) |
|---|---|---|---|
| S6 off | 88.4 fps | 76 % | 85.4 fps, 40 % at 90 |
| S6, no batching | 82.6 fps | 59 % | 73.8 fps, 19 % at 90 |
| S6 with batched runs | 88.9 fps | 90 % | 89.0 fps, 89 % at 90 |

(The last without its loading window and the windows after SteamVR ended the session.) A few single long frames remain
(5 over 25 ms). **S6 is now on by default** (`kSceneThread`, `kSceneTwo`: `env_default_on`; `WWHD_VK_SCENE_THREAD=0` or
`WWHD_VK_SCENE_TWO=0` turns it off; off on Android unless =1), at its threshold of 1,400 scene draws a frame.

## S6: the split balanced (2026-11-03)

**Found** in a headset run in Forest Haven (PS VR2, Capped 90; views of 4,000 to 4,400 draws a frame, 1,885 of them on
the sun atlas's thread): 60 to 79 fps there, "in-between pass 13.2 ms" against its 11.1 ms. The GPU was busy 31 to 38 %
of the time; the render thread's CPU was 6.3 to 7.3 ms a frame. What was over is the render thread's wait at the part's
flush join, 4.2 to 4.7 ms a frame: the drawing threads draw at 9.0 to 9.8 us a draw (the render thread about 5), and A2
starts only after B. At A1's end A waited for B 0.5 to 1.6 ms a frame in every heavy window: B's span, from the run's
middle, held more of the work than A1, which starts at the part's start and not at the run's.

**The change:** B's span starts at `g_bStart` of last frame's run instead of its middle: half to start, then later by
0.02 after a span whose A waited for B more than 0.1 ms, earlier by 0.01 otherwise, within 0.3 to 0.8 (as S7's share).
`WWHD_VK_SCENE_TWO_START` fixes it. Logged in the second drawing thread's line ("its span from N% of it"). Only which
thread records which draws moves; the conflict rules are S6's, so the pictures are unchanged. Expected: about half of
A's wait off the frame (0.3 to 0.8 ms). Not enough alone for the heaviest views; next to try there:
`WWHD_VK_SCENE_TAIL=1` (the render thread, idle at that join, draws the part's rest itself at about half the cost a
draw). Not measured yet.

**In the headset** (PS VR2, Capped 90, Forest Haven, gameplay windows of 2,000+ draws, by draws a frame):

| Run | 3,000 to 3,999 | 4,000+ | All |
|---|---|---|---|
| Before (build c50d8ff) | 86.9 fps, 54 % at 89.5+ | 78.8, 23 % | 86.2, 50 % |
| Balanced by A's wait only, and the tail always | 76.6, 12 % | 87.1, 64 % | 80.9, 33 % |
| Balanced by both waits, no tail | 87.5, 60 % | 84.0, 38 % | 86.8, 55 % |

- By A's wait alone the start went to 80 %, and B then waited for A1 0.8 to 1.7 ms a frame (a surface A1 still
  changed). Now B's waits for A1 in the span (`Split::spanBWaitNs`) move it earlier when they exceed A's by 0.1 ms:
  both waits then 0.1 to 0.2 ms, the start between 30 and 67 %.
- The tail always on: the 4,000-draw views gained, but the 3,000-draw ones lost (the render thread's own draws rose
  from 0.2 to 1.0 to 1.4 ms a frame where the threads had kept up).

## S6: the automatic tail (2026-11-03)

The tail is now chosen per frame (`kSceneTailMode`, `WWHD_VK_SCENE_TAIL` unset): on in a frame after one whose render
thread waited at the joins more than 2.5 ms (the threads behind: it sat idle while A drew the rest at twice its cost a
draw), and kept while its wait at the span's end stays over 1 ms (the threads still behind there). `=1` always, `=0`
never. Logged: `the tail on the render thread (automatic): in N% of the frames with the scene's thread`. Which thread
records which draws only; the pictures are unchanged.

**In the headset it lost** (same setup as above): 3,000 to 3,999 draws 83.3 fps (21 % at 89.5+), 4,000+ 83.7 (20 %),
all 85.0 (34 %), against 86.8 (55 %) with the tail off. It was on in 91 to 99 % of the busy frames: the render thread
reaches the span's end before the threads in nearly every busy frame, so its wait there is always over 1 ms and says
nothing about whether A would have drawn the rest in time. **Now off by default**; `WWHD_VK_SCENE_TAIL=auto` keeps
the rule for later tries, `=1` always. The default is the balanced split without the tail. What is left in the
heaviest views (the render thread waiting about 4 ms at the flush join) is the drawing threads' cost a draw (9 to 10
us against the render thread's 5): the next work there.

## S6: the tail by measured cost (2026-11-03)

Two changes to `WWHD_VK_SCENE_TAIL=auto` (still off by default):
- **The tail is the run's last piece only.** The render thread draws from the span's end to the next join site (where
  the part would have been joined without it); parts begin again after it (`scene_tail_flush`). It drew the rest of
  the frame before, which cost the 3,000-draw views (the render thread's draws 0.2 to 1.0 to 1.4 ms a frame).
- **Chosen by cost** (`TailRule`). Both ways are timed from the span's end to the render thread being past the part:
  - without the tail, measured: to the join's end; with it, estimated: until A1 and B were both done (A's time stamp
    at A1's end, `g_sceneBothDoneNs`) plus the rest's draws at the render thread's own cost a draw;
  - with it, measured: to the next join site; without it, estimated: the larger of the rest's draws at the hand-over
    cost and the wait at the span's end plus the rest at A's cost a draw (A2 after A1 and B).
  - The costs a draw are running averages of the measured ones (starting at 1.5, 9.5 and 5 us). The way changes when
    the other is the cheaper by 0.3 ms three frames in a row.
- Logged with the tail's line: frames each way, their measured and the other way's estimated costs, the costs a draw.
Which thread records which draws only; the pictures are unchanged.

**In the headset it lost too:** 3,000 to 3,999 draws 82.8 fps (24 % at 89.5+), 4,000+ 83.8 (20 %), all 84.7 (42 %),
against 86.8 (55 %) with the tail off. The rule took the tail in nearly every busy frame: by its own measure (span's end
to past the part) the tail was cheaper, 3.5 against 3.8 to 4.7 ms, yet the frame rate fell. The measure misses what
the tail costs the rest of the frame (the costs a draw it found, 13 to 27 us, are mostly other work in that stretch,
not draws). Every tail tried (always, automatic by waits, by cost) cost the 3,000-draw views 4 to 10 fps; the tail
stays off by default and is not taken further. (Its log line's share of frames reads 0 %: the tail's flag is cleared at
its join site before the frame's end counts it.)

## The helper threads' cost a draw (2026-11-03)

The scene's drawing threads drew at 9.6 to 11 us a draw in Forest Haven in the headset, the render thread at about 5.
**Measured** (a headset run with `WWHD_PROF_RENDER_PHASES=1 WWHD_VK_LOCK_COUNT=1 WWHD_VK_STATS=1`):
- Every phase is slower, none stands out: eye 1 lookups 1.9 to 2.1, vertex 1.4 to 1.6, shader 1.3 to 1.4, textures
  1.2 to 1.3, uniforms 1.1 to 1.2 us.
- The continued-draw fast path is not it: A continues 71 to 73 % of its draws, B 93 %, the render thread 61 %; a span's
  start costs A 0.6 % (no state). Option 3 (keeping it across hand-overs) has nothing to gain, and was not built.
- Locks a frame (all threads): surfaces 11,300 taken (900 waits, 0.75 ms), buffers 6,000 (350, 0.35 ms). By site:
  `find_or_create_surface` 3,230, `adopt_newer_alias` 2,880, `sampled_texture` 1,740, `stereo_shared_texture` 1,410,
  `stereo_eye0_texture` 1,260, `upload_surface` 610.
- The CPU (Ryzen 9 3900X) has four L3 groups of three cores; data shared across groups is read several times slower.

**Tried** (desktop stereo, `return` at 3x, `WWHD_VK_SCENE_THREAD_MIN_DRAWS=0`, two runs each where it mattered):

| Change | Threads' cost a draw | Render thread spare | Kept |
|---|---|---|---|
| Before | 7.61, 7.62 us | 2.91, 3.02 ms | |
| 1. Threads on the core groups (`WWHD_VK_PLACE_THREADS=1`) | 6.91, 7.09 us | 3.16, 3.25 ms | switch, off by default |
| 2. `adopt_newer_alias` without the lock when it has nothing to adopt | 7.66 us (no change) | 2.96 ms | yes (exact) |
| 4. Kept descriptor sets dropped only for buffers they name | 7.85 us (noise: no drops here) | 2.63 ms | yes (exact) |
| 1, 2 and 4 together | 7.05 us | 3.10 ms | |

- **1** (`platform/core_groups.h`): the render thread and the scene's drawing threads on one logical processor of each
  core of the last L3 group; the atlas's thread, the record worker, the end-frame thread and the game's threads on the
  other groups. The threads' every phase got cheaper (7 to 9 % in all), the lock waits fewer. Windows only.
- **2**: `formatViews` is only ever set, so a surface without it has nothing to adopt; 2,880 locks a frame off. No
  change in the cost a draw: the lock taking alone is not what makes the threads slow. Deeper per-thread caches for
  `find_or_create_surface` and the buffer cache were not built: their answers depend on state other threads change
  (write sequence, eviction, the write watch), and removing a quarter of the surface locks moved nothing.
  `WWHD_VK_ADOPT_LOCKED=1` restores the lock (comparisons).
- **4**: in the headset A wrote 100 to 390 descriptor sets a frame anew (B 15 to 90): every thread's kept sets were
  dropped when any buffer or image was destroyed, every 4 to 20 frames. Buffers destroyed without images (155 of 160
  drops logged: staging buffers, never in a set) now drop a thread's kept sets only if one names such a buffer
  (`destroyed_buffers_since`, before the buffers are destroyed, so a handle made again is never matched).
  `WWHD_VK_DESCRIPTOR_BUFFER_DROPS=0` restores the old drops; `WWHD_VK_DESCRIPTOR_DROPS=1` logs each drop's site. The
  desktop views drop none, so only the headset shows it.
- **Pictures:** frame 4200 byte-identical with all three against none, from `return` (left eye `af248b83c83f14c6`; that
  frame has no right-eye picture) and `fortress` (`3314ce8453504608` / `4e0be041d2e564d9`).
- The desktop cannot make the headset's 4,000-draw views: Forest Haven draws 2,520 a frame there, and a 32:9 picture
  draws no more (the game's culling; in VR the wider projection widens it). `return` at 3x (the headset's pixels an eye)
  stays the desktop's heaviest view.

**In the headset** (PS VR2, Capped 90, Forest Haven, gameplay windows of 2,000+ draws, by draws a frame):

| Run | 2,000 to 2,999 | 3,000 to 3,999 | 4,000+ | All | Helpers a draw |
|---|---|---|---|---|---|
| Before (c50d8ff) | 89.4 fps, 61 % at 89.5+ | 86.9, 54 % | 78.8, 23 % | 86.2, 50 % | 9.0 to 9.8 us |
| Balanced split (31b8ff5) | 89.5, 67 % | 87.5, 60 % | 84.0, 38 % | 86.8, 55 % | 9.0 to 10 us |
| 1, 2 and 4 | 89.7, 94 % | 89.2, 75 % | 89.5, 82 % | 89.5, 83 % | 7.4 to 8.5 us |
| 1 and 4 (2 off: `WWHD_VK_ADOPT_LOCKED=1`) | 89.7, 86 % | 88.4, 50 % | 89.9, 100 % (7 windows) | 89.3, 76 % | 7.74 us |

- A wrote 0.1 to 0.7 descriptor sets a frame anew (100 to 390 before), no drops: 4 works in the headset.
- 2 makes no difference measured (7.74 against 7.76 us a draw; the frame rates within the runs' spread); kept, exact.
- **The placement is on by default** (`WWHD_VK_PLACE_THREADS=0` turns it off). Windows only; on a CPU with one L3 for
  all its cores, or under three cores in a group, it places nothing.

## A standalone headset from 700 scene draws (2026-11-05)

A Quest 3 player's logs: the scene's thread in 11 to 64 % of the frames at the threshold of 1,400 scene draws, the
render thread over its time (17 to 25 ms a frame at 72 Hz) in every window. Asked for: use it more often. On a
standalone headset (`quest_theater::standalone_headset`) the threshold is now 700 (`scene_thread_min_draws`;
`WWHD_VK_SCENE_THREAD_MIN_DRAWS` still wins). Which thread records which draws only; the pictures are unchanged.
Expected gain small: where it ran, its draws took 6.68 ms a frame and the render thread waited 6.00 ms of it at a
join (18 us a draw there). The next Quest log's `[vulkan scene thread] used in N% of frames` and its joins' waits
show what it changed. Not tried in the headset.

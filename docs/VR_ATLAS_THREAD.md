# Recording the sun atlas on a thread of its own

The plan for roadmap item #1 in [VR_PERFORMANCE.md](VR_PERFORMANCE.md). The stereo frame is limited by the render
thread's CPU. In the desktop stereo mode, without the sun atlas's work, it reached three frames per step (the rate 90
fps needs in the headset). The atlas's draws are to be prepared and recorded on a second thread while the render
thread goes on with the scene, drawing exactly the same shadows.

Every stage leaves the game running as before. Each is checked with:
- byte-identical pictures in both eyes (fixed seeds, interpolation off, `frame_<n>.png` and `frame_<n>_right.png`);
- the full test suite;
- a timing run in the desktop stereo mode.

## The atlas's block (stage 1)

`WWHD_VK_ATLAS_STREAM=1` holds the atlas's commands in a stream of their own on the render thread (`stereo.cpp`
`stereo_atlas_hold`, `stereo_atlas_emit`). It showed:
- The atlas's draws are the frame's first 3,029 draws.
- The scene's draws, about 2,600, all come after them.
- Inside the block there are 2 clears or copies and the game's mid-frame `GX2Flush`, a submission. So the block runs
  as up to 3 parts, or the second thread takes the clears and the submission point too.

## What an atlas draw touches (stage 2.1)

Two ways to find it:
- **The code:** what `draw()` and its helpers call.
- **A run:** `WWHD_VK_THREAD_CHECK=1` (`thread_check.h`). Each part below notes the thread that uses it. The first
  thread owns the part; a use from any other thread is logged once, with both threads.

In the desktop stereo mode, with the record worker and the atlas's held stream on, every part was owned by the render
thread. The one other use was the background save of the pipeline cache. It reads the pipeline map from another
thread, but only under the GX2 execution lock, which the render thread holds while it works, so the two never run at
once.

**The rule that keeps it so:** the second thread runs only inside the render thread's locked section (it is started
and joined while the render thread holds the GX2 execution lock).

| Part | Entry points (checked) | Shared state | Today | Stage |
|---|---|---|---|---|
| Draw preparation | `draw` | The GX2 register file the draw reads (`r`), the dirty register groups (`gx2::take_dirty_groups`), the continued fast path (`continued`, `continuedEyes`), draw batching (`drawBatchState`), about 76 function-local statics, the statistics in `R` (`R.drawCount`, `R.cpuPreparation`, `R.vertex*`, `R.descriptor*`, `R.pipelineCreates`) | render thread | 2.2 (scratch, counters), 2.9 (registers, fast path) |
| Uploads | `snapshot` (also `vertex_snapshot`, `vertex_window_snapshot`, index uploads) | the upload arena of the active submission (`R.submissions[R.activeSubmission]`) | render thread | 2.3 |
| Shaders | `vk::translate`, `vk::get_fetch_shader` | the shader and fetch-shader caches, `g_shader_state_gen` | render thread | 2.4 |
| Pipelines | `pipeline`, `resolve_pending_pipeline` | the pipeline map (`pipelines`), `lastPipelineLookup`, `pipelineLookaside`, `samplerMemo`, the pipeline-cache dirtiness | render thread; the background cache save under the GX2 lock | 2.4 |
| Descriptors | `stage_descriptor_set` | `R.descriptorPool`, the descriptor set cache and the last-set memo per stage | render thread | 2.5 |
| Render passes | `begin_pass` | `R.rendering`, `R.pass*`, the submission's render scope, the framebuffer and pass caches (`pass_caches()`) | render thread; the atlas's own pass since stage 1 | 2.5 |
| Surfaces | `find_or_create_surface`, `upload_surface` | the eye's surface map (`R.surfaces`, swapped per eye), the invalidation index, `g_surface_epoch`, texture uploads | render thread | 2.6 |
| Image layouts | `transition_image` | each `Surface`'s layout and its barriers, recorded into the stream that is attached | render thread | 2.7 |
| Buffer cache | `cached_guest_range` (also `buffer_cache().lookup`) | the guest buffer cache and its write tracking | render thread | 2.8 |

**Not checked yet** (no single entry point):
- the command streams themselves: `record::` routes by `eye_deferred_commands`, a global the second thread must have
  its own value of;
- `stereoEye` and the eye surface-map swap. The atlas belongs to eye 0, and its draws must not see an eye switch made
  by the render thread meanwhile;
- the shader stage state handed to eye 1 (`stereoShadersOut`).

Stages 2.2 to 2.9 each make one row safe. Stage 3 runs the block on the second thread.

## Stage 2.2 done: scratch space and counters per thread

**Counters.** The draw path's statistics are one struct, `DrawCounters` (`backend.h`), which used to be 24 separate
fields of `Renderer`. It is `R.drawCounters`:
- the draw path counts through `draw_counters()`, the renderer's own on the render thread;
- a second thread gets its own with `set_thread_draw_counters`, added in with `DrawCounters::add` when it joins;
- counting stays a plain increment, not an atomic one.

**Statics.** In `draw.cpp`, every function-local static used only for one call or for diagnostics is `thread_local`:
- the index copy buffer;
- the per-frame memo of resolved targets;
- the "seen once" log sets and the periodic reports' frame markers;
- the diagnostic statistics (`vertexWindowStats`, `feedbackStats`, `continuedStats`).

On one thread they behave as before.

**Left for later stages:** the per-stream caches (vertex and uniform snapshot history: 2.3; the sampler cache: 2.4;
descriptors and bind state: 2.5). Draw batching also stays as it is, because it submits, and a second thread never
submits.

**Checked:**
- frame 4200 byte-identical in both eyes, with and without the atlas's held stream; frame 3600 matched its known
  variant;
- frame time unchanged (17.4 ms, 2.5 us a draw);
- 608 of 608 tests.

## Stage 2.3 done: the upload arena per thread

**A second upload arena.** `R.atlasUploadBlocks` has the first arena's lifecycle:
- moved into the submission's slot at submit (`slot.atlasUploadBlocks`);
- reset when that slot's fence completes;
- handed back when the slot is reused.

`allocate_upload` allocates from this thread's arena (`upload_blocks()`; `set_thread_upload_blocks` picks another).
While the atlas's held stream holds, the render thread allocates from the atlas's arena, so the second arena's
lifecycle is exercised now. Both arenas' slices belong to the same submission, so either stream may use the other's.
The upload counters (`uploadAllocations`, `uploadBytes`) moved into `DrawCounters`.

**The reuse caches of upload slices are per thread:** the vertex snapshot history, the vertex window entries and the
uniform reuse cache. Each thread reuses only slices of the arena it allocates from.

**Memory:** the atlas's arena grows to what one submission's atlas draws need, at least one 32 MiB block (as the first
arena's), and only while `WWHD_VK_ATLAS_STREAM=1`.

**Checked:**
- frame 4200 byte-identical in both eyes, with and without the atlas's held stream; frame 3600 matched its known
  variants;
- frame time unchanged;
- 0 new upload blocks per window: neither arena grows once settled;
- 608 of 608 tests.

**For stage 3:** the profiler's upload count (`rprof::add_upload`) and `rprof::g_upload_kind` are render-thread
globals. Off the render thread they must be counted per thread, or skipped.

## Stage 2.4 done: the shader and pipeline caches locked

`draw_threads.h`: a `CacheLock` takes its mutex only while a second thread prepares draws
(`second_thread_active()`, set by the render thread before that thread starts and cleared after it joins). Until
then a lock is one predictable branch. `WWHD_VK_LOCK_ALWAYS=1` takes them on one thread too, for tests. The mutexes
are recursive, and where two are held at once the order is pipelines before shaders.

**What is locked:**
- **Shaders:** `vk::translate` and `vk::get_fetch_shader`, one lock for the subsystem. Shaders are owned by the map's
  `unique_ptr`s, so a returned pointer stays valid. The shader binaries need no lock: they are already shared with the
  pipeline compile workers.
- **Pipelines:** the map lookup and insert in `pipeline`, and `resolve_pending_pipeline`. The map is an
  `unordered_map`, whose elements stay in place when it grows, so a reference found under the lock stays valid.
- **Samplers:** the sampler cache's lookup and insert.

**Per thread:** the last-lookup memo, the lookaside and the sampler memo. `reset_pipeline_lookup_cache` advances a
generation, and every thread drops memos made before it at its next lookup. Otherwise a second thread's memo could
point into a cleared map.

**Checked:**
- pictures byte-identical in both eyes, with defaults and with the locks forced on (and the atlas's held stream);
- no deadlock in a full run with the locks forced on;
- their cost when taken on every call: within noise (17.4 against 17.3 ms; the shader phase 1.62 against 1.60 to
  1.64 ms);
- 608 of 608 tests.

**For stage 3:** while a shader or pipeline is still compiling, the exact path presents completed frames as it waits
(`shader_binary_words_exact`, `resolve_pending_pipeline` with `present`). The second thread must never present: such
a wait goes back to the render thread.

## Stage 2.5 done: descriptors and bind state per thread

**A second descriptor pool per submission slot** (`slot.atlasDescriptorPool`, `R.atlasDescriptorPool`), because a
Vulkan pool may be used by one thread at a time. It has the first pool's lifecycle: created with the slot, reset when
its fence completes, made active with the slot, destroyed with an unsubmitted slot. It is the same size, and made only
while `WWHD_VK_ATLAS_STREAM=1`.

`stage_descriptor_set` allocates from this thread's pool (`descriptor_pool()`; `set_thread_descriptor_pool`). While
the atlas's held stream holds, it allocates from the atlas's pool. The log confirms it once: `[vulkan stereo] sun
atlas held stream: its own upload arena, its own descriptor pool`.

**Per thread:** the descriptor set caches (`descriptorCache`, `lastDescriptors`, keyed by submission) and the
bound-state cache (`DrawStateCache` per eye). Within one stream the bound-state cache is right, because:
- it is cleared at every new render pass;
- every switch between streams ends their passes (`stereo_atlas_hold`, `stereo_atlas_emit`, `stereo_flush`).

**Checked:**
- pictures byte-identical in both eyes, with defaults and with the atlas's held stream and the locks forced on;
- frame time unchanged (17.3 to 17.45 ms; descriptors 1.0 ms);
- 608 of 608 tests.

**For stage 2.6:** a texture resample in `surfaces.cpp` allocates its descriptor set from `R.descriptorPool`
directly. Off the render thread it needs `descriptor_pool()`.

**For stage 3:** with two threads recording at once, the render thread's scene commands must not reach the command
buffer before the atlas's. So the render thread's own stream is held while the atlas's is recorded, or the atlas's is
put in place ahead of it.

## What a frame looks like (`WWHD_VK_ATLAS_TIMELINE=1`)

One frame's events by draw number, the same at TV frames 2400 and 3000:

| Draws | What happens |
|---|---|
| 1 to 1,010 | the atlas's first cascade |
| 1,010 | a clear or copy |
| 1,011 to 2,020 | the second cascade |
| 2,020 | a clear or copy |
| 2,021 to 2,048 | the start of the third |
| 2,048 | a submission (the game's `GX2Flush`) |
| 2,049 to 3,030 | the rest of the third |
| 3,031 | the scene starts |
| 4,101 | the scene first makes the atlas ready for sampling |

The atlas's draws bind 15 to 16 textures 2,670 times a frame, and the scene binds every one of them too (out of about
185 it binds).

**What follows for stage 3:**
- **The overlap window** is the scene's first 1,070 or so draws, up to its first use of the atlas: about 3.3 ms of the
  render thread's work against the atlas's 3.7 ms.
- **The mid-block `GX2Flush`:** if the threads had to join there, two thirds of the atlas would have nothing to
  overlap. So while the second thread runs, the flush waits until the threads join. That is safe: the flush itself
  waits for nothing, and any real wait (`GX2DrawDone`) or submission joins first.
- **The shared textures** are settled almost always (uploaded, and in the layout for sampling), so neither thread
  changes them. The rules for the rare frame they are not are in stage 2.6b.

## Stage 2.6a done: the selected eye per thread

The second thread draws the atlas as eye 0 while the render thread switches eyes at every scene draw. So:
- **`stereoEye` is `thread_local`.**
- **The two eyes' surface maps no longer swap.** `R.surfaces` is always eye 0's; the other map, in `stereo.cpp`, is
  always eye 1's.
  - `surfaces_of(eye)` gives an eye's map, and `selected_surfaces()` the calling thread's selected eye's.
  - Every former use of `R.surfaces` means one of those, and was rewritten by its meaning.
  - The linear targets and the main depth address are the same (`selected_linear_targets`, `selected_main_depth`).
- **The render pass state is one struct,** `PassState`, which used to be 8 `Renderer` fields. `R.pass` is the render
  thread's current pass; `swap_pass` swaps it whole with the other eye's and the atlas's. `pass_state()` gives the
  calling thread's (`set_thread_pass_state` for another thread's own).

**Checked:**
- the renderer's smoke test on the device (`--renderer-smoke`: eye switches, stereo off and on): PASS;
- stereo frame 4200 byte-identical in both eyes, with and without the held stream and forced locks; frame 3600
  matched its known variants;
- mono frame 4200 byte-identical to the previous commit's;
- frame time unchanged (17.3 ms);
- 608 of 608 tests.

## Stage 2.6b done: the surface maps locked, and the overlap's rules for surfaces

13 of the frame's 183 `GX2Invalidate`s come inside the atlas's block, so joining at each would leave nothing to
overlap. The render thread keeps applying them as it meets them. The atlas's thread may then see one earlier than it
would in order, which costs at most a repeated upload of the same guest data. That holds because the game never
rewrites memory that queued GPU work still reads before its next `GX2DrawDone`, the same rule that lets a real GPU
read late, and every `GX2DrawDone` joins the threads first.

**Locked** (`draw_threads::surface_mutex`, a `CacheLock` taken only while a second thread runs):
- the surface maps' lookups, inserts and walks: `find_or_create_surface`, `sampled_texture`, `with_mip_chain`,
  `adopt_newer_alias`;
- uploads and destruction: `upload_surface`, `destroy_surface_image`;
- invalidation: `invalidate`, `invalidate_surface_cache`, `stereo_invalidate`;
- copies and the reset: `copy_surface_impl`, `ss_reset_surfaces`;
- the stereo sharing checks: `stereo_shared_texture`, `stereo_eye0_texture`;
- dynamic resolution and `stereo_latch`, at the frame's end.

**The overlap's rules for surfaces** (`atlas_overlap.h`): each `Surface` has `atlasUse` and `sceneChange` marks.
- **Hooks where a surface's state changes:** an upload that really happens, a layout that really changes, its contents
  as a draw target, its image going.
  - On the atlas's thread, the hook marks the surface as used.
  - On the render thread, it joins first for the atlas, a texture the guest uploads, or anything the atlas's thread
    used; anything else it marks as changed.
- **The atlas's thread marks every texture it binds.** Finding one the render thread changed in this overlap switches
  overlaps off for the session, with a log line.
- **Until stage 3 starts an overlap,** every hook returns at once.

**Checked:**
- the renderer's smoke test: PASS;
- frame 4200 byte-identical in both eyes (with defaults, and with the locks forced on and the held stream) and in mono;
- frame 3600: three more runs with defaults gave the known variant. One run gave a third, which differs from it in
  7 pixels by 1 step. For scale, the two known variants differ in about 10,000 pixels, in a band across the picture
  (the scene's own timing).
- 608 of 608 tests.

**The forced locks' cost on one thread:** about 1.3 ms of render-thread CPU a frame (textures 1.86 against 1.25 ms,
passes 1.7 against 1.3 ms). The surface lock is taken thousands of times a frame. It costs nothing until stage 3, but
then it comes out of the overlap's gain, so stage 3.2 makes it cheaper.

## Stage 2.7 done: commands and passes on the right thread

The layouts' rules went in with 2.6b (`transition_image` calls `overlap::before_change` when a layout really changes).
This stage makes sure each thread's commands land in its own stream:
- **`eye_deferred_commands` is `thread_local`** (`deferred_commands.h`). `dispatch_recorder` reads it per thread, so
  everything the atlas's thread records goes to its own held stream, whatever stream the render thread attaches
  meanwhile. That includes its barriers (`transition_image`) and its pass ends (`end_encoder`).
- **The GPU pass-timestamp scopes** are skipped on the atlas's thread (`gpu_begin_render_scope`, `gpu_count_render_draw`,
  `end_encoder`). They live in the active submission slot, and they are a diagnostic.
- **The render-pass path's caches** (`begin_render_pass`, `end_render_pass`, `forget_framebuffers`, for devices without
  dynamic rendering) are locked (`draw_threads::pass_mutex`).

**Checked:**
- the smoke test: PASS;
- frame 4200 byte-identical in both eyes on both rendering paths (dynamic rendering; render passes via
  `WWHD_VK_FORCE_RENDERPASS=1`), with and without the locks forced on and the held stream, and in mono;
- 608 of 608 tests.

One run with defaults gave a different frame 4200 in both eyes (31 % of pixels). It is the same scene, rendered
correctly, at another moment: the clouds, wind lines and Link's idle pose had moved on. So the game had run to another
point by that TV frame. Three repeats gave the usual frame, and the locked and render-pass runs matched it.

## Stage 2.8 done: the buffer cache locked

When entries go: only once a frame (`end_frame`, at the swap, where the threads have joined) or on a full reset. The
map's elements stay in place when it grows, so an entry pointer stays valid through a frame. What does need a lock is a
lookup that misses and its upload, as one step: the same index buffer can be drawn into the atlas and into the scene,
and another thread must never find an entry that holds no data yet. `draw_threads::buffer_mutex`:
- inside `cached_guest_range` and `cached_native_indices` (lookup and upload together);
- around `cached_index_extent`'s memo in the shared entry;
- in `draw()`, from the converted indices' lookup through their upload or check;
- `buffer_cache_end_frame`, `buffer_cache_invalidate_all`, `buffer_cache_free`.

**Checked:**
- the smoke test: PASS;
- frame 4200 byte-identical in both eyes, with defaults, with the locks forced on and the held stream, and with the
  cache's verify mode on as well (`WWHD_VK_BUFFER_CACHE_VERIFY=1`: every hit compared with guest memory, 0
  mismatches);
- mono byte-identical;
- 608 of 608 tests.

## Stage 2.9 done: a second register machine

The renderer's draws read the register file and three counters derived from its writes:
- the shader-state generation (shader lookups are reused while it is unchanged);
- the full-state version and the dirty register groups (the continued-draw fast path).

A second thread replaying the atlas's block needs its own of all four, decided exactly as the render thread decides
them. Bumping everything on every write would be correct, but would lose the fast paths and memos the atlas's draws
use, and make them slower than the window they have.

**`gx2/replay_machine.h`:**
- **`ReplayMachine`** has its own register file (0x10000 words) and counters. `apply` applies a register write with
  `apply_regs`'s classification, from the same read-only tables, without the context shadow or the profiler.
  `load_regs` and `note_full_state_change` are a context load's two parts, `load` a full resync.
- **The render thread feeds a replay machine set as its observer,** from inside `apply_regs`, with the values actually
  applied (`OP_SET_PROJ_REGS` adjusts its payload). Also from `note_full_state_change` (`OP_LAYOUT_CONTENT` bumps the
  version without a write), from `set_context` and from a save-state load.
- **The draw path reads the calling thread's counters** (`gx2::shader_state_gen()`, `gx2::full_state_version()`,
  `gx2::take_thread_dirty_groups()`): the GX2 machine's own, unless a replay machine is attached.
- **Per thread:** the continued fast path's state (`continuedEyes`, dropped with the pipeline memos' generation), and
  the shader subsystem's last-lookup memo and state memo.
- **The profiler's per-draw state is per thread** (`g_draw_sampled`, `g_mark`, `g_upload_kind`), and a thread can opt
  out of the frame's upload and guest-read counts (`rprof::set_thread_uncounted`). These ran always, profiling or not,
  and would have raced.

**`WWHD_GX2_REPLAY_VERIFY=1`:** the render thread feeds a verifier with every register write. It checks after each
one that both machines hold the same registers and bumped the generation, version and dirty groups alike. Result:
- 0 differences in 148 million writes (stereo);
- 0 differences in 144 million writes (mono).

(A first version of the check compared groups that the render thread's draws clear but the verifier's never are; that
was the check's mistake, not the machine's.)

**Checked:**
- the smoke test: PASS;
- frame 4200 byte-identical in both eyes, with and without the verifier, and in mono;
- the profiler's upload counts unchanged (11.1 MiB a frame);
- 608 of 608 tests.

One default run's right eye differed in 15 pixels by 1 step, in a 50 by 60 patch, like frame 3600's 7-pixel variant.
Its left eye matched, and two repeats matched in both eyes.


## Stage 3.1 done: the atlas's draws on a second thread, one at a time

`WWHD_VK_ATLAS_THREAD=1` (with `WWHD_VK_ATLAS_STREAM=1`; off by default) records the sun atlas's draws on a thread of
their own (`gfx/vulkan/atlas_thread.h`/`.cpp`).

**How it works:**
- **At the block's first draw** (`stereo_atlas_hold`) an overlap starts. The renderer's open pass ends, the thread's
  replay machine is loaded from the render thread's registers, the locks turn on (`second_thread_active`), and the
  overlap gets its id.
- **Every register change the render thread makes is forwarded** (`gx2::set_register_forward`) and queued, in order
  with the draws: writes, context loads, full-state changes and save-state resyncs.
- **Each atlas draw is queued and recorded there** into the atlas's held stream. The thread uses its own pass state,
  upload arena, descriptor pool, counters and register machine; it never submits (no draw batching) and never presents
  while waiting for a shader.
- **At the stream's emit points** (`stereo_atlas_emit`) the render thread waits for the thread to finish: its pass
  ended, its counters added to the renderer's. It then emits the stream as stage 1 does.
- **A rare atlas draw that also writes colour** (not view-independent) is finished there for eye 0. The stream is
  emitted, and eye 1 is drawn on the render thread, whose registers are the same.
- **Errors** thrown on the thread are rethrown on the render thread at its next wait.

**Stage 3.1 waits for each draw:** the same work in the same order, on two threads, with no overlap yet. It proves
that the atlas's draws record correctly from another thread. It is slower: each of the about 3,000 draws costs a
thread hand-off, and the locks are on throughout.

| desktop stereo, 120 Hz proxy | frame | render thread CPU |
|---|---|---|
| atlas stream only | 17.4 ms | 14.4 ms |
| atlas thread, one draw at a time | 38.1 ms | 26.0 ms |

Stage 3.2 removes the per-draw wait.

**Checked:**
- the smoke test: PASS;
- frame 4200 byte-identical in both eyes (`89FDFF9DEFEA1038`, `33CB7D247CB2B0D0`) and in mono (`40D8480C103CD22C`);
- `WWHD_VK_THREAD_CHECK=1`: it logged every area's first use from the atlas's thread, as expected (each area was made
  safe for two threads in stages 2.2 to 2.9; the check is an inventory, not a race detector). Its first run's left eye differed in 0.04% of pixels in one patch (a timing
  variant), and a repeat matched in both eyes;
- 608 of 608 tests.

## Stage 3.2 done: the atlas's thread overlaps the scene

With `WWHD_VK_ATLAS_THREAD=1` (still off by default) the render thread no longer waits for the atlas's draws. It
queues the whole block and goes on with the scene while the atlas's thread catches up.

**The block on the atlas's thread:**
- **Draws are handed over in batches,** one per draw, with the register writes before it. There is no allocation per
  job once warm, and the thread is only woken while it is idle.
- **A draw the registers prove writes no colour** (colour writes disabled, or no colour target both unmasked and bound)
  is not waited for. All 3,030 of the frame's are. Any other atlas draw is waited for as in 3.1, and the atlas's
  thread checks the proof.
- **The atlas's own depth clears** (two a frame, between cascades) are done there, in order, from a copy of the
  guest's depth buffer taken when they were queued. They used to end the held stream, so the block was one overlap
  instead of three.
- **A guest flush (`GX2Flush`) waits** until the threads join, at the latest before the render thread waits for
  commands (`render::before_idle`). It only starts the GPU sooner. Draw batching's submissions also pause meanwhile.
- **The block's invalidates are applied at once** on the render thread, as before the thread: the scene that follows
  must see them.

**The scene while the atlas's thread runs:**
- **Eye 0's commands are held in a scene stream.** Eye 1's are held as before.
- **Eye 1's flush from inside a draw** (`stereo_release_texture`, maybe inside a cache lock) does not join. Eye 1's
  held commands continue the scene stream instead.
- **Uploads and layouts of game textures the atlas's thread has not used yet** go into a stream emitted before the
  atlas's. That is where the atlas's own first use would have put them, since the scene only uploads a texture the
  atlas has not checked this frame, from the same guest memory.
- **The threads join** before a scene draw that may sample the atlas (any stage's texture unit naming its image), an
  invalidate, a submission, an in-order clear or copy, the swap, or the render thread running out of commands. The
  streams then go to the renderer's in order: before-atlas, atlas, scene.
- **The textures the atlas sampled last frame are made ready** (uploaded if changed, then laid out for sampling) on
  the render thread at the overlap's start: about 15 a frame.

**Safety nets:**
- **The atlas's thread using a surface the render thread changed in this overlap** switches overlaps off for the
  session, with the surface and the change logged. It happened only before the pre-pass laid textures out for
  sampling and before the before-atlas stream existed; neither run since has logged it.
- **A join while the render thread holds a cache lock** would deadlock. It is counted (`draw_threads::held_locks`) and
  switches overlaps off; it never happened.

**A frame (Outset, desktop stereo):**
- one overlap: 3,030 atlas draws and 2 clears on the atlas's thread;
- 1,012 to 1,067 scene draws recorded meanwhile;
- the threads join at the scene's first invalidate (draw 3,877), 4 draws before the scene first samples the atlas;
- the render thread waits 0.6 ms there;
- lock waits on both threads together: about 0.1 ms a frame (buffers 440, surfaces 120, shaders 35).

**Timing (120 Hz proxy, two runs each):**

| | atlas stream only | atlas thread (3.2) |
|---|---|---|
| render thread CPU | 14.45 / 14.53 ms | 12.65 / 12.57 ms |
| render thread waiting for commands | 1.6 ms | 2.8 ms |
| game waiting for the render thread (DrawDone) | 7.7 ms | 7.0 ms |
| frame | 17.38 ms | 17.40 ms |

The render thread gets about 1.2 ms of each frame back: 1.9 ms of CPU less, of which the join wait takes back 0.6 ms.
The proxy's frame time does not move, because the game's 30 Hz logic gives two in-between frames per step until the
render thread is fast enough for three (no atlas at all crossed that: 12.2 ms). In a headset, the 1.2 ms counts
against the 11.1 ms budget of a 90 Hz frame.

**Checked:**
- the smoke test: PASS;
- frame 4200 byte-identical in both eyes and in mono, with and without `WWHD_VK_THREAD_CHECK=1` (its timing; it lists the areas both threads use, all made safe in stage 2);
- a last run with the final build matched the default renderer's run, in the alternate game moment that both hit
  (`7B5C1B49…` / `8305B7A7…`, the timing variant seen before);
- 608 of 608 tests.

## Stage 3.3 done: hardening, and the default

All runs are desktop stereo with `WWHD_TEST_FIXED_SEED=1`, the default renderer against `WWHD_VK_ATLAS_STREAM=1
WWHD_VK_ATLAS_THREAD=1`.

**A long run** (9,100 frames):
- what it covers: a stage load into the Great Sea, walking for 1,500 frames, turning the camera, walking again;
- run three times: the default renderer, the atlas thread, and the atlas thread with `WWHD_VK_THREAD_CHECK=1`;
- result: frames 4500, 7000 and 9000 byte-identical in both eyes in all three runs;
- no conflicts and no join inside a lock (the thread check lists the areas both threads use, as expected; it does not
  detect races).

**Save states:**
- **A full state** was saved at frame 3000 and loaded at 4000 (the exact restore, through `ss_reset`'s in-order path).
  Frame 4300 is a timing-sensitive moment for the default renderer too: its runs gave `7EE04EA3…` once and
  `4CF20595…` twice. The atlas thread's runs gave `4CF20595…` once and two other variants, with no conflict logged.
  This is game-state timing after the load, not rendering.
- **A portable state** was saved at 5000 and loaded at 5800 (a stage re-entry). Frame 6600 was byte-identical in both
  eyes.

**Mono:** the thread never starts (it needs the stereo streams). Frame 4200 is byte-identical.

**Not covered:**
- a VR headset session (the XR mode uses the same stereo streams, but it has not been run);
- switching stereo on and off during play. The swap joins the threads before `stereo_latch`, so no overlap
  outlives a mode change, but no run has switched modes.

**The default: off, for now.** The evidence says the overlap is correct:
- every comparable picture is byte-identical;
- its safety nets never fired.

The gain is about 1.2 ms of the render thread's frame. It only shows as frame rate where the render thread is the
limit; in the 120 Hz proxy the frame rate stays capped. Turning it on by default should follow one headset session
with `WWHD_VK_ATLAS_STREAM=1 WWHD_VK_ATLAS_THREAD=1`, checking the log for `sun atlas thread: off for this session`.

## A cost the stages added to the default renderer, found and mostly removed

An A/B against the commit before stage 1 (`b2f8ee65`), with both built from the same tree and run alternately in
desktop stereo at default settings, showed the stages had made the default renderer's render thread about 2.0 ms a
frame slower: 14.9 against 12.9 ms of CPU, with every switch off. Frame time hid it, since the 120 Hz proxy is capped,
but in a headset it is real. A bisect spread it over stages 2.5 to 2.9 and later. A sampling profile (the thread
sampler, now also on x64; symbols from the executable via `llvm-nm`) showed every draw phase 5 to 15% slower, not one
hot spot. The causes, and what was done:

| cause | change |
|---|---|
| per-thread state read many times a draw through `thread_local` (several dependent loads on Windows, plus a wrapper call when defined in another file) | `per_thread.h`: while no draw thread is possible this session (neither `WWHD_VK_ATLAS_THREAD=1` nor `WWHD_VK_EYE_THREAD=1`), the selected eye, the held stream being recorded into, pass state, counters, upload arena, descriptor pool, the register machine's counters, the profiler's marks and the draw path's caches and memos live in plain globals. Their accessors are inline, one branch on a constant |
| accessors made out-of-line calls (`pass_state`, `draw_counters`, `upload_blocks`, `descriptor_pool`, the eye maps, `thread_state_counters`) | inline, in the headers |
| switches read through function statics on every draw (`stereo_atlas_emit`, the timeline, the eye segments, the threads' `running`/`enabled`, `on_atlas_thread`, `before_change`, `stereo_before_atlas`, the thread check, the lock flags) | namespace-scope constants (`stereo_switches.h`), and inline fast paths that return at once while their feature is off |
| cache locks built and destroyed out of line with no second thread | an inline constructor and destructor that only test a flag; the locking itself out of line |
| every register write tested the forward, the verifier and a function-static switch | one test of three flags for the common case |

Result: three alternating pairs, render thread CPU 13.40 ms (before stage 1) against 14.05 ms now, so about 0.65 ms
of the 2.0 ms remains. In wall-clock time per operation the gap is about 0.4 ms (draws +0.33, register writes +0.07).
The rest is spread thinly over many functions; code growth is the likely cause. Pictures are byte-identical with
defaults and with the atlas thread; 608 of 608 tests pass.

Also found on the way: eye 1's thread object was made the first time `eye_thread::running()` was asked, even with the
thread off. With the plain globals its start-up wrote the render thread's state and crashed the record worker (a
driver read of a bad handle). The object is now made only by `begin`, and a draw thread refuses to start unless
per-thread state is on.

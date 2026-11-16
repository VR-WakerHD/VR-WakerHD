# VR streaming audit: OpenXR use on streamed headsets (2026-11-14)

What this is: a read-only audit of how the VR build drives OpenXR on streamed headsets: Virtual Desktop's VDXR, Steam
Link, Quest Link and Air Link, and ALVR. Its goal is to find what in our code adds latency or drops frames. No code was
changed, nothing was run, and no benchmarks were taken. Every "measure" below is a proposal. Line numbers are as of
`9958337d`.

What set it off: a Galaxy XR player on Virtual Desktop (VDXR 1.1.0, reported as a Quest 3, RTX 5090, 90 Hz, Streamer
1.34.25) reported head-movement jitter, Wi-Fi latency and dropped frames. In their earlier log the game's side looked
clean: a steady 90 fps, even frame times, evenly spaced display times, and `xrWaitFrame` returning in about 0.03 ms.
The log read for this audit (`Downloads\wwhd.log`, build `acd955e`) is a short session. It has no 10 s timing lines.
It does show Depth for Virtual Desktop on, Streaming optimal on, Auto at 90, the hands layer going on and off, and a
3089x2880 UI layer swapchain.

Scope note: the pasted brief "Comprehensive PCVR Streaming Pipeline Performance Audit" asks for implementation, builds
and before/after headset benchmarks. This session is read-only and cannot run a headset. Its deliverables appear here
as far as reading code allows:

- the architecture and ownership map (section 1);
- ranked bottlenecks (section 3);
- what needs runtime support (section 4);
- compatibility and settings (sections 5 and 6);
- a test plan instead of results (section 7).

Encoder, bitrate and network adaptation belong to the streaming runtime, not to this game (section 1). Nothing in them
can be implemented here.

---

## 1. The frame pipeline and who owns each stage

```
 GAME (ours)                                              RUNTIME / STREAMER (not ours)            HEADSET (not ours)
 ─────────────────────────────────────────────────────    ───────────────────────────────────     ──────────────────
 game thread (30 Hz logic, interp passes 45/60/72/80/90)
   │  camera: head pose located at display_time()           VDXR / OVRServer / SteamVR /
   │  (vr_game.cpp:1461) = last real frame's P + k·T         ALVR server
   │  controllers: xrSyncActions + xrLocateSpace
   ▼
 GX2 command stream → render thread (+ scene threads,
   record worker, sun-atlas thread)
   │  scene drawn multiview into the game's own eye
   │  targets (2688x2880 per eye here)
   ▼
 render::swap()  (backend.cpp:4408-4427)
   ├ frame_begin: end-thread join → xrWaitFrame → xrBeginFrame ──► pacing, predictedDisplayTime
   │   → xrLocateViews (FOV/IPD for the NEXT frames)
   ├ present(TV mirror) / record_eyes: acquire+wait swapchain,
   │   blit eye targets → swapchain images; flush to queue
   ├ frame_end: views the frame was drawn with → job
   └ planned_repeats: turn-smoothing frames (low rates only)
 end-frame thread (vr_vulkan.cpp:421-558)
   └ under graphics_queue_mutex: release images, xrEndFrame ──►  layer composition (VDXR on the PC)
                                                                 ► video encode (NVENC H.264/HEVC/AV1)
                                                                 ► transport (Wi-Fi / USB)             ► decode
                                                                                                         ► timewarp /
                                                                                                           SSW / ASW
                                                                                                         ► display
```

| Stage | Owner | What the game can affect |
|---|---|---|
| Logic, camera, controller poses | game | prediction time, pose consistency |
| Scene rendering, eye images | game | resolution, GPU time, evenness |
| Frame loop (`xrWaitFrame`/`Begin`/`End`) | game calls, runtime paces | follow the runtime's pacing or not (finding 1) |
| Layer list and contents | game | layer count, sizes, head-locked layers, depth |
| Composition of layers | runtime (VDXR composites on the PC before encoding, as far as is known; Meta and SteamVR on the headset or PC by their own rules) | fewer and smaller layers |
| Encoder: codec, bitrate, rate control, keyframes, B-frames, VBV | streamer (VD Streamer, Meta Link, SteamVR / Steam Link, ALVR) | **none**: no OpenXR API exists. Only even frames and a GPU with headroom |
| Network adaptation, packet loss, jitter buffers | streamer | none |
| Decode, timewarp, SSW/ASW, display | headset runtime | depth (`XR_KHR_composition_layer_depth`) for depth-aware reprojection; nothing else |

So sections 2 and 3 of the brief (encoder and bitrate) cannot be done in this codebase. A game that set codecs or
bitrates would be reimplementing the streamer, which the brief itself rules out. Players set these in the streamer's
own app (section 6).

---

## 2. What is already right (checked against the spec)

- **Call order.** `xrWaitFrame` → `xrBeginFrame` → `xrEndFrame` (`openxr_session.cpp:1036`, `:1098`, `:1466`). The
  end-frame thread is joined before the next `xrWaitFrame` (`vr_vulkan.cpp:1082`, `:543-557`), so the frames never
  overlap. `XrFrameEndInfo.displayTime` is the predicted time of the same frame's wait (`openxr_session.cpp:1176`): the
  join keeps the next wait from overwriting it.
- **Swapchain GPU sync.** The eye copies are flushed to the queue before release (`backend.cpp:4414-4419`). Release
  and `xrEndFrame` happen under `graphics_queue_mutex` (`vr_vulkan.cpp:462`), as `XR_KHR_vulkan_enable2` requires
  (the runtime relies on queue order). The CPU never waits for the GPU before release.
- **Swapchains.** sRGB RGBA8 (`openxr_session.cpp:748`), 1 sample, 3 images, at exactly the runtime's recommended size
  (2688x2880 = VDXR's own "1.000 supersampling"). The game draws its eye targets at that size (log: "screen targets
  sized to the headset resolution's 2688x2880"), so nothing is drawn beyond what the streamer asked for. The eye copy
  is 1:1.
- **Layers.** The base projection layer has no blend flags. Quads set `BLEND_TEXTURE_SOURCE_ALPHA` without the
  unpremultiplied flag, which matches their premultiplied contents. The hands layer sets `UNPREMULTIPLIED` where its
  contents are not premultiplied (`openxr_session.cpp:1336-1337`).
- **Pose drawn = pose submitted.** `frame_end` submits the views the frame's camera was drawn with, plus the eye
  offsets its passes used (`vr_vulkan.cpp:1964-1980`). The only deliberate change is the stick-turn time correction
  at low rates (`:2002-2027`). The compositor reprojects from exactly the drawn pose, so a prediction error becomes a
  timewarp correction, not a wrong image. There is one exception: a frame whose camera is set more than once is drawn
  with one call's pose and sent with the last call's. The existing `[vr] camera calls per frame` line measures this.

---

## 3. Findings, ranked by likely effect on latency and dropped frames

Each finding gives evidence, the proposed change, its risk, and whether it needs a headset. Changes that would add
render-thread work or a lock or wait between threads are marked **needs owner OK**, per the standing rules.

### 1. The game's frame clock runs free of the runtime's (`xrWaitFrame` never paces it on VDXR)

**Evidence.**
- The game steps and draws on a virtual vsync built from `std::chrono::steady_clock` at the nominal refresh period:
  `g_vsync_epoch` (`gx2_core.cpp:2354`), `vsync_clock_now` (`:2372`), and `lock_to_headset` setting the period from
  `xrGetDisplayRefreshRateFB` (`vr_vulkan.cpp:579-593`, `gx2_core.cpp:2416`).
- `xrWaitFrame` is called only when the render thread reaches the swap, after the scene is drawn
  (`backend.cpp:4408`).
- A wait of 0.03 ms every frame means the game reaches `xrWaitFrame` at or after the moment VDXR would release it.
  The runtime never throttles, so the game's clock alone sets when frames are made.
- Two 90 Hz clocks (the PC's `steady_clock` and the headset's display clock, relayed over the network) differ by a
  small, unknown amount. Any difference makes the frames' phase against the headset's refresh drift. Each time the
  drift crosses a slot, either one refresh gets no new frame (the streamer repeats or reprojects the last one: a
  dropped frame the game's own log never shows) or two frames compete for one slot.
- On a wired headset `note_display` moved the grid when frames got near the deadline (`vr_vulkan.cpp:598`). On a
  streamed headset with Streaming optimal it is switched off (`:604`), and nothing replaces it.
- The game also cannot see this today. Predicted display times are XR time, and the game never converts them to its
  own clock: `XR_KHR_win32_convert_performance_counter_time` is offered by VDXR (and by Meta and SteamVR) but not
  enabled (log: "OpenXR extensions enabled (4)").

**Proposed change.**
- (a) Measurement first: enable `XR_KHR_win32_convert_performance_counter_time` and, every 10 s, log the lead
  (`predictedDisplayTime` minus the time `xrWaitFrame` returned, both on one clock) and its drift (ppm). Do the
  conversion on the end-frame thread from values the render thread already stores, so the render thread does nothing
  new.
- (b) If the lead drifts: phase-lock the game's virtual vsync to the runtime. Adjust `g_vsync_shift` (a slow,
  heavily filtered loop over seconds, since streamed times carry network noise) so frames reach `xrWaitFrame` a fixed,
  small margin before the runtime's wake point. That margin is also the extra latency we stop adding.

**History (section 9).** (b) must not bring back what was removed on 2026-11-03 (`7102cf4c`). On a streamed
headset, the frame-timing shift read network jitter in the display times as lateness, and each of its moves was a
hitch. A loop that follows the streamed display times can do the same, so (b) would be:
- built only if (a) shows a real drift;
- continuous, from the drift filtered over tens of seconds, never stepping the grid;
- off on streamed headsets unless measured there.

It must also keep `lock_to_headset`'s 30.00 steps a second and the 72/80 Hz locks (VR_FRAME_RATE.md, "Locked to the
headset's clock").

**Searched (2026-11-14): never tried before.** The git history, the docs and past session transcripts show no
attempt to drive the game's frames from `xrWaitFrame`, to phase-lock to the runtime, or to use the time-conversion
extension. What exists:
- `74a02da9` (10-28): the grid locked to the nominal refresh, plus the half-refresh shift (`note_display`). This fixed
  45 fps frames sliding across the deadline (50-100 a 10 s window). It is kept.
- `7102cf4c` (11-03): the shift left out on streamed headsets, since network jitter made each move a hitch. It was
  tied to Streaming optimal on 11-05 (`71c061d5`). That is the only reversal in this area, and (b)'s guards come from
  it.

There is a related lesson from the flat game (performance.md, issue #164, 10-12). Two clocks (a wall-clock step
schedule and the vsync grid) drifted 0.034 ms a step and made a 16 s stutter cycle. The fix was one clock, the grid.
So (b) adjusts the grid itself (`g_vsync_shift`) and adds no second schedule.

**Risk.** (a) none. (b) medium: it touches the pacing core. The logic stays at 30 steps a second on average but
follows the headset's clock within parts per million, the same kind of change `lock_to_headset` made (29.97 → 30.00).
**Needs owner OK** because it adjusts the logic clock. **Headset test:** yes. (a) on VDXR and a wired headset gives
the drift; (b) is judged by frames a refresh late and repeats.

### 2. Camera pose predicted a refresh too far in a race window (head, hands and controller jitter)

**Status (2026-11-14): implemented, built, on by default, not run in a headset.** VR_FRAME_RATE.md, "The head pose
counted from its own swap". `WWHD_VR_PREDICT_PAIRED=0` restores the old count for a comparison run.

**Parked (2026-11-14):** the owner has no headset to test with. The rest of this audit stays as proposals. Next, when
testing is possible: the measurement-only steps 1A and 5A, run by players.

**Evidence.**
- `display_time()` = `real_frame_display` + (1 + `swaps_in_flight`) × refreshes per frame × period
  (`vr_game.cpp:1461-1488`, `:1474`, `:1486`).
- `real_frame_display` is set in `frame_begin`, inside `render::swap()` (`vr_vulkan.cpp:1101`). `g_swaps_executed`
  goes up only after `render::swap()` returns (`gx2_core.cpp:2177-2178`), which is after the eye copies, the flush,
  `frame_end` and `planned_repeats`.
- A camera located by the game thread in that window gets swap N's new display time as its base and still counts swap
  N as in flight. It is predicted one refresh (11 ms at 90 Hz) further than the frame will be shown.
- Whether a frame lands in the window depends on thread timing, so the horizon flips between frames. Rotation is
  corrected by the compositor (the pose sent is the pose drawn). Translation without depth, and Link's hands and items
  (drawn in the scene from controller poses predicted the same way, `sync_input(display_time())`), are not corrected.
  On a streamed runtime that already predicts tens of ms ahead, an extra 11 ms adds prediction error.
- The `[vr] camera pose predicted ahead ... changes` line counts these flips. It was not in the log provided.

**How often it happens is not known.** The 2026-11-05 wireless audit read the `camera pose predicted ahead` lines of
an earlier Virtual Desktop player as mostly "1" (VR_WIRELESS_AUDIT.md, problem 4), so the window may be rare there.
Check that line in the new player's logs before ranking this high. The fix must keep the 2026-10-28 rule that counts
each frame in refreshes per drawn frame (2 at 45 fps, 3 at 30; VR_FRAME_RATE.md, "The head pose at 45 and 30 fps").

**Proposed change.** Pair the base with the swap it belongs to: store the swap index with `real_frame_display` (one
more relaxed atomic store next to the existing one at `vr_vulkan.cpp:1101`), and count in-flight swaps after that
index instead of `g_swaps_executed`.

**Risk.** Low on its own: one store beside an existing one, and no new wait. **But it feeds Turn smoothing**
(section 8). The camera's `v.time` drives the turn-rate measure (`vr_game.cpp:2088-2091`), the carried rate
(`:2199-2216`), `since_shown` and the real-frame time correction (`vr_vulkan.cpp:2015`). These were tuned with the race
present (for example the rate smoothing `kRateTau`). The display-time blend was turned off for this same jump
("`display_time` ... jumps by a frame whenever the queue's depth changes", FRAME_INTERPOLATION_IMPLEMENTATION.md
change 1). **Headset test:** yes. Compare the "camera ahead" counts and changes, then at Capped 45 with Turn smoothing
check that `steady stick turn` and `frame evenness` are no worse, before keeping it.

### 3. `show_again` holds the graphics queue lock across `xrWaitFrame`

**Evidence.** `show_again` takes `graphics_queue_mutex` (try-lock) at `vr_vulkan.cpp:2193`, then calls `begin_frame`
(`xrWaitFrame`) at `:2198` and `xrEndFrame` at `:2233`, all under it. On a runtime whose `xrWaitFrame` blocks (Meta
Link, SteamVR and Steam Link, wired headsets), each turn-smoothing repeat (`planned_repeats`, `:2634`) or kept-alive
frame (`headset_frame`) holds the queue for up to a refresh. The record worker, the scene threads, the desktop mirror
and the texture uploads cannot submit meanwhile, so the next real frame can miss its slot. The generated-frame path
already takes the lock only around release and end (`:2590-2603`).

~~**Proposed change.** Take the lock only around `xrEndFrame`, as at `:2602`.~~

**Withdrawn after the turn-smoothing review (section 8).** The try-lock before `xrWaitFrame` is deliberate and dates
from the first kept-alive frames (`c3890e1c`, 2026-10-28). A frame must not be begun unless it can be ended, and the
queue "may be held by a thread that waits on the render thread". Taking the lock after `xrBeginFrame` would have to
block, which risks a deadlock. The gain is also small: while repeats run, the render thread itself is the one blocked,
and the threads it feeds have nothing new to submit. **Do not change.**

### 4. Turn smoothing on a streamed headset mixes two clocks (commit `c043cf4f`)

**Evidence.**
- With `turn_step_clock()` on (streamed, Streaming optimal; `vr_vulkan.cpp:2176-2190`), repeats are turned by whole
  refreshes from the game frame (`:2209-2212`).
- The real frame before them is still turned by the network-moved display-time gap
  (`predicted_display_time() - v.time`, `:2015`).
- In one turn, frame 0 follows the network clock and frames 1..n follow the step clock. Whenever the gap is not a
  whole number of refreshes, the step between the real frame and its first repeat is off: the uneven stick turn the
  commit set out to fix, now once per game frame.

~~**Proposed change.** Correct the real frame by 0 in step-clock mode.~~ **Revised after the turn-smoothing review
(section 8).** A correction of 0 would undo "Every frame timed to the refresh it lands on" (2026-11-12,
`WWHD_VR_TIME_CORRECT`). That change fixed the doubling from real frames landing a refresh off, and was confirmed in a
headset ("real and generated frames stepping alike").

**Revised proposal, in two steps.**
1. Test `c043cf4f` as it is first. It has never been in a headset. Run Virtual Desktop at Capped 45 with Turn
   smoothing, `WWHD_VR_TURN_STEP_CLOCK=1` against `=0`, and compare the `[vr] steady stick turn` and `frame evenness`
   lines.
2. Only if the share of refreshes "off by a quarter or more" stays high with the step clock: in step-clock mode, round
   the real frame's gap to whole refreshes (k = round(gap / period), corrected by k·period), and turn repeat i by
   (k + i)·period. The landing correction stays, only the network's sub-refresh noise goes, and both use one clock.

The wired path (step clock off) stays unchanged.

**Risk.** Low, limited to streamed headsets with Streaming optimal and Turn smoothing at 45/36 fps. **Headset test:**
yes, on Virtual Desktop.

### 5. Frames bunching when `xrWaitFrame` returns at once (turn smoothing, kept-alive frames)

**Evidence.**
- `planned_repeats` submits 1 to 3 frames right after each real frame and relies on `xrWaitFrame` to space them a
  refresh apart (comment at `vr_vulkan.cpp:2150-2152`).
- On VDXR that wait took 0.03 ms. If it also returns at once for repeats, the repeats go out within a millisecond of
  the real frame. What VDXR does then is not known: it might queue them, replace one with the next (a dropped frame),
  or give them the same predicted time.
- The existing `[vr] repeated frames' timing` line counts repeats more than 1.5 refreshes late, but not repeats in the
  same slot as the frame before.

**Proposed change.** First, a diagnostic: count repeats whose predicted display time is not after the previous
frame's, and log their wait. This is a compare in an existing log path, but on the render thread: **needs owner OK**.
If they bunch, the fix must keep Turn smoothing working, since it is the setting the owner likes (section 8).
Do not stop sending repeats. Pace them instead, which needs a design and the owner's OK. The first, slipped-in version
of Turn smoothing (2026-11-12) took real frames' slots and lost: 147 missed refreshes in 10 s.

**Risk.** Diagnostic none. Change medium (a behaviour choice). **Headset test:** yes, VDXR at 45 fps while turning.

### 6. Head-locked quads on a runtime that flattens layers before encoding

**Evidence.**
- The UI layer is placed in VIEW space unless conversation text is held in the world (`openxr_session.cpp:1369`).
  The wrist HUD's Free Roam label is too (`:1437`).
- Where the streamer composites the layers into the video on the PC (VDXR, as far as is known; ALVR and Steam Link
  through SteamVR do too), the headset's timewarp then moves the whole picture, head-locked UI included, by the head's
  motion since the frame was made. Head-locked text then swims or jitters with head movement and network latency,
  instead of staying still as it does on a wired or Link compositor.
- The UI swapchain is large: 3089x2880 RGBA8 here, the symmetric field's size (`vr_vulkan.cpp:2045-2051`,
  `openxr_session.cpp:816-829`). It is sampled in full per eye per frame whenever shown.

~~**Proposed change.** A headset A/B with the UI quad in the tracking space, re-posed each frame.~~ **Revised after the
history check (section 9).** That A/B cannot help. Once a streamer flattens the layers, a tracking-space quad posed at
the predicted head pose and a VIEW-space quad come out the same in the encoded picture. Head-locked content then moves
by the headset's correction, which is the prediction error, a fraction of a degree, not the whole latency. Moving the
quad would also undo the deliberate head-locked UI layer design (VR_EYE_PROJECTION.md, "The game's UI as its own
layer"), and conversation text is already held in the world (VR_CUTSCENE_TEXT.md).

**Remaining proposal:** submit only the UI image's drawn bounding box as `subImage.imageRect`, so the compositor
samples fewer pixels. The layout code knows the box. The swapchain stays as it is: made once, never remade
mid-session, where SteamVR hung.

**Risk.** Low to medium, since UI placement has regressed before. **Headset test:** yes. Likely effect: small. Ranked
low now.

### 7. A second full projection layer for the hands

**Evidence.**
- At low rates (Auto under the headset's rate, Capped 45, Steady 30) a second projection layer is sent with alpha
  (`openxr_session.cpp:1322-1341`). Its shared image is twice the eye width (5376x2880).
- On SteamVR this measured ~3.8 ms of hand-off against ~0.8 ms without it, and ~0.85 ms with the shared image (comments
  at `:1313` and `:830-832`).
- On VDXR, if it composites on the PC, it is an extra full-resolution alpha blend per eye on the same GPU as the
  encoder, right before encoding.
- The log shows it switching on and off with Auto. Each switch changes the layer list the streamer sees.

**Proposed change.** Measure the hand-off on VDXR with and without it: the `submission per frame` line, and
`WWHD_VR_HANDS_SUBMIT=none` for an A/B. If it is costly there, send the hands only in the rows they cover (a sub-image rect per
eye). Do not turn the layer off on streamed headsets: "Steady hands while turning" is headset-confirmed and on by
default everywhere (`ccea436f`, `8dfd1a7c`), and turning it off would bring back the hands' shake in stick turns
(section 8).

**Risk.** Low for the measurement. **Headset test:** yes.

### 8. The depth layer for VDXR: its format, its size, and switching on and off

**Evidence.**
- Depth goes as D24S8 at half size (1344x1440) (`openxr_session.cpp:774-780`), with `minDepth` 0..1 and the planes
  set (`:1293-1298`).
- The spec does not require the depth image to match the colour image's size. Some runtimes assume it does, and what
  VDXR does with a half-size image is not documented.
- Depth is dropped while text, menus, the GamePad panel or the item wheel show (`:1238-1239`). The streamer's
  reprojection then switches between depth-aware and flat mid-session, a possible visible pop.
- Depth may also cost encoder bandwidth if VD sends it to the headset. That is not documented.

**Must keep (section 9):**
- no depth while text, menus, the GamePad panel or the item wheel show (`11588950`: VDXR put the person talked to over
  the conversation text);
- Head movement smoothing's frames resending the real frame's depth (`a1c0e654`: otherwise depth switches every other
  refresh);
- Depth for Virtual Desktop and Streaming optimal off by default (`d1f43896`).

**Proposed change.** A headset A/B on VDXR. First, `vr.vdxrDepth` on against off, with VD's SSW off and then on,
comparing judder in fast head turns and translations. Second, if depth on helps, try full-size depth. Do not change
the default without that test.

**Risk.** Low, since these are setting tests. **Headset test:** yes.

### 9. Render-thread runtime calls and a shared tracking lock in `frame_begin`

**Evidence.**
- `frame_begin` calls `xs().refresh_rate()`, which is `xrGetDisplayRefreshRateFB` (`openxr_session.cpp:923-928`),
  four to six times per frame: `vr_vulkan.cpp:1108`, `:1110`, `:1131`, `:1141` and `low_rate_repeats`.
- It also calls `locate_views` (`:1155`) under `trackingMutex_` (`openxr_session.cpp:1480`). The game thread holds the
  same lock for `sync_input` (`xrSyncActions` plus four `xrLocateSpace`, `:1812-1830`) and its own locates. The render
  thread can wait on the game thread there.
- The render thread's views only set the field of view and eye offsets for later frames. They are never drawn or
  submitted.

**Proposed change.** Read the refresh rate once per frame.

~~Use the game thread's views for the field of view and eye offsets.~~ **Revised after the history check (section
9).** The render thread's locate is the documented source of the eye offsets drawn and latched after the swap
(VR_CAMERA_AUDIT.md, inputs table). `frame_end` submits the eyes at exactly those offsets ("Both eyes, one snapshot").
Taking them from the game thread would let the eyes drawn and the eyes sent come from different locates again. Keep
the locate. The shared `trackingMutex_` is noted only: changing locks needs the owner's OK.

**Risk.** Low. **Headset test:** a short one: the "end-frame thread waited" and timing lines must not get worse.

### 10. `xrBeginFrame` after the scene's GPU work

**Evidence.** The scene is drawn and largely submitted before `xrWaitFrame` and `xrBeginFrame` (`backend.cpp:4408`).
OpenXR expects `xrBeginFrame` to mark the start of the frame's rendering. Runtimes that time the app's GPU from
`xrBeginFrame` to `xrEndFrame` (Meta's for ASW and performance HUDs; VDXR's own stats) see only the eye copy. Their
latency and frame-timing statistics for this game are then too small, and any pacing they base on it is off.

**Proposed change.** None on its own: finding 1(b) would move frame starts onto the runtime's schedule. Note it when
reading Meta's or VD's performance overlays.

**Risk.** n/a. **Headset test:** n/a.

### 11. Smaller items

- **Copy instead of rendering into the swapchain.** The eyes are drawn into the game's own targets and blitted 1:1
  into the swapchain images (`record_eyes`, `vr_vulkan.cpp:1415+`). That is about 2 × 7.7 MP per frame, a fraction of
  a millisecond on an RTX 5090. Rendering straight into array swapchain images would be a large change for little gain.
  Not recommended now.
- **Swapchain wait timeout.** `xrWaitSwapchainImage` times out after 100 ms on the render thread
  (`openxr_session.cpp:1112`). That is correct as a guard and costs nothing normally.
- **Streamed detection.** ALVR behind SteamVR is detected by name only if the system name says so. The runtime is
  SteamVR, so the `alvr` check on the runtime name misses it (`openxr_session.cpp:446-447`). The display-time
  measuring catches it within 10 to 40 s, but Streaming optimal is a launcher choice. Low impact.
- **Head jump at start.** `head tracking jumped 1.21 m in 36 ms` right after focus is the Quest STAGE origin settling.
  It was held, and is harmless.

### 12. The runtime's frame period doubles while Virtual Desktop's smoothing runs (2026-11-14, from VDXR's source)

**Evidence.**
- VDXR is open source (MIT): github.com/mbucchia/VirtualDesktop-OpenXR. The player's build, VDXR 1.1.0 at commit
  `6da20fe4`, is in it.
- Its `frame.cpp` (`xrBeginFrame`) sets the frame period to twice the ideal whenever OVR's perf stats report
  `AswIsActive`, which is how Virtual Desktop's Synchronous Spacewarp shows up. `xrWaitFrame` then reports that as
  `predictedDisplayPeriod`. No setting gates it, and its own comment says the opposite.
- Our code read that period as "one refresh":
  - Turn smoothing's whole-refresh steps on streamed headsets, its clamps, and the camera's carried turn;
  - the "a refresh late" checks and `frame evenness`;
  - the kept-alive frame timer;
  - the streamed display-time checks and streamed detection.

  While SSW ran, all of these doubled. With SSW on Automatic it switches on and off with load.

**Changed (2026-11-14, built, not run in a headset).**
- `Session::refresh_period()` is set at each `xrWaitFrame` from the headset's refresh rate
  (`XR_FB_display_refresh_rate`, read once a frame, which `refresh_rate()` now returns instead of asking four to six
  times a frame). Without that extension it uses the runtime's period, unless that jumped to 1.8 times the last
  refresh or more.
- The "one refresh" uses above read it.
- The head pose's prediction distance (`display_time()`) still reads the runtime's period, which is part 2 and parked.
  If Virtual Desktop holds the game to half rate while SSW runs, as Oculus's ASW does, the doubled spacing is right
  for that use. Player logs decide.
- **Log only:** `[vr] the runtime's frame smoothing: on (it asks for frames N refreshes apart ...)` / `off` (the
  first 30 changes) and `[vr] the runtime's frame smoothing (10 s): on in N of M frames, switched K times`. Checked on
  the end-frame thread after the frame's end, so nothing is added to the render thread, and no lock is taken.
  Nothing is logged with `WWHD_VR_END_FRAME_THREAD=0`.

**Unchanged:** wired headsets, and Virtual Desktop with SSW off. Both periods are equal there.

### What VDXR's source settled for findings 5 to 8

- **Finding 1 (supported).** VDXR submits frames on its own thread, which does the OVR pacing (`ovr_WaitToBeginFrame`).
  `xrWaitFrame` waits only until that thread has taken the last frame, or until a refresh less 2 ms after the last
  frame start ("running start"). A wait of 0.03 ms means the game arrived after the thread was already idle: the
  game's clock set the pace.
- **Finding 5 (likely a non-issue on VDXR).** A repeat calls `xrWaitFrame` right after the real frame's end, while that
  frame is still queued, so it waits until the thread takes it or until the running-start deadline. Each frame also
  gets OVR's predicted time for its own frame number. Repeats are spaced, not bunched.
- **Findings 6 and 7 (weaker).** VDXR does not flatten layers. Every layer goes to OVR as a native layer
  (`ovrLayerType_EyeFov`, `Quad`, `Cylinder`), and view-space quads get `ovrLayerFlag_HeadLocked`. What Virtual
  Desktop's streamer does with them before encoding is closed source.
- **Finding 8.** Depth is used by default (`quirk_use_depth`, default on) and sent as `ovrLayerType_EyeFovDepth`.
  What the streamer does with it is closed source.

---

## 4. Needs runtime or driver support (not doable in the game)

- Codec (H.264, HEVC, AV1), bitrate, rate control, keyframe interval, B-frames, VBV, adaptive quantization: the
  streamer's own settings. OpenXR has no API for them.
- Network jitter, packet loss and round-trip time: not exposed by any OpenXR extension these runtimes offer.
- Encode and decode latency, reprojection ratio, motion-to-photon: VD's own overlay, Meta's ODT / OVR Metrics, and
  SteamVR's frame timing show them. `XR_META_performance_metrics` is not offered by VDXR (log), so the game cannot
  read them there and must not invent them.
- Frame synthesis (`XR_EXT_frame_synthesis`, `XR_FB_space_warp`): not offered by VDXR. Depth only is what VDXR takes.
- Generated frames (`vr.frameGeneration`): out of scope, stays off.

What the game can measure honestly:
- its own frame times, GPU busy share (already logged), and `xrWaitFrame` waits;
- display-time steadiness (already logged on streamed headsets);
- with finding 1(a), the lead from frame start to predicted display, on one clock.

The brief's in-game overlay would show only these, each labelled as measured by the game.

---

## 5. Compatibility notes

| Runtime | Detected as | Pacing (`xrWaitFrame`) | Layers composited | Notes |
|---|---|---|---|---|
| Virtual Desktop (VDXR) | streamed, by runtime name | returned in 0.03 ms in a player log: the game is never throttled (finding 1) | on the PC before encoding, as far as is known (findings 6, 7) | depth only; refresh rate via `XR_FB_display_refresh_rate`; no Super Resolution, no frame synthesis |
| Virtual Desktop → SteamVR | streamed (Quest or Pico name) | SteamVR paces | SteamVR on the PC | finding 3 applies |
| Quest Link / Air Link (Meta runtime) | streamed (Quest name) | Meta paces; blocks | Meta's compositor; layers may go to the headset | ASW is Meta's; finding 3 applies |
| Steam Link | streamed (Quest name) | SteamVR paces | SteamVR on the PC | finding 3 applies |
| ALVR (via SteamVR) | streamed by measuring, not by name (finding 11) | SteamVR | SteamVR | |

---

## 6. Settings for Quest 3 and Quest 3S (until the fixes are measured)

In the game:
- Streaming optimal on (Detect Headset does this).
- Frame rate: Auto Mode, which Detect Headset keeps with its streamed profile (VR_STREAMING.md, "Every frame-rate
  mode"). Capped at the stream's refresh rate is the alternative: 90 on an RTX 5090, 72 if Wi-Fi is marginal (72/80
  get a frame every refresh).
- Anisotropic filtering on: less shimmer in head movement, which also eases the encoder (VR_WIRELESS_AUDIT.md).
- Headset resolution at 100 % (the streamer's recommended size is already the encoder's input).
- Depth for Virtual Desktop: A/B test it (finding 8), don't assume it helps.
- Hands layer and Turn smoothing only at 45 or 36 fps.

In the streamer (player-reported and from vendor documentation, not measured here):
- VD's SSW set to off or always on, never automatic. Off with Turn smoothing.
- The stream's refresh matched to the game's rate.
- On an RTX 40/50, VD's AV1 or HEVC codec at VD's suggested bitrate.
- A dedicated 5 or 6 GHz router near the play space.
- For Link, Air Link and Steam Link: the encode bitrate and resolution in their own tools (Meta ODT, SteamVR video
  settings), not in the game.

---

## 7. Test plan (no results yet: nothing was run)

For each change, compare the same scene (Outset Island, a fixed walk and a 90-degree stick-turn sequence) at 72 and 90
Hz on VDXR and at 90 Hz on Link or SteamVR. Use the existing 10 s lines plus the new lead line from finding 1(a):

- `[vr] frame times` and `frame pacing` (p50/p95/p99, 1 % low);
- `submission per frame` and `end-frame thread`;
- `GPU busy`;
- `streamed display times`;
- `camera pose predicted ahead`;
- `frame evenness` and `steady stick turn`;
- `repeated frames' timing`.

Alongside them, the streamer's own overlay for dropped and reprojected frames and latency (VD's performance overlay,
ODT/OVR Metrics). A run counts as better only if the streamer's dropped and reprojected counts fall with the game's
numbers no worse.

Suggested order (revised after section 8):
1. Finding 1(a), measurement only.
2. A Virtual Desktop headset run of `c043cf4f` as it is (step clock on against off).
3. Finding 2, with the Turn smoothing checks above.
4. Finding 4's rounding only if step 2 says so. Finding 3 is withdrawn.
5. The A/Bs in findings 7 and 8, the refresh-rate cache (finding 9), and the UI crop (finding 6) last.
6. Decide on finding 1(b) and finding 5 from what they show.

---

## 8. Turn smoothing: the history behind it, and what any change must keep (2026-11-14 review)

Sources: FRAME_INTERPOLATION_IMPLEMENTATION.md (change 3 and the 2026-11-12 sections), VR_HANDS_LAYER_PLAN.md,
VR_STREAMING.md, and commits `c3890e1c`, `c0ef529c`, `ccea436f`, `8dfd1a7c`, `f5509be0` and `c043cf4f`.

### Kept: headset-confirmed, or what the owner likes

| What | Where | Result |
|---|---|---|
| Repeats planned right after each game frame, spaced by `xrWaitFrame`, never in a game frame's slot | `planned_repeats` (`vr_vulkan.cpp:2634`), from the swap after `frame_end` (`backend.cpp:4427`) | replaced the slipped-in first version, which lost |
| Real frame turned from the moment it was drawn for to the refresh it lands on (`WWHD_VR_TIME_CORRECT`) | `frame_end` (`vr_vulkan.cpp:2002-2027`) | headset run: real frames drawn 0 ms ahead on average (0-22 ms before); real and generated frames stepping alike (0.72 and 0.69 degrees a refresh) |
| Repeats measured from the views' time (`since_shown`), not the refresh the real frame landed on | `vr_vulkan.cpp:163-169` | fixed repeats carrying the real frame's landing error |
| Clamp -3..6 refreshes (was 0..3) | `show_again` (`:2211`) | fixed two repeats clamped to one moment after a hitch |
| Game frame turns by the last rate over the repeats' span, and the new rate only for its own refresh | `vr_game.cpp:2199-2216` | fixed the bounce on release and the jump on push |
| Turn rate smoothed over `kRateTau` (0.08 s); snaps, cuts and recentres never smoothed | `vr_game.cpp:2122-2141` | fixed uneven repeats from 30 Hz heading steps at 45 fps |
| Turn about the game's pivot (`vr::turn_pivot`) | generated frames | maths-checked |
| Steady hands while turning: hands layer not turned on repeats | `openxr_session.cpp:1322-1341`, `show_again` passes the unturned views (`vr_vulkan.cpp:2235`) | headset-confirmed; on by default everywhere |
| Shared hands image (one image twice the eyes' width) | `hands_shared` | SteamVR hand-off 0.85 ms against 3.7 ms with two images |
| Off at a real 90 | `low_rate_repeats`, `update_auto_low_hold` (1 s hold, menu frames not judged) | headset-confirmed 87-90 fps |
| UI layer resent on repeated frames | `show_again` passes `g_shown.uiQuad` | fixed the HUD blinking |
| Queue taken by try-lock before `xrWaitFrame` | `show_again` (`:2193`) | deadlock guard since `c3890e1c` |

### Tried and lost (do not bring back)

- Repeats slipped in whenever the render thread was idle: 147 missed refreshes in 10 s, "its worse now".
- The near field held in repeats (`WWHD_VR_TURN_NEAR_HOLD=1`): "everything is worse now". Off by default.
- Generated frames (`vr.frameGeneration`): ghosted, removed. Out of scope.
- The display-time blend (`WWHD_VR_DISPLAY_BLEND`): "fps seems worse". Off; it read the jumping `display_time`.
- Movement smoothing at 45/30: "the world is ghosting". Off by default.
- A per-pixel depth guess for steady hands (tried twice): gaps and stutter. Replaced by the hands layer.

### Not yet tried in a headset

- `c043cf4f` (step clock on a streamed headset with Streaming optimal).
- Head movement smoothing (`vr.headSmoothing`).

### What this changed in this audit

- Finding 3 withdrawn (the lock is the deadlock guard).
- Finding 4 revised: keep the landing correction and test `c043cf4f` first.
- Finding 5: never stop sending repeats.
- Finding 7: never turn the hands layer off.
- Finding 2: flagged as feeding Turn smoothing's inputs, so it needs a Turn smoothing check before it is kept.

---

## 9. Every finding against the streaming and pacing work already done (2026-11-14 review)

Sources:
- VR_STREAMING.md (all sections);
- VR_WIRELESS_AUDIT.md (2026-11-05);
- VR_FRAME_RATE.md ("The head pose at 45 and 30 fps", "Locked to the headset's clock");
- VR_PERFORMANCE.md ("One head pose a frame", "Frames while the game is busy", "xrEndFrame on its own thread");
- VR_CAMERA_AUDIT.md;
- VR_EYE_PROJECTION.md ("The game's UI as its own layer");
- VR_CUTSCENE_TEXT.md;
- section 8 (Turn smoothing);
- commits `7102cf4c`, `ac1f8595`, `c01c36de`, `11588950`, `bb2fd65f`, `71c061d5`, `d1f43896`, `815abb55`, `bfe11b4a`,
  `c3890e1c`, `a1c0e654`, `c043cf4f`.

| Finding | Earlier work it touches | Conflict? | What changed here |
|---|---|---|---|
| 1 Free-running clock | `lock_to_headset` (30.00 steps/s, the user's choice; 72/80 locks); frame-timing shift off on streamed (`7102cf4c`: it read network jitter as lateness, each move a hitch) | (a) none. (b) could bring the hitch back | (b) only if (a) shows drift; continuous, never stepping; off on streamed unless measured |
| 2 Prediction race | refreshes per drawn frame (2026-10-28); one head pose a frame; display blend off because `display_time` jumps; Turn smoothing inputs (section 8); the wireless audit saw "ahead 1" mostly | none if those are kept; may be rare | check the "camera ahead" counts first; a Turn smoothing check before keeping |
| 3 Queue lock in `show_again` | kept-alive frames' try-lock deadlock guard (`c3890e1c`) | **yes** | withdrawn |
| 4 Two clocks in turn smoothing | "Every frame timed to the refresh it lands on" (headset-confirmed); `c043cf4f` (untested) | **yes** for the first proposal | revised: test `c043cf4f` first, then round to whole refreshes |
| 5 Bunched repeats | repeats that took game frames' slots: "practically unplayable" (10-28), 147 missed refreshes (11-12) | none for the diagnostic; stopping repeats would regress Turn smoothing | diagnostic only; never stop repeats |
| 6 Head-locked layers | the head-locked UI layer design; conversation text held in the world; UI swapchain never remade | the tracking-space A/B would undo the design and cannot help once flattened | dropped; only the UI sub-image crop remains, ranked low |
| 7 Hands layer | Steady hands while turning (headset-confirmed, on by default); shared image (`ccea436f`) | turning it off on streamed would regress it | measure only; crop the rect, never off |
| 8 Depth for VDXR | the UI guard (`11588950`); Head movement smoothing's depth resend (`a1c0e654`); off by default (`d1f43896`) | none if all three are kept | "must keep" list added |
| 9 Render-thread calls | eye offsets from the render thread's locate, sent as drawn ("Both eyes, one snapshot"); one head pose a frame (game thread only) | **yes** for moving the locate | only the refresh-rate cache remains |
| 10 `xrBeginFrame` late | end-frame thread order (`bfe11b4a`) | none (no change proposed) | — |
| 11 Smaller items | streamed detection (`ac1f8595`): measured fallback | none | — |
| Section 6 settings | Detect Headset keeps Auto Mode (streamed profile) on a streamed headset (2026-11-05) | the first draft said Capped | fixed: Auto first, Capped as the alternative; anisotropic filtering added |

Unchanged by all findings:
- the 72/80 fps schedules and the even half rates (`vr_even_fps`);
- Auto's streamed profile and gameplay-only judging;
- streamed detection;
- the frame-timing shift staying off on streamed headsets;
- the end-frame thread and its order;
- the 10 s diagnostic lines.

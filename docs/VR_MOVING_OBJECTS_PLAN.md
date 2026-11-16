# Moving objects between game frames at Steady 30 and Capped 45: plan (2026-11-14)

**Status: plan only.** Nothing here is built, compiled or tested. The owner asked for plans only ("do create plans for it, i
don't want to move forward just yet on this"). The owner decides when to build. Each stage below says what it changes, its
headset test, and what result counts as a failure. Nothing counts as fixed until the owner confirms it in the headset.

## The problem

At Steady 30 on a 90 Hz headset, refresh filling (`planned_repeats` in `vr_vulkan.cpp`, on by default since 2026-11-14,
`WWHD_VR_FILL_REFRESHES=0` turns it off) sends the last game frame again on the two refreshes between game frames
(`show_again(false)`: no drawing or copying, the same images at the views they were drawn with). The runtime turns the
picture to the head, so head rotation stays smooth, and SteamVR's Motion Smoothing no longer switches on and off. The owner
confirmed it looks smoother.

Objects that move by themselves (seagulls, boats, enemies, NPCs, swinging platforms) still hold each 30 fps position for 3
refreshes. The eye tracks them at their real speed, so each held position smears across the retina: the ghosting the owner
sees, worst on distant moving objects. The runtime's reprojection corrects only the head's motion.

**Goal:** on the refreshes that refresh filling now fills with plain repeats, move only the pixels that have their own object
motion part of the way to where they are heading. Everything else (static world, sky, UI, hands layer, timing, real frames)
stays exactly as it is now.

**Not part of this plan:** SteamVR Motion Smoothing, Head movement smoothing and movement smoothing are all worse for the
owner and testers (the last two have been removed from the settings). Nothing here brings them back or relies on them. The
removed "Generated frames" (`vr_genframe`, `show_generated`) reprojected the *whole picture* for the *head* at fill time.
This plan leaves the head to the runtime, as today, and moves only object pixels. It reuses the warp's search method, not
its purpose.

## What exists (read for this plan)

| Piece | Where | What it gives us |
|---|---|---|
| Refresh filling | `vr_vulkan.cpp` `planned_repeats` (right after `frame_end` in the swap, render thread), `show_again`, `plain_fills()` | the fill refreshes: `repeats = low_rate_repeats(hz, fps)` (2 at 30 fps / 90 Hz, 1 at 45, 3 at 30 / 120 Hz; Auto by measured pace) |
| The motion pass | `motion_pass.h/.cpp`, `draw.cpp` `motion_packet`, `motion_pipeline.*`, `motion_store.h`; docs/VR_DLSS.md 5d and 10 | per eye, after the scene: every depth-writing draw drawn again with both frames' data into an R16G16F target at the eye's **depth size**, holding `(prev - cur) * 0.5` in the game's clip space. The background draws (sky, sea) where no geometry wrote depth. Implausible motion written as 0. `eye_result(eye)` gives the target, the depth it was tested against, `flipsY`, and `reset` (a cut or new scene) |
| The motion's meaning | VR_DLSS.md section 10 | **total** screen motion: head rotation and translation, stick turn, locomotion and object motion all together (the game bakes the view into its per-draw matrices, so the pass cannot leave the camera out) |
| Conversion to NDC | `vr::framesynth::motion_scale(flipsY, x, y)` | `cur - prev` in Vulkan NDC = target × (-2, ±2) |
| Scene depth and planes | `vr_framesynth::last_depth`, `last_tangents` | each eye's depth target and slice, projection terms `p[10]`, `p[11]`, `halfZ`, the field of view's tangents |
| Depth to view point | `vr_genframe.cpp` `kWarp` (`viewDepth`, `viewPoint`, `toPicture`) | GLSL already written and headset-run for turning depth back into a view-space point and projecting it into another camera, nearest-of-2×2 so outlines go with their object |
| The drawn view per frame | `g_shown` (`views`, `turnRate = vr::view_turn_rate_for`, `velocity = vr::view_velocity`), `note_shown` | the view's pose, heading, turn rate and movement per frame, which gives the camera delta between two game frames |
| When the motion pass runs | `motion_pass::on()` (decided once: `WWHD_VK_MOTION_PASS`, DLSS, or `vr::framesynth::motion_wanted()`, and only with dynamic rendering), `set_needed` (per frame: SGSR2, frame synthesis not held, DLSS) | **on PC at Steady 30 it does not run at all** (no DLSS, no frame synthesis backend). On a Quest at 90 Hz Steady 30 it is built but paused (frame synthesis is held) |

## Interpolation is not possible. The plan extrapolates by a fraction.

When a fill refresh goes out, the next game frame does not exist yet: the fills are shown *because* it is not ready. A true
"in-between" of frame N and N+1 would need N+1, which means holding back the real frame by one game frame (33 ms more
delay on moving objects, which would then lag behind the static world). That is rejected.

What the plan does: each fill shows moving pixels at **frame N + f × (motion from N-1 to N)**, with f = 1/3 and 2/3 at 30 fps
on 90 Hz and 1/2 at 45. An object moving at constant speed then shows at N, N+⅓, N+⅔, N+1: evenly spaced, and the real frame
N+1 lands where the fills were heading. Delay stays the same as today.

The cost of extrapolating: when an object stops or turns, the fills overshoot by up to ⅔ of one game frame's motion, then the
real frame pulls it back. The risk table and stage 2 deal with this.

## 1. Separating each pixel's object motion from camera and head motion

Per eye, for pixel p (eye picture uv, y down) with scene depth d:

1. **Total motion** from the motion pass: `m_total(p) = cur - prev` in uv = target(p) × (-1, ±1) (half of `motion_scale`,
   since uv spans half of NDC). The target is at depth size; with dynamic resolution or an upscaler the eye picture can be
   another size, so sample it by uv, nearest, never filtered (filtering blends motions across object edges).
2. **Camera-only motion**: where p would have been in frame N-1 if it were static.
   - Rebuild p's view-space point from d with the projection terms and tangents (`viewDepth`/`viewPoint`, as `kWarp` does).
   - Transform it by the eye camera's relative pose between the two frames, `T = E(N-1)^-1 · E(N)`. Here `E` is the eye
     camera in the game world: the tracked eye pose, the game heading and the locomotion position, in metres by
     `vr::world_scale()`.
   - Project it with frame N-1's tangents. That gives `uv_static_prev`. Then `m_cam(p) = p - uv_static_prev`.
3. **Object motion**: `m_obj(p) = m_total(p) - m_cam(p)`.

**The camera delta.** The renderer must know `E(N-1)` and `E(N)` for each eye, from the same drawn view the game was given
(`CameraOverride` writes it, VR_CAMERA_AUDIT.md section 1). The renderer already keeps the drawn view's pose, heading, turn
rate and velocity per frame (`g_shown`, `vr::view_velocity`, `vr::view_turn_rate_for`). Step 1 of building: find where
`view_velocity` gets its positions and carry a small per-frame record `{eye pose in game world ×2, tangents ×2}` with the
frame, the way `note_view_cut` rides the swap. Keep the previous frame's copy. **No new thread handoff:** the record goes
with the frame data that already crosses to the render thread.

**Why not do the separation in the motion pass's vertex shader** (exact per vertex): it would change shaders that DLSS,
SGSR2 and the runtime's frame synthesis use, and the per-draw matrices hold model-view combined, so the shader would still
need the same camera matrices. The per-pixel version touches nothing those features use.

**Tolerance.** Depth is quantised (D24 on the scene, far precision poor), and the camera record and the motion pass use the
same frames but different maths. A pixel counts as moving only if `|m_obj| > max(ε_px, k × |m_cam|)`. Start values:
ε = 0.75 eye pixels, k = 0.1. Both are env-tunable in the test switch and both are logged at start. Pixels at the cleared
depth (sky, far sea) get `m_obj = 0` by default: the sky's own drift is too slow to matter and its motion is the
background draws' estimate.

**Check that the separation is right (stage 1):** with nothing moving in view, the moving-pixel share during head turns,
stick turns and walking must stay near zero. A wrong camera delta shows up as the whole world "moving".

## 2. Which pixels move, and by how much on each filled refresh

- **Fraction per fill:** `f_k = k / (repeats + 1)`, k = 1..repeats, from the same `repeats` value `planned_repeats` uses:
  90 Hz / 30 fps gives ⅓ and ⅔; 90 / 45 gives ½; 120 / 30 gives ¼, ½, ¾. Where the cadence is uneven (72 Hz without frame
  synthesis at 30 fps: 2,3,2,3 refreshes) or in Auto, use time: `f = k × refresh period / game frame interval`
  (`g_gameFrameInterval`), clamped to [0, 0.9]. Log the fractions used.
- **A pixel moves when all of these hold:**
  - `|m_obj| > threshold` (section 1);
  - its motion pass packet had the previous frame's data (otherwise the pass already wrote 0);
  - view depth ≥ **near limit** (start: 2.0 m in the world). This keeps Link's body, sword, shield and hands out: they
    already have their own handling (the hands layer at low rates) and are where errors would show worst;
  - its 16×16 tile was not sent back to plain repeat by the limits in section 3.
- **How far:** moved pixel position = `p + f × m_obj(p)`. Only the object part moves. The camera part is left to the
  runtime, as today, because the fill is still submitted at the real frame's views (as `show_again` does).
- **The rest of the picture is copied exactly.** A pixel with no moving pixel landing on it and no own motion gets its
  source value unchanged, so the static world in a fill is bit-identical to today's repeat.

## 3. The area a moved object leaves behind (disocclusion), and the limits

Moving an object forward uncovers background behind its trailing edge. Frame N does not have that background (and frame
N-1 usually does not either: the object covered that strip in both).

**The warp (gather, per output pixel p)**, adapted from `kWarp`'s fixed-point search:
1. Start with q0 = p - f × m_obj(p). Repeat 3-4 times: q = p - f × m_obj(q). Take m_obj from the nearest depth in the 2×2
   around q, so outlines move with their object.
2. If the search converges (|q + f × m_obj(q) - p| < 0.5 px), output = colour(q). If two candidates converge (p's own and
   a mover's), take the **nearer depth**: the object in front wins.
3. If nothing converges, p is a **hole** (the uncovered strip). Search a few samples along the motion direction on both
   sides and take the colour of the **farther** sample whose own m_obj is about 0: the background is stretched over the
   strip. Holes are counted.

**Limits that keep large or near motion as a plain repeat** (decided per 16×16 tile and per eye, then shared):
- **Hole width:** if `f_max × |m_obj|` in a tile is more than H_max (start: 6 eye pixels, so about 9 px per game frame at
  ⅔), the tile goes plain. Stretched background over a wider strip looks worse than a held object.
- **Near:** any moving pixel nearer than the near limit makes its tile plain.
- **Large motion:** `|m_obj|` over 48 px per game frame (fast objects, cuts the motion pass did not catch) makes the tile plain.
- **Neighbours:** tiles next to a plain tile that hold similar motion (|Δm| < 2 px) also go plain, repeated twice. This
  keeps one object from tearing into a moved half and a held half.
- **Both eyes agree:** if one eye's moving-pixel count differs from the other's by more than 25 % (or one eye's tiles went
  plain where the other's moved), that fill is plain in **both** eyes. One eye moving and the other holding causes eye
  rivalry, which is worse than either alone.
- **Whole fill plain:** more than 20 % of pixels moving (a camera-separation fault, or a cut the pass missed), the frame's
  `eye_result.reset`, a menu or loading screen, or no camera record. All counted with a reason.

## 4. Where the work runs (the owner's rules: no new render-thread work, no new locks or waits)

There are three parts. **Two of them need the owner's OK as designed.** Each gets a measurement before any decision.

### 4a. The motion pass itself (the big one): needs the owner's OK

It redraws every depth-writing draw once more per eye. Measured cost so far:
- desktop, DLSS: packets 0.45-0.69 ms a frame over the drawing threads and render thread together, the pass recorded
  0.31 ms on the render thread (after the NGX call moved to the record worker);
- Quest 3 player's log (frame synthesis): packets 1.1-2.3 ms, recording 0.6-1.4 ms. The both-eyes multiview pass
  (docs/VR_DLSS.md, "Both eyes in one motion pass") has since cut the draw count on mobile GPUs;
- plus the motion store's copies of each draw's blocks and vertex bytes, done wherever the draw is recorded, so on the
  render thread for draws the render thread records;
- plus GPU time for a second depth-tested geometry pass per eye (not measured on Quest).

**Ways to keep it off the render thread, without new locks:**
1. **Record the pass on the record worker** through the existing `record::call` (`PodCommands::call`, opcode 30: a function
   run in order by whichever thread records the command buffer, already used for NGX, no drain). The render thread would
   only push one call that carries the frame's packet list, and the worker records the draws. This removes "pass recorded"
   from the render thread. Ownership: the packet vector moves into the call's bytes as a heap pointer, which the call
   frees. No lock and no wait are added: the call rides the stream that already exists.
2. **Packets and store copies** run on the drawing threads (the scene split's threads), except for draws the render thread
   records itself. That part cannot move without changing who records draws. It has to be measured and accepted, or not.

**What needs the owner's OK even with option 1:** the render-thread share of packet building and store copies (not zero),
and the GPU redraw. **Measure first (stage 0):** the motion pass forced on and its result unused, at Steady 30, against
today's build. Add timers that split the existing `[motion] cost` line into render-thread vs other-thread nanoseconds.

### 4b. The object-motion and warp passes (GPU): a few commands recorded, needs the owner's OK

Per eye: one compute or fullscreen pass to classify (m_obj and the tile stats), one small pass for the tile decisions, one
warp per fill fraction. That is about 10-15 Vulkan commands per eye per game frame. **Recommended place:** at the real
frame's eye copy (`record_eyes`), in the same command stream the record worker already replays, writing all the frame's
in-between pictures at once. This works because the in-between picture depends only on frame N's data, not on when the
fill is shown (the head is left to the runtime). The fills themselves then draw and copy nothing.

The render thread only *emits* those commands into the stream (microseconds, but strictly new render-thread work), so
this needs the owner's OK. A thread of its own would need a handoff (a queue with a wait), which also needs the owner's OK.
**Rejected:** drawing at fill time on the render thread, as `show_generated` did (the removed path: timing-sensitive, and it
blocks the render thread while it waits for each refresh).

**GPU timing:** recorded after the eye copy in the real frame's submission, the warp delays that frame's GPU completion
(the compositor waits for the whole submission). Measure with GPU timestamps around the passes (stage 2). If it makes
real frames late (the `slips` and late counters in the frame-timing log), move the passes into their own submission after
the real frame's `xrEndFrame`. That is a design change for the owner to approve.

### 4c. Getting the in-between pictures to the headset

`show_again` today submits the last released images of the eye swapchains. The in-between pictures need a home:
- **(A) Extra eye swapchains** (one per fill fraction per eye), written and released with the real frame. Each fill
  submits its projection layer from those swapchains. **No commands at fill time.** Memory: about 18 MB per image at
  Quest 3 eye size, times about 3 images per swapchain, times 2 fractions, times 2 eyes, so about 216 MB. Too much for a
  Quest, fine on PC.
- **(B) Private images** (2 fractions × 2 eyes, about 72 MB on Quest), copied into the eye swapchain at each fill. That is
  a copy recorded and submitted at fill time on the render thread (new render-thread work, needs the owner's OK), and the
  copy must finish before the refresh.
- **Recommended:** (A) on PC first. Decide Quest at stage 5 from memory measurements. Either way `xs().end_frame` gets an
  optional "which eye images" parameter, and today's call stays the same.

## 5. Regression guards

- **Off by default.** `WWHD_VR_MOVING_OBJECTS=1` turns it on, read once at start. No settings entry and no launcher entry
  until the owner says so. `=measure` runs the motion pass and the classification, logs them, and still sends plain repeats
  (stages 0 and 1).
- **Off means today's code path exactly:**
  - the switch is read before the renderer's device exists. Off, it sets nothing: `vr::framesynth::motion_flag()`,
    `motion_pass::on()`, `set_needed`, the twin pipelines and the store are all decided as today;
  - no extra swapchains are created and no new images allocated;
  - in `planned_repeats` the new branch is one `if (moving_on)` check, which falls through to today's
    `show_again(motion, i + 1)` unchanged;
  - `record_eyes` gets one `if` check, false when off;
  - code review checks that every new call site is behind that one flag.
- **Log state at start:** `[vr] moving objects at 30/45 fps: off (WWHD_VR_MOVING_OBJECTS=1 to try)`, or
  `on: near limit 2.0 m, hole limit 6 px, threshold 0.75 px / 10 %, tiles 16, pictures in extra swapchains`, or
  `measure only`, or `on but unavailable: <why>` (no dynamic rendering, the motion pass failed, swapchains failed).
- **Never while the runtime's own frame synthesis runs.** On a Quest at 72 or 80 Hz Capped 30 (drawn at 36 or 40), and at
  Capped 45 on 90 Hz with frame synthesis, the runtime makes the in-between frames, and two methods would compete.
  - **Which check:** `vr_framesynth::active()` is broader than frame synthesis. It is also true with depth-only submission
    (PC SteamVR with `vr.frameSynthesis` on or auto) and with head smoothing (`vr_vulkan.cpp` ~1187), so gating on it would
    switch this off on PC whenever depth is sent.
  - The exact check is the one `vr_vulkan.cpp` already computes: `xs().has_synthesis_swapchains() &&
    !vr_framesynth::held()` (frame synthesis in use), or `vr_framesynth::carrying()` for a per-frame check.
  - **Recommended:** off whenever frame synthesis is in use; depth-only does not count. The owner may still prefer
    `active()` as named in the task; the plan notes the difference so the choice is deliberate.
- **Fall back to a plain repeat on any doubt:** no motion result for either eye, a reset or cut, menu or loading, turn
  smoothing turning (today's turned copies win: stage 4 joins them), extra images not ready, a GPU pass skipped.
  Each fallback is counted with its reason.
- **The real frames are never touched.** Only fill refreshes change. The UI quad, wrist panels, laser and the hands layer
  are submitted exactly as `show_again` submits them now.

## 6. Proof counters (show in a headset log whether it ran, and on how much)

Every 10 s, while any fill happened:

```
[vr] moving objects (10 s): game frames 300; fills 600: 512 with moved pixels, 88 plain (reasons: 31 too many moving,
14 eyes disagree, 22 cut/reset, 9 menu, 12 turning, 0 synthesis, 0 no motion); moving pixels per moved fill: 1.8 % avg,
6.2 % max; tiles sent plain: 140 near, 63 hole limit, 5 large motion, 210 neighbours; hole pixels filled 0.21 % avg;
largest object motion 31 px/frame; fractions used 0.33 0.67; GPU passes 0.42 ms avg / 0.71 max a game frame (timestamps);
render thread recording 9 us avg; real frames late 2 (today's counter for comparison)
```

- At start: the state line (section 5).
- With `measure`: the same line, "would have moved" instead of "moved", and plain repeats sent.
- `[motion] cost` gains the render-thread vs drawing-thread split (stage 0).
- **Captures for evidence** (`WWHD_VR_MOVING_CAPTURE=1`, off by default, causes hitches): every 4 s, up to 4 sets of
  **whole eye images** as the headset gets them (real N, fill ⅓, fill ⅔, real N+1), plus the moving-pixel mask and the
  tile decisions as PNGs, with a text file of the fractions and counts. Written on a detached thread, as `capture_write`
  does now.
- **Debug view** (`WWHD_VR_MOVING_DEBUG=1`): fills tint moving pixels red, plain tiles blue, holes green. Headset only, for
  stage 1.

## 7. Causes and risks, most likely first

| # | Cause or risk | Likelihood | What it looks like | Mitigation in the plan | How a test shows it |
|---|---|---|---|---|---|
| 1 | **Smear and halos at moving edges** | high | a fringe or doubled outline round moving objects in fills; stretched background behind them | nearest-of-2×2 motion (outlines move with their object), nearest depth wins, hole fill from the far side, hole width limit, tiles plain with neighbours | captures: fill images against real N and N+1; owner: edges of gulls, boats, enemies |
| 2 | **Draws without motion**: batched/instanced, depth test off, transparent effects | high | batched grass or particles stay held (as today). A transparent effect in front of a mover is dragged with it (one motion per pixel: the mover's) | first two: no regression, they stay as today. Transparent: not solved; counted by test spots | owner checks the boat's wake, sea spray, smoke, fire, enemy death puffs, rupee sparkle, in fills |
| 3 | **Camera-separation error** | medium-high | the static world "moves" in fills during head moves, stick turns or walking: wobble or shimmer everywhere | threshold max(0.75 px, 10 % of camera motion), sky excluded, whole fill plain over 20 % moving | stage 1: moving share near 0 with nothing moving; debug tint |
| 4 | **Overshoot when objects stop or turn** (extrapolation) | medium | objects jitter forward and back at direction changes, swinging limbs wobble | fraction up to ⅔ only; option: move only where the object's motion matches the last frame's (needs the previous m_obj kept, one more image per eye) | owner: enemies stopping, NPCs turning, a swinging sign |
| 5 | **Cost: render-thread share of the motion pass** | medium (certain to exist; size unknown) | lower fps, or the render thread late for real frames | record via `record::call`; measure first (stage 0); owner's OK | stage 0: render-thread ms and % on time against base |
| 6 | **Cost: GPU** (second geometry pass, warp passes) | medium on Quest, low on PC | real frames late, more `slips` | timestamps; move the passes after `xrEndFrame` if needed | stage 2/5 GPU ms, late counters |
| 7 | **Quest / Adreno** | medium | the motion pass or both-eyes pass misbehaving, memory pressure from the extra pictures, RG16F sampling | PC first; Quest in stage 5 with memory and GPU numbers; option (B) for pictures | stage 5 |
| 8 | **Eyes disagree** | medium | one eye moves an object and the other holds it: rivalry, eye strain | shared plain decision, 25 % agreement rule | owner: either eye closed in turn |
| 9 | **The both-eyes motion pass's run-to-run variation** (open since 2026-11-03) | low | a few pixels' motion differs between runs | tolerance absorbs small motion; counters | none needed unless captures show it |
| 10 | **Clashes with other features** (turn smoothing, hands layer, Auto mode toggling, dynamic resolution) | low-medium | turned fills losing their moved objects, a hands layer mismatch, sizes wrong | plain while turning (stage 1-3); hands layer untouched; motion sampled by uv | stage 4 |
| 11 | **Steady 30 cannot be forced by the test env** (`WWHD_VR_FRAME_RATE` takes full, half, auto, unlocked; there is no `original`) | certain | a test run at the wrong rate | run with `vr.frameRate` set in settings.ini (back it up first), or the owner approves adding `original` to the test env | the log's frame rate line |

## 8. Stages (each: the change, the headset test, what failure looks like)

Every headset run: **ask before each launch** (it starts SteamVR). Restart SteamVR before an A/B. Back up the logs, pass
`WWHD_LOG_FILE=1`, and report average fps and % of refreshes on time. State sample sizes (seconds, frames, fills). Compare
against today's build with the switch off, in the same place in the game.

**Stage 0: measure the motion pass (`WWHD_VR_MOVING_OBJECTS=measure`, classification not built yet).**
- Change: the switch sets the motion flag before the device. `set_needed` stays true while fills run. The pass is recorded
  via `record::call`. Render-thread timers are split out. The picture is unchanged (plain repeats).
- Test: PC headset at Steady 30, 2 minutes on Outset (Link walking, gulls, the sea), then the same with the switch off.
- Failure: render-thread time a game frame up by more than the owner's limit (owner to set; 0.5 ms suggested), more late
  real frames, any visible change, or the `[motion]` lines missing (it did not run).
- **Owner decision gate: go on or stop here.**

**Stage 1: object motion found, shown as a tint (`measure` + `WWHD_VR_MOVING_DEBUG=1`).**
- Change: the camera record carried with the frame, the classification pass, the tile decisions, counters. Fills still
  plain except for the debug tint.
- Test: (a) stand still, nothing moving: tint about 0; (b) head turns, stick turns, walking with nothing moving: moving share
  under 0.5 % average; (c) gulls, a boat and enemies in view: they tint and the world does not.
- Failure: the static world tinted while the camera moves (camera-separation wrong; fix before stage 2), movers not tinted,
  or the eyes' tints differing.

**Stage 2: move distant objects, small motion only (`WWHD_VR_MOVING_OBJECTS=1`, hole limit 2 px, near limit 5 m).**
- Change: the warp into the in-between pictures (option A swapchains on PC), fills submitting them, GPU timestamps.
- Test: owner A/B against off: distant gulls, the King of Red Lions' sail at a distance, Moblins on a far island, the sea
  and sky while turning the head. Captures of 4 sets.
- Failure: any halo or doubled edge the owner sees, static things moving, eye rivalry, more late real frames, the owner
  not seeing less ghosting on distant movers. If it fails on halos, stop and review before stage 3.

**Stage 3: hole fill and the full limits (hole limit 6 px, near limit 2 m).**
- Test: NPCs walking at mid distance on Windfall, enemies charging, a boat passing; captures.
- Failure: a background streak behind movers, objects tearing at tile edges, effects dragged badly (cause 2).

**Stage 4: the other rates and turn smoothing.**
- Change: Capped 45 (½), 120 Hz (¼ ½ ¾), Auto while low, and moved pictures submitted with turn smoothing's turned views
  (the image holds the object motion, the pose holds the turn: they combine).
- Test: each rate on its headset mode; stick turning past moving things.
- Failure: anything from stages 2-3 at these rates, or turn smoothing worse than today.

**Stage 5: Quest 3 standalone.**
- Change: build for Quest. Decide option A or B from memory use. Check GPU time per pass on Adreno.
- Test: Quest 3 at 90 Hz Steady 30 (frame synthesis held there, so this feature may run). Confirm it stays off at 72 Hz with
  frame synthesis.
- Failure: memory warnings or crashes, GPU time pushing real frames late, any stage 2-3 artefact.

**Stage 6: the default, the owner's call only.** A settings entry or a default change only on the owner's word, with the
settings-format bump rules.

## Separate small fix: no refresh filling while the runtime's frame synthesis is in use

**Built 2026-11-14 (untested in a headset):** `planned_repeats` returns when `vr_framesynth::carrying()` (the frame
just shown carried both eyes' motion vectors; set in `record_eyes` before `planned_repeats` runs). Menus and loading screens
carry none, so their fills go on. The log, on each change: `[vr] refresh filling: paused (the runtime's frame synthesis
makes the in-between frames)` / `resumed (no frame synthesis)`, with counts since the start. **Found while reading for
this plan:** `planned_repeats` fills refreshes whenever `low_rate_repeats` says so, and
does not check frame synthesis:
- On a Quest at 72 Hz, Capped 30 is drawn at 36 with frame synthesis (`vr_vulkan.cpp` ~1175-1189: not held at 72 Hz).
  `low_rate_repeats(72, 36)` = `lround(72/36) - 1` = 1, so each real frame is followed by a `show_again` frame.
- The same happens at Capped 45 on 90 Hz with frame synthesis on.
- Those plain repeats carry no synthesis data, and they take the refresh the runtime would have made a synthesized frame
  for. The runtime paces `xrWaitFrame` at half rate while frames carry synthesis data, and an extra frame per real frame
  works against that pacing. Expected effect: synthesized in-betweens replaced by plain repeats, or the game's rate pushed
  down. Not seen in a log yet. The turn smoothing line's `plain fills` count in a Quest 72 Hz log would show it.

**Fix:** at the top of `planned_repeats`, return when frame synthesis is in use:
`xs().has_synthesis_swapchains() && !vr_framesynth::held()` (or `vr_framesynth::carrying()`). **Not `vr_framesynth::active()`
as such:** that is also true for depth-only submission on PC and for head smoothing, and would stop refresh filling on PC
SteamVR when depth is sent, bringing back Motion Smoothing's on-off switching. Log once on each change:
`[vr] refresh filling: paused (the runtime's frame synthesis makes the in-between frames)` / `resumed`.

**Check:** a Quest 3 log at 72 Hz Capped 30 with frame synthesis shows the pause line and 0 plain fills. The owner judges
smoothness against the last build. On PC, `plain fills` is unchanged at Steady 30.

## Files the build would touch (for scope, when the owner says build)

- `runtime/src/gfx/vulkan/vr_vulkan.cpp`: the switch and start line, the `planned_repeats` branch, the `record_eyes` hook,
  the camera record kept per frame, the separate fix.
- New `runtime/src/gfx/vulkan/vr_moving.h/.cpp`: the classification, tile and warp passes, counters, captures (GPL-3.0+
  with the VR credit header, as `vr_genframe.cpp`).
- `runtime/src/gfx/vulkan/motion_pass.cpp`: recording via `record::call`, render-thread timer split, needed while the
  feature wants it.
- The OpenXR session (`openxr_session.cpp` / `xs()`): the motion flag at start when the switch is on, the extra swapchains,
  `end_frame` taking alternate eye images.
- `runtime/tools/vr_test.cpp`: unit tests for the camera separation (known camera moves give m_obj = 0, a known object move
  gives its motion), the fractions per rate, the tile limits. Written with the code, run only when the owner says.
- Docs: this file, VR_FRAME_SYNTHESIS.md (the limits and the separate fix), VR_DLSS.md (the motion pass's new user).

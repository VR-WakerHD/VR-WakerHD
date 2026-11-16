# Frame interpolation: implementation (2026-11-12)

Changes 1 and 2 of [FRAME_INTERPOLATION_AUDIT.md](FRAME_INTERPOLATION_AUDIT.md), section G. Built on Windows; **not run
in a headset**, and no automated test was run (the pacing test target compiles).

## Change 1: the display-time blend

**Status:** implemented, **off by default** after its first headset run (2026-11-12, PC VR at Capped 90: "fps seems
worse"); `WWHD_VR_DISPLAY_BLEND=1` turns it on. The run's log: 83-89 fps, and the blend moved 54-109 of every ~560
in-between frames by up to 0.67 of a step (two frames). The predicted display time it reads (`display_time`: the last
prediction plus the swaps still queued) jumps by a frame whenever the queue's depth changes; the head pose's
reprojection hides that, the world does not. A steadier time source (the render thread's own record of missed
refreshes) would be needed before it can be on.

**What it fixes.** Frame interpolation draws each frame at a fraction between two logic steps fixed by the pacer's
schedule (`interp_pacing.h` `pass_fraction`: 1/3, 2/3 and 1 at 90 fps). That is right only while every frame reaches
the headset on its planned refresh. A frame that misses is shown a refresh late with the world where it should have
been a refresh earlier, and the frames after it carry the lag, so the world moves unevenly while the reprojected head
stays smooth: the jitter PC VR players reported with interpolation on.

**How.** `interp.cpp` `pass_t` now returns `display_blend(planned_pass_t())`:

- **The frame's display time** is the camera's own prediction (`vr::game::frame_display_time`, which returns
  `display_time()`: the render thread's last predicted display time plus the frames still queued, times the refreshes
  each drawn frame is shown for). The head pose of the same frame is located at that time, so the world and the head
  agree.
- **A timeline** keeps the display time at which the step being blended from is drawn exactly. It advances one logic
  step (`paced_step`, two vsyncs of the game's clock) per step, and follows each pass's own estimate
  (display time minus planned fraction times a step) by 5 % a pass, so a steady offset (a runtime's latency) never
  shows and a sudden lateness does. A jump of more than two steps (a pause, a load, the session's start) re-seeds it.
- **The fraction** is `(display time - exact time of the step before) / step`, clamped to [0, 1]: between the two
  steps being blended, never extrapolated.
- **Applied only when it matters:** when the frame is at least half a frame off its planned fraction; smaller
  differences (noise in predicted times) keep the plan, so a frame on schedule draws exactly as before.
- **Unchanged:** the record pass (it draws and records the exact step the next step blends from) and exact steps stay
  at 1; outside VR, and without interpolation, nothing changes. Computed once a pass (every blend of a pass sees the
  same fraction).
- **The log:** every 10 s with any correction, `[interp] display-time blend (10 s): N of M in-between frames drawn at
  their display time, not their plan (at most F of a step off)`.

**Files:** `runtime/src/interp.cpp` (`planned_pass_t`, `display_blend`, `pass_t`), `runtime/src/vr/vr_game.cpp` and
`vr_game.h` (`frame_display_time`).

**Limits.** A frame that misses after it was drawn (its GPU work ran long) is still shown late; what changes is that
the frames after it are drawn for their real display times instead of carrying the lag. It cannot blend past the newest
logic step (no extrapolation), so a frame later than a whole step shows that step exactly.

## Change 2: a steadier Auto Mode

**Status:** implemented; needs a headset test.

Auto (`vr.frameRate=auto`) chooses between the headset's full rate and half (90 or 45 fps at 90 Hz) from the share of
in-between frames drawn. Each change of rate is a hitch, and on a computer near its limit it changed again and again.
It already judged gameplay only (`auto_rate_settle`: not the title, loads or cutscenes without the gameplay camera).

| | Before | Now |
|---|---|---|
| Window judged at the full rate | 45 steps (1.5 s) | 90 steps (3 s): one hitch no longer drops it |
| Drop to half | under 90 % drawn | unchanged |
| Wait before trying full again | 300 steps (10 s) | 600 steps (20 s) |
| After a failed try, doubling to | 1,200 steps (40 s) | 2,400 steps (80 s) |
| Keep full after a try | 98 % | unchanged |
| Streamed headsets (Streaming optimal) | 6 s window, 40-160 s waits | unchanged |

**Files:** `runtime/src/interp_pacing.h` (`kAutoWindow`, `kAutoBackoff`, `kAutoBackoffMax`, `AutoRate::backoff`),
`docs/VR_FRAME_RATE.md`.

## Change 3, stage 1: turn smoothing

**Status:** implemented, **off by default** (`vr.turnSmoothing`: F1 > VR "Turn smoothing", the launcher's Comfort
section). Its headset run at Capped 45 (2026-11-12, "its worse now"): turning at 118 degrees/s, 127 frames shown again
in 10 s (of about 450 refreshes between game frames) and 147 missed refreshes (10-36 in the other windows). Slipped in
between the game's frames, a frame shown again took the slot the next game frame wanted and delayed it a refresh, so the
turn mixed turned, unturned and late frames. Doing it right needs the render thread to own the refresh cadence (plan
which refreshes get a game frame and which a frame shown again), not to slip frames in.

**Rebuilt the same day, planned (on by default again):** right after each game frame (`backend.cpp` swap, after
`vrvk::frame_end`), `planned_repeats` shows the frame again for the refreshes before the next one (round(refresh / frame
rate) - 1: one at 45 fps on 90 Hz, two at 30), each turned on to its own display time. Each waits in `xrWaitFrame` for the
next refresh in order, before the next game frame asks for one, so no game frame's slot is taken. Only while the view
turns faster than 3 degrees a second; at the full rate none. The log: `[vr] turn smoothing (10 s): N frames shown again
turned on after M game frames`. Needs a headset test. Stage 2 (movement, with depth) followed the same day: below.

**What it fixes.** At Steady 30 or 45 fps the headset shows each frame for three or two refreshes. Between frames the
runtime turns the last image with the head only; the view's own turn (the stick's smooth turn, the view following Link
round) stepped 30 or 45 times a second while the head moved at the refresh rate.

**How.**

- **The turn rate** (game thread, `vr_game.cpp` `CameraOverride`): the mapping's heading's change since the last camera
  over the time between their display times (`vr::set_view_turn_rate`). First person in play only (third person and
  the boat's views draw the game's own camera). A change of 20 degrees or more at once (a snap turn, a recentre, a cut)
  counts as no turn: never smoothed.
- **Shown again, turned** (render thread, `vr_vulkan.cpp` `headset_frame`): while the view turns faster than 3 degrees a
  second and the game's frames come more than 1.6 refreshes apart, the last frame goes out again half a refresh after
  the last one went out, its views turned about the head by `-rate x (this frame's display time - its own)`: an image
  drawn at heading h0 and shown at h0 + w dt is the same as one submitted turned by -w dt in the tracking space, since
  the view's heading in the game is the mapping's heading plus the head's tracked yaw. The runtime then reprojects it
  to the head as always. Nothing is drawn or copied.
- **Never in a game frame's slot:** not when the next game frame (the last one plus the game's frame interval) is due
  within a refresh of it. At Steady 30 on 90 Hz that is two frames shown again per game frame, at 45 fps one.
- **Not turning, nothing changes.** At the full rate (interpolation drawing every refresh) it never runs.
- **The log:** `[vr] turn smoothing (10 s): N frames shown again turned on (the view turning at D degrees/s)`.

**Files:** `runtime/src/vr/vr_config.h` (`turnSmoothing`), `vr.h`/`vr.cpp` (`set_view_turn_rate`, `view_turn_rate`),
`vr_game.cpp` (the measurement), `gfx/vulkan/vr_vulkan.cpp` (`turn_repeat_wanted`, `headset_frame_due`,
`headset_frame`), `overlay/overlay.cpp`, `tools/vr_launcher/vr_launcher.cpp`.

**Limits.** The rate is the last camera's, so starting or stopping a turn the frames shown again run on by at most one
frame's turn (at 120 degrees a second and 30 fps, 2.7 degrees) before the next game frame corrects it. Things that turn
with the view (the hands, the held sword and shield) turn with it in the frames shown again, as they do now with the
head. Linux and Windows PC VR, streamed runtimes and the Quest standalone build all take the extra frames the same way
(an ordinary OpenXR frame); a streamed runtime encodes them as frames of their own.

## Change 3, stage 2: movement smoothing

**Status:** implemented, **off by default** (`vr.movementSmoothing`: F1 > VR "Movement smoothing", the launcher's
Comfort section; applies at the next start). Its first headset runs (2026-11-12): at Capped 90 the depth it turns on
flickered on and off between frames (fixed: `vr_framesynth.cpp` notes each eye's camera at least once a frame); at 45
and 30 fps "the world is ghosting", most likely the runtime reprojecting every frame by depth never checked in a headset
(its values or range). Its movement threshold went from 0.1 to 0.5 m/s (standing still measured 0.11).

- **The velocity** (game thread, with the turn rate): where the mapping puts the head's tracked position now against
  where the last camera's mapping put it, so the head's own movement is left out and the room-scale step (anchor and
  origin moved together) cancels; turned into the tracking space and per second (`vr::set_view_velocity`). None while
  Link rides the boat (its deck moves with the view and would slide), and none for a jump of more than 40 game units in
  a frame (a door, a warp, a recentre).
- **Shown again, moved** (`headset_frame`): frames are shown again while the view turns or moves faster than 0.1 m/s,
  their views moved back by `velocity x dt` (an image drawn at p0 shown at p0 + v dt is one submitted from v dt
  behind), after the turn.
- **Depth:** a runtime moves an image by its depth; without it, as if everything were far away. Movement smoothing
  enables depth alone (`XR_KHR_composition_layer_depth`) where nothing else sends depth and the runtime offers it
  (`openxr_session.cpp`, the frame synthesis choice), and a frame shown again carries the last frame's depth again
  (`reuse_last_depth`: the depth swapchains' last released images, the ones that frame wrote). Depth is still left out
  while the UI layer shows text or menus (the earlier VDXR rule).
- **The log:** `[vr] turn and movement smoothing (10 s): N frames shown again moved on (... degrees/s, ... m/s; depth
  sent | not offered)`.

**Limits (known before building it):** things moving with the view (the hands, held items, Link's body) are in the
same image and move with the world in the frames shown again, a step behind until the next game frame; near edges
show stretching where depth reveals what was not drawn; water, particles and transparent effects have no depth of
their own and move with what is behind them; starting and stopping, the frames shown again run on by up to a frame's
movement. Turning depth on also changes the runtime's normal reprojection of every frame (SteamVR then reprojects head
movement positionally): a change of its own to watch for.

## Generated frames (stages 1-4)

**Status:** the "Generated frames" setting (`vr.frameGeneration`) was **removed** on 2026-11-13 (the user: "it doesn't
work anyway"; its headset runs of 2026-11-12 ghosted and were worse than off). A saved `vr.frameGeneration` is ignored.
The generated-frame pass itself (`show_generated`, `vr_genframe`) stays: Head movement smoothing (below) and Turn
smoothing's near-field hold (`WWHD_VR_TURN_NEAR_HOLD=1`) use it. What follows describes it as it was built

**What it does.** At a frame rate under the headset's (Steady 30, 45 fps), while the view turns or moves, each refresh
between the game's frames gets a frame drawn on the GPU from the last real one: both eyes' pictures reprojected by their
scene depth to the head where the runtime predicts it for that refresh and the view's own turn and movement since. Turning
(the stick, the view following Link) and moving (running, sailing) are then as smooth as the headset's refresh; things
that move by themselves (enemies, Link's animation, the sea) still step at the game's rate.

**Hands layer (2026-11-13).** Generated frames now send the hands layer too: with Steady hands on, Link's hands are
drawn only in that layer, so they were missing from every generated frame.

**Head movement smoothing (2026-11-13, `vr.headSmoothing`, its own setting next to Smooth Camera while Turning since
2026-11-14 (it was under Streaming optimal), off by default).**
Players at 45 and 36 fps, on wired headsets (PS VR2 through SteamVR) and Virtual Desktop, saw near things (the arms, the
sword, close walls) step back and forth as they looked around, while stick turns were smooth; a real 90 was clean.
Looking around moves the eyes a few centimetres (the head pivots at the neck) and the runtime's reprojection of a
repeated refresh corrects only the turn (Meta, Google: rotation-only reprojection leaves positional judder on near
objects); Virtual Desktop's timewarp and SSW do not use the game's depth for it. With the setting on, every refresh
between the game's frames with no stick turn and no movement of the view (`head_smoothing_wanted`) gets the last frame
warped by the eyes' movement alone (`show_generated(false, true)`, `headOnly`): the target camera is the real frame's
orientation at the eye's position now, the frame submitted with that pose, and the runtime turns it to the head as for
any frame. It is not the removed Generated frames setting. Any headset, wired or streamed; nothing at a
real 90. The log's `[vr] generated frames (10 s)` line counts these frames and how far the eyes moved (mm).
With Depth for Virtual Desktop (or any depth-only submission) the warped frames resend the real frame's depth
(`reuse_last_depth`, as movement smoothing's repeats): VDXR places the layers by depth, and depth on the real frames
only would have switched every other refresh.

- **Stage 1, the inputs** (`vr_framesynth.cpp` `record`, `last_depth`): each eye's scene depth target (the one most of
  its camera draws drew into; a multiview eye 1 takes eye 0's layer twin) and the camera projection's `p[10]`, `p[11]` and
  depth range, kept each frame. The depth noting runs whenever generated frames are on (`set_active`), whether or not
  depth goes to the runtime. The eye pictures are the TV and right-eye scans the eye copy reads; the warp runs right after
  the real frame, before the next frame draws into them, so no copy is kept.
- **Stage 2, the warp** (`vr_genframe.cpp`): per eye a full-screen pass into a freshly acquired swapchain image. For each
  pixel of the in-between picture a few fixed-point steps find the real picture's pixel that lands there (the desktop's
  `reproj.cpp` kWarp search, MPL-2.0), with the eye's own asymmetric field of view, the in-between camera a full 6DoF
  move (its axes and position in the real eye camera's space), and depth as distance by the projection's own terms
  (`b / (ndc + a)`, either depth range).
- **Stage 3, the delivery** (`vr_vulkan.cpp` `show_generated`, from `planned_repeats`): the refreshes turn smoothing plans
  right after each game frame (one at 45 fps on 90 Hz, two at 30) are generated instead of turned copies, in the headset
  test loop's sequence (begin, locate, acquire, draw, submit, release and end under the queue's lock), submitted at the
  head's own pose (the view's turn and move are drawn in). Without depth or a picture that frame, a turned copy as before.
- **Stage 4, the edges:** a pixel the search lands on whose own move does not bring it there is the background a near
  object uncovers (never drawn): the farther of the two pixels either side along the move. Out of the picture: its edge.
- **The log:** `[vr] turn smoothing (10 s): N frames between M game frames (... G generated, F generations failed ...)`.
- **CPU:** none on the game thread; on the render thread an OpenXR frame and two passes recorded per generated frame. Only
  below the headset's rate and only while turning or moving faster than 0.3 m/s or 3 degrees a second: at 90 fps nothing.

**Known limits:** held items and hands were reprojected with the world (a step behind until the next real frame:
fixed by the near field below); the
per-pixel search can tear thin near parts on fast moves (the desktop's warped mesh is the next step if it shows);
transparent effects move with what is behind them.

### Smooth turning with the frames between (2026-11-12)

The stick's smooth turn and the frames shown between the game's (generated frames, or turn smoothing's turned copies)
now agree in two ways; checked headset-free in the maths test (game camera against warp, refresh by refresh):

- **The pivot.** The game turns the mapping's heading, which swings the head's sideways offset from the room-scale origin
  (up to `vr.maxHeadOffset`, 1 m) about that origin. Generated frames turned about the head itself: standing 0.8 m from
  the origin, a 5 degree turn put a point 1 m away 1 to 1.6 % of the picture off. They now turn about the game's pivot
  (`vr::turn_pivot`). The movement measured for them leaves the turn out (it read the swing as movement before).
- **The stick pushed and let go.** Each game frame turned by the stick's rate over the whole interval since the last,
  while the frames between carried the last rate on. Let go, the frames between ran on two refreshes and the next game
  frame jumped back (2.5 degrees at 120 degrees/s, Steady 30); pushed, the view held and then jumped three refreshes at
  once (3.8 degrees). Now each game frame turns by the last rate for the time the frames between showed and by the new
  rate only for its own refresh: every refresh steps forward by one refresh's turn or holds. Let go, the view stops up to
  two refreshes later (22 ms at Steady 30, 11 ms at Capped 45) instead of bouncing. Only in Capped 45 and Steady 30 with
  Generated frames or Turn smoothing on; Capped 90, Auto and Unlocked are unchanged.

### The near field (2026-11-12)

What lies within 1 m of the eye (the hands, the sword and shield, held items) moves with you, not with the world. The
warp now draws it with the head's own move since the real frame only, not the stick's turn or the walk, in front of
the world; the world is searched through it as if it lay 10 m back, and the world it uncovered (never drawn) is filled
from the nearest world pixel beside it, sideways first, 2 to 128 pixels out. `WWHD_VR_GENFRAME_NEAR=metres` changes the
distance (0: off, every pixel warped as the world); `WWHD_VR_GENFRAME_DEBUG=1` shows the held near field green.
Checked headset-free: the shader compiles with glslang, and a CPU copy of it on a made-up picture (a hand at 0.5 m on a
wall at 5 m, a 2 degree stick turn) kept every hand pixel in place, moved 99.6 % of the wall exactly (the rest at the
hand's 2-pixel border), and filled none of the uncovered wall with hand. Limits: a wall or object of the world within
1 m is held too (it steps with the game's frames during a turn); the filled strip beside a hand repeats the wall next
to it; the hands' own movement between the game's frames is not drawn (that needs the controllers' poses).

### The field of view, put back (2026-11-12, headset run)

The warp's field of view taken from the camera matrix noted for depth was wrong: that matrix is the game's own, before
each eye's frustum is applied (`stereo_projection.h` `project_eye`), so it read 1.386 wide for 1 high and symmetric,
against the eye's 1.049 and its offset centre. Every generated frame turned by about three quarters of the real turn and
was skewed: the headset run was "still blurry and still ghosting". Back to the views' field of view (the one the eye's
projection is built from). The same run showed the other fixes working: real frames drawn 0 ms ahead of their refresh
on average (0 to 22 ms before), and the real and generated frames stepping alike (0.72 and 0.69 degrees a refresh).

### Every frame timed to the refresh it lands on (2026-11-12)

Captured headset frames (`WWHD_VR_GENFRAME_CAPTURE=1`, left eye as submitted, PNGs in `captures/genframe/`) showed the
generated frames correct (a 2.17 degree turn moved the picture 45 px, the real frames' own 2.2 degrees 42-44 px; clean,
no doubling), and the red tint showed they reach the eyes. What the log had shown all along: real frames are drawn for
a predicted refresh and land on another in about a third of frames (their lead's spread 6.5 ms, up to 22-33 ms either
way at Steady 30); in a stick turn that is a refresh's turn (0.7 degrees at 65 degrees/s) back or ahead, and the
generated frames made from them carried it on: the view stepped back or stood still, the doubling. Now each real
frame is submitted turned by the stick's turn from the moment it was drawn for to the refresh it lands on (as frames
shown again, the runtime turning it as for the head), and generated frames measure their turn from the moment drawn
for. Capped 45 and Steady 30 with Generated frames or Turn smoothing; `WWHD_VR_TIME_CORRECT=0` off. The log's
`[vr] steady stick turn` line measures it: each refresh's turn against the stick's, smooth is 1.00, 0 % spread, none off.

## Runtime compatibility

Both changes are in the game's own pacing and need nothing from the runtime: Windows and Linux PC VR, Virtual Desktop,
SteamVR, Link. The Quest standalone build draws Steady 30 FPS by default (no interpolation), where change 1 does nothing.

## Headset test

1. PC VR, Frame rate Capped 90 FPS (interpolation on), a busy scene (Windfall, or sailing): compare with
   `WWHD_VR_DISPLAY_BLEND=0` in the environment; watch the world while turning with the stick and while running.
2. Frame rate Auto in the same scene: the log's `[interp] VR frame rate` changes should be rarer.
3. Send `captures\wwhd.log`: the display-time blend lines say how often frames were corrected.

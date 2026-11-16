# VR refresh rate and frame rate

Two settings (`settings.ini`; launcher Graphics, F1 > VR > "Graphics and frame rate"):

- **Refresh rate** (`vr.refreshRate`): 60, 72, 80, 90 or 120 Hz (90 by default). Anything else read from a file (the
  "runtime's choice" 0 of older formats, 144, 61) becomes 90; a rate a runtime reports (89.9, 119.88, 59.94) counts
  as the menu's within 0.6 Hz (`vr_config.h` `supported_refresh_rate`).
- **Frame rate** (`vr.frameRate`), by meaning: `full` (capped at the refresh rate; the default before the VR fork made `auto` the default), `half` (capped at
  half of it), `unlocked` or `auto`. The menu names them from the refresh rate (`frame_rate_choices`, the one place):

| Refresh rate | Unlocked | Full | Half | Auto (default) |
|---|---|---|---|---|
| 60 Hz | Unlocked FPS | Capped 60 FPS | Capped 30 FPS | Auto Mode |
| 72 Hz | Unlocked FPS | Capped 72 FPS (drawn at 60) | Capped 36 FPS (drawn at 30) | Auto Mode |
| 80 Hz | Unlocked FPS | Capped 80 FPS (drawn at 60) | Capped 40 FPS | Auto Mode |
| 90 Hz | Unlocked FPS | Capped 90 FPS | Capped 45 FPS | Auto Mode |
| 120 Hz | (not offered) | (not offered) | Capped 60 FPS | Auto Mode (default) |

A new refresh rate selects the default frame rate, Auto Mode (VR fork). 120 Hz offers only Capped 60 FPS and Auto Mode for now. Older values:
`45` and `30` read as half, format 8's `60`, `90` and `120` as full, and `unlocked` from before format 8 (its default
then) as full (settings format 9).

**What is drawn** (`interp_pacing.h` `vr_mode_cap`, `vr_drawable_fps`, `cap_fps_vr_mode`): the caps follow the rate
the headset actually runs, as the runtime reports it, not the one chosen. The pacer has frame schedules for 120, 90,
60, 45, 40 and 36 fps and 30 (frame interpolation off); a cap without one is drawn at the fastest one below it (72 and
80 fps at 60), and the log says so (`[interp] VR frame rate: capped at 72 fps (all of the headset's 72 Hz),
drawn at 60 fps (no schedule for its own rate)`). Schedules for 72 and 80 fps (2.4 and 2.67 frames a logic step) are
not written yet.

**36 fps** (half of a 72 Hz headset, such as the Steam Frame's, added 2026-11-03; `interp_pacing.h` `is_fps36`): 5 logic
steps over 6 frames, each frame 5/6 of a step after the last and shown for two refreshes. It is built like 40 fps:
- One step in five gets an in-between frame: its logic pass is drawn at 1/6 and its record pass exactly.
- The next four steps are blended record passes at 5/6, 4/6, 3/6 and 2/6 (`Fractions40` with a lag of 1/6).
- The virtual vsync's step slots (granule 20 ticks, steps of 24) say which step gets the in-between frame.
- The game's clock is locked to the headset's over five steps (`vr_vulkan.cpp` `lock_to_headset`): 12 refreshes of
  72 Hz, the vsync 1/60 s. 80 Hz locks the same way over three steps (8 refreshes, for its 40 fps).

Before this, 36 fps had no schedule, so the game drew 30 fps on 72 Hz: 2.4 refreshes a frame, unevenly. Unlocked draws as many frames as the headset shows evenly and the computer draws (the
desktop's interpolation rate, capped: `cap_fps_vr`).

**Applied to the headset** (`vr_vulkan.cpp` `request_refresh_rate`, `openxr_session.cpp`): at the session's start and
whenever the setting changes during play, the game asks the runtime for exactly that rate when it lets the game
choose (`XR_FB_display_refresh_rate`: Quest, Android XR) and offers it; a rate not offered is not swapped for another
(logged). SteamVR does not let the game choose: its rate is set in SteamVR's video settings. The settings overlay
shows the rate the headset runs and the frames drawn, in orange when that is not the rate chosen.

**Detect Headset** (launcher, General): after finding the headset it runs the game program with
`--vr-refresh-probe` (`vr/vr_probe.cpp`). That starts a session of its own for about a second (30 frames with
nothing drawn; the headset shows the runtime's empty scene), reads the rate it runs (the extension's, else the frame
period xrWaitFrame predicts, as on SteamVR) and the rates offered, prints one line and exits, without a log file. The
launcher then selects that rate and the frame rate capped at it (`apply_detected_refresh`: "Capped 90 FPS" at 90 Hz),
and saves at once when nothing else was waiting to be saved. A rate the menu has no choice for (144 Hz) selects the
fastest of the menu's rates the runtime offers (asked for at the game's start), or changes nothing, and says so. A
failed detection changes nothing. Not on Android yet (the game asks for the chosen rate when it starts), and only with
the game program itself (`wwhd`) next to the launcher: the release's program starts the game.

**Auto** (`interp_pacing.h` `auto_rate_step`): capped at the refresh rate while its frames fit, else at half: it counts
the in-between frames drawn at the full rate of those that could be; under 90 % over 3 s it goes to half. From half it
tries the full rate again after 20 s (for 2 s, after 0.5 s for the pacer's estimates) and keeps it with 98 %; a failed
try doubles the wait, up to 80 s (2026-11-12: 1.5 s, 10 s and 40 s before; fewer changes of rate, each a hitch:
docs/FRAME_INTERPOLATION_IMPLEMENTATION.md). (VR fork: it was 97 %, 99 %, 10 s and a minute, and sat at half in busy scenes; then
85 %, 90 %, 5 s and 20 s, which in the headset sat at an uneven 60 fps too often; 2026-10-31: these, between the two.) Test runs: `WWHD_VR_FRAME_RATE=full` (or `half`, `auto`, `unlocked`) sets the frame rate for one
start without changing the saved `vr.frameRate`. The pacer's estimates of the full rate are kept while at half and restored for
the try. The log says each change and why.

The game logic runs at 30 steps a second (29.97, two 59.94 Hz vsyncs) in every mode. No mode changes how fast the game
runs, only how many pictures of it are drawn.

## Where the 30 fps comes from

There is no 30 fps cap in the renderer or the VR code. 30 fps is the game's own rate: its main loop (the per-frame
function, `interp.cpp` hook of 0203593C) runs one logic step and draws one frame per two vsyncs
(`GX2SetSwapInterval(2)`). Everything in the game counts in those steps: movement, physics, animations, timers, AI,
collisions, item use.

Frame interpolation (`interp.cpp`, `interp_fx.cpp`, `interp_pacing.h`) adds frames without running more logic.
- After a logic pass come hold passes, which run only the game's drawing. The game's children of the per-frame
  function are skipped on them (`hold_back`), so no logic, collision, damage or timer runs twice.
- Models, the camera, effects, the sea, cloth and attachments are drawn blended between the last step and the new one
  at the pass's fraction (`pass_t`). The blends are written for the draw and put back after it, never into the
  game's state.
- The last hold pass (the record pass) draws the new step exactly and keeps it as the "before" of the next step's
  blends.

VR switches interpolation on for the session (`vr.autoFrameRate`). The headset's refresh rate then caps the drawn rate
to what it shows evenly (`pacing::cap_fps_vr`). The VR systems already work per pass:
- the head pose and the controllers are read for every frame;
- the arms are re-aimed every frame (`frame_correct`);
- the views are tagged with the game time each frame shows (`store_views`);
- the sword's collision uses the step's pose (`link_executed`), once per logic step.

## The modes

**Unlocked** is the behaviour before this setting existed: the most frames the headset shows evenly and the computer
draws in time (`cap_fps_vr`). 90 fps needs every frame within 11.1 ms; when three a step do not fit, the pacer draws two
(60 fps), shown for 1 and 2 refreshes in turn.

**30 fps** turns frame interpolation off while the VR session runs (`interp_on`: `vr_original`). The saved desktop
setting is unchanged.

**45 fps** (new) draws 2 logic steps over 3 frames, each 2/3 of a step after the last. On a 90 Hz headset every frame
is shown for two refreshes, and the game moves evenly from frame to frame (60 fps on 90 Hz moves unevenly). It follows
the 40 fps mode, which has 3 steps over 4 frames:
- **The step slots** on the virtual vsync use a 16-tick granule (`vsync_granule_ticks`; steps are 24 ticks), so the
  steps get 2 and 1 frames in turn (`gx2::step_frames`).
- **A step with 2 frames:** its logic pass is drawn at 1/3 of the step and its record pass exactly
  (`pacing::pass_fraction_45`).
- **A step with 1 frame:** its logic pass is a *blended record pass*. It is drawn blended at 2/3, and its exact state
  is kept as the next step's "before" at the same time (`g_blend_record`). This is the one new kind of pass.
  - Each history that a record pass keeps blends from its old record, then keeps the new exact one with the next
    record generation (`record_stamp`). That covers models (viewCalc), the camera, cloth and line packets, material
    animations, the sea grid and attachments.
  - The generation goes up when the pass ends.
  - The histories kept at logic time (particles, sway, waves, wood) need nothing new: they count generations, which
    still go up once a step.
- **The frames** show the game at 1/3, 1, 1 2/3, 2 1/3, 3, ... steps.
- **A step that follows a dropped record pass** is drawn exactly and recorded, as at 40 fps, so nothing blends from a
  stale state.

On a 120 Hz headset, half is 60 fps (2 frames a step, each 2 refreshes). Headsets under 88 Hz keep unlocked's rate
(60 fps at most).

## Switching

The mode is read every headset frame (`vr_vulkan.cpp` `frame_begin`: `interp::set_vr_frame_mode`). It takes effect at
the start of the next logic step (`apply_vr_frame_mode`), so a step's passes keep the schedule they began with. No step
is run twice or skipped, and the game time does not jump. Changes apply at once from the F1 menu, with no restart.

## Checked

- **Unit tests** (`interp_pacing_test` `test_45`):
  - the fractions;
  - the modes' rates at 72, 90 and 120 Hz, and without a headset;
  - the step slots give 2 and 1 frames in turn at any phase of the grid;
  - the frames come out exactly 2/3 of a step apart.

  `vr_test`: `vr.frameRate` is saved and read, and is unlocked by default, also in older settings files. The other
  pacing and interpolation tests pass unchanged (`vsync_pacing`, `true60_steps`, `turbo_steps`, `interp_joints`,
  `interp_nodraw`, `interp_reticle`, `interp_sploosh`).
- **Desktop stereo mode** (no headset; `WWHD_INTERP_FORCE_45=1` draws the 45 fps schedule on the desktop, a test aid):

  | Run | Frame | Frames/s | Logic steps/s | Frames per step |
  |---|---|---|---|---|
  | Unlocked (as before) | 17.34 ms | 57.7 | 28.9 | 2.00 |
  | 45 fps | 22.94 ms | 43.6 | 29.0 to 29.3 | 1.50 ("on target", every frame 22.2 ms) |

  The game keeps its speed: 1.5 frames a step at the same logic rate.
- **A crash found and fixed** in the first 45 fps run: two histories (the sea grid, attachments) kept their state
  from a local that was destroyed before the code that keeps it ran.
- **In the headset** (2026-10-27): the player tried the modes, switching them from the VR menu (docs/VR_MENU.md), and
  kept them.

## The head pose at 45 and 30 fps (2026-10-28)

The head pose a frame is drawn with is predicted for the time it will be shown (`vr_game.cpp` `display_time`): the
render thread's last predicted display time plus the frames between. That counted one display period a frame, which
holds only at 90 fps on a 90 Hz headset. At 45 fps a frame is shown two periods after the one before, and at 30 fps
three. So the pose was predicted a refresh (11 ms) too early at 45 fps, and the view lagged the head while it moved;
SteamVR corrected each frame's rotation, but not the head's movement (jitter while the head moved, reported in the
headset).

Now the frames between are counted in the headset's refreshes per drawn frame: refresh rate over the drawn rate, from
1 to 4. That is 2 at 45 fps and 3 at 30; at 90 fps (and unlocked at its cap) it is 1, as before.

Not yet seen in the headset.

## The head pose counted from its own swap (2026-11-14)

The prediction counts the frames still queued after the frame whose display time it starts from. That time is set
by the render thread in `frame_begin`, inside its swap, but the swap was counted as done only when the whole swap
returned: after the eye copies, the frame's end and Turn smoothing's frames. A camera located by the game thread in
that window started from the new display time and still counted that swap as queued, so it was predicted one refresh
(11 ms at 90 Hz) too far. How often depended on thread timing, so the horizon could flip from frame to frame.

The headset corrects the turn of a frame (the pose sent is the pose drawn), but not the head's movement, nor Link's
hands and items, which are drawn from controller poses predicted the same way.

Now the display time is stored together with its swap (`vr::set_real_frame_display(time, swap)`, both read together
without a lock), and `display_time` counts the swaps issued after that one (`vr_game.cpp`). Unchanged:
- the refreshes per drawn frame (above);
- `WWHD_VR_PREDICT_FRAMES` and `WWHD_VR_PREDICT_QUEUED_MAX`;
- the pose sent equal to the pose drawn.

**On by default** (2026-11-14, not yet run in a headset). `WWHD_VR_PREDICT_PAIRED=0` counts as before, for a
comparison run.

**Check in the headset:** the `[vr] camera pose predicted ahead` line should show fewer changes. At Capped 45 with
Smooth Camera while Turning, `steady stick turn` and `frame evenness` must be no worse, since turn smoothing reads
the same times. Not yet run (docs/VR_STREAMING_AUDIT.md, finding 2).

## Locked to the headset's clock

The game steps and draws on a virtual 59.94 Hz vsync, as on the console. At 45 fps that put a frame every 22.244 ms,
while a 90 Hz headset shows one every 22.222 ms. The frames slid slowly across the headset's deadline, and while they
were near it, small timing variations put about one frame in seven on the wrong refresh: shown for 3 refreshes, with the
next shown for 1. The world juddered when the head moved, with 50 to 100 such frames every 10 s in the log
("missed display periods").

In VR, the virtual vsync is now 1.5 refreshes of a 90 Hz headset, or 2 of a 120 Hz one
(`gx2::lock_vsync_to_display`, called from `lock_to_headset` in `vr_vulkan.cpp`). The game still steps at 30 Hz, but at
the headset's 30.00 steps a second rather than 29.97 (0.1%). Headsets whose rate is not a multiple of 30 Hz (72, 80)
keep 59.94 Hz. Once the clocks are locked, the frames arrive at the same point of each refresh. If that point is near
the deadline, `note_display` sees three or more frames land on another refresh within 2 s and moves the grid half a
refresh later, once, at most every 5 s. The log notes each move ("frames near the headset's deadline").

## Quest defaults: 72 Hz, capped at 36 fps (2026-11-11, replaced the same day: below)

On a Meta Quest (`quest_headset()`) the defaults are **72 Hz** (`default_refresh_rate()`) with the frame rate **capped
at half: 36 fps** (`FrameRate::Half`); everywhere else 90 Hz and Auto Mode, as before. Why: a player's Quest 3 at the
old defaults (90 Hz, Auto Mode) and Headset resolution 0.70x drew 20-26 fps in play with the GPU 94-100 % busy
(35-69 ms a frame), never reaching Auto Mode's 45; 36 fps at 72 Hz is the rate earlier Quest builds held best.
Settings format 20 moves a Quest's saved old defaults (90 Hz with Auto Mode) to the new ones once; any other saved
rate or frame rate stays.

## Quest defaults: 90 Hz, Capped 30 FPS (2026-11-11, settings format 21)

The next player's log at 72 Hz / 36 fps: 17-23 fps with the GPU 100 % busy. A frame then stays on show for three
refreshes, then four (42, then 56 ms), and whatever moves moves in jerks ("highly jittery"). AstroQuest (a Quest 3
port, README "Two refreshes for a frame, or three") found the same and holds its game at **30 fps on 90 Hz**: every
frame exactly three refreshes, the same time on show as the next.

- **Capped 30 FPS** (steady) (`FrameRate::Original`, saved as `steady30`; the pacer's `kVrOriginal`: the game's own 30 steps a
  second, no in-between frames) is offered where the refresh rate is a multiple of 30 (90 Hz: three refreshes a frame;
  120 Hz: four), not at 72 or 80 Hz (uneven there: `validate` turns it to the half cap) nor at 60 Hz (its half cap is
  already 30).
- **A Quest's defaults:** 90 Hz with Capped 30 FPS. 30 fps leaves 33.3 ms a frame against 36 fps's 27.8. Format 21
  moves a Quest's saved format-20 defaults (72 Hz, Capped 36) and the older ones (90 Hz, Auto Mode) to them once.
- Head turns stay smooth at 90 Hz: the compositor turns each frame to the head's latest pose at every refresh.
- Below 30 fps frames still fall unevenly; the GPU savings (foveation, draw distance, the GPU's clock level,
  docs/VR_PERFORMANCE.md) and the Headset resolution are what bring it up to 30.

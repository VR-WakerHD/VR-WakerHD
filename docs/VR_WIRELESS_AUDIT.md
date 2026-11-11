# VR wireless streaming audit (2026-11-05)

Why a Quest 3 on Virtual Desktop (VDXR 1.1.0, Streamer 1.34.25, RTX 5080) stutters over Wi-Fi in this game when the
same setup is smooth wired and in other games, what was changed, and what still needs a headset to confirm. Evidence:
two game logs of the player's sessions (`wwhd.log`, `wwhd-previous.log`), Virtual Desktop's own logs (old install
errors only; its `OpenXR.log` was another application's session), and the code.

## 1. Problems found

| # | Problem | Evidence | Wireless only? |
|---|---|---|---|
| 1 | **60 fps submitted into a 72 Hz stream.** The pacer had no schedule for 72 (or 80) fps, so Capped at the full rate and Unlocked drew 60 there: no new frame on every sixth refresh. Virtual Desktop encodes and sends the frames it gets, so the stream received frames 1 and 2 refreshes apart; a wired headset's compositor hides it, a stream shows it as lag. | `refresh rates offered: 72 Hz`, `drawn at 60 fps (no schedule for its own rate; shown unevenly)`, 59.6 to 60 frames/s to the headset | Mostly |
| 2 | **Auto Mode tuned for a wired headset.** It judged 1.5 s windows and dropped to half at 90 %, so a hitch could change the stream's rate; each change of rate is a change of the encoder's cadence and of Virtual Desktop's frame smoothing. Earlier logs of the same player: Auto at half (45 fps) all session. | 2026-11-04 logs (docs/VR_STREAMING.md) | Yes |
| 3 | **Frame-timing shift reading network jitter as lateness** (fixed 2026-11-03, now tied to the setting below). | display times 24 to 460 ms apart on Wi-Fi | Yes |
| 4 | Not the cause: GPU headroom (42 to 47 % busy with the encoder), the game thread (VR work 0.02 ms a frame), the frame end (its own thread; the render thread never waited for it), head and controller tracking (no losses in play), GPU idle waits (asynchronous presentation is on: `vkQueueWaitIdle` runs only on swapchain recreation and resize), head poses (located once a frame at the predicted display time, one frame ahead). | `GPU busy`, `VR work`, `end-frame thread`, `Quest tracking`, `camera pose predicted ahead 1` lines | |

## 2. Code changes (uncommitted as of this report)

| File | Change | Why |
|---|---|---|
| `runtime/src/interp_pacing.h` | 72 and 80 fps schedules (`is_fps_variable`, `variable_step_frames`, `frame_variable`, `granule_variable`); `vr_drawable_fps`, `cap_fps_vr`, `cap_fps_vr_mode` take a streamed flag; Auto's streamed optimal profile (`kAutoWindowStreamed` 6 s, `kAutoDropStreamed` 85 %, `AutoRate::streamOptimal`) | Problems 1 and 2 |
| `runtime/src/interp.cpp` | `fps_variable()`: steps of 3 and 2 frames ending on the exact record pass (`plan_step_variable`), frame interval, granule and step ticks for them; `set_vr_stream_rates`; Auto's profile from it, logged; a warning when a streamed headset is drawn unevenly | Problems 1 and 2 |
| `runtime/src/interp.h` | `set_vr_stream_rates` | |
| `runtime/src/gfx/vulkan/vr_vulkan.cpp` | Streaming optimal applied each frame (`streamed() && vr.streamingOptimal`); the frame-timing shift off only with it; a `[vr] frame pacing` line: average fps, 1 % and 0.1 % lows, interval spread | Problems 1 to 3; diagnostics |
| `runtime/src/vr/vr_config.h` | `vr.streamingOptimal` (off by default; Detect Headset turns it on for a streamed headset) | Settings |
| `runtime/src/overlay/overlay.cpp`, `tools/vr_launcher/vr_launcher.cpp` | Streaming optimal toggle (F1 > VR, launcher Graphics); Detect Headset sets it from the runtime (streamed: on, frame interpolation for VR on; wired: off), Auto Mode kept | Settings, automatic configuration |
| `runtime/src/vr/vr_probe.cpp` | the probe line says `streamed=0|1` | Detect Headset |
| `runtime/src/vr/vr_swing.h` | a reversal is a turn past 120 degrees (was 90) | a regression from the stab rule of `a56f39d0` found by `vr_test` (one side swing taken for two) |
| `runtime/tools/interp_pacing_test.cpp`, `runtime/tools/vr_test.cpp` | checks for the 72/80 fps rates and patterns, both Auto profiles, the stab start | Tests |
| `docs/VR_STREAMING.md` | the schedules, the setting, the two profiles | |

Other uncommitted work from the same day, unrelated to wireless: Quest multiview re-pairing between frames and its
logs (`surfaces.cpp`, `backend.cpp`, `draw.cpp`, `crash_context.cpp`), Full-size torchlight always off on standalone
headsets (`renderer.cpp`), the second drawing thread from 700 draws on standalone headsets (`stereo_switches.h`).

## 3. Performance

No headset run yet (headset tests need the user's go-ahead). What is measurable now:
- The player's sessions: 59.6 to 60 frames/s to a 72 Hz headset; 1 frame in 6 refreshes without a new one.
- Expected with the change: 72 frames/s to the headset, every refresh; game time per frame a third or a half of a
  logic step (not exactly 5/12: a step must end on its exact frame), against today's half step then nothing.
- `interp_pacing_test` passes (all checks, the new ones included).

## 4. Wireless compatibility

Wireless-specific: the 72/80 fps schedules (streamed only), Auto's streamed optimal profile, the frame-timing shift off,
Detect Headset's streamed settings. General: the `[vr] frame pacing` diagnostics; the swing fix.

Streamed or not is known from the runtime's and headset's names and, for other runtimes, from the steadiness of their
predicted display times (`openxr_session.cpp`). OpenXR does not say whether a Quest is on Wi-Fi or a cable: a Quest
through Virtual Desktop is "streamed" either way (both are video streams). The setting can be turned off by hand.

## 5. Runtime compatibility

Unchanged: no runtime-specific code was added to the renderer; the schedules depend only on the refresh rate and the
streamed flag. Extensions stay optional with fallbacks (refresh rate selection, cylinder layers, depth, frame synthesis).
Runtimes not tested in this round: all (no headset runs).

## 6. Settings

| Setting | Default | Where |
|---|---|---|
| Streaming optimal (`vr.streamingOptimal`) | off (Detect Headset: on for a streamed headset) | launcher Graphics; F1 > VR |
| VR frame rate (`vr.frameRate`) | Auto Mode (wired optimal, or streamed optimal on a streamed headset with Streaming optimal) | same |

The brief's other controls do not apply to this renderer or exist already: there is no film grain, procedural screen
noise, screen-space reflections or volumetric lighting in Wind Waker HD's renderer; motion blur ("Disable screen blur"),
bloom, ambient occlusion, anti-aliasing (FXAA), anisotropic filtering, render scale ("Headset image size", internal
resolution) and dynamic resolution are existing settings. Particle density was not changed (the game's own emitters;
reducing them changes the look and gameplay feedback, and the GPU was half idle).

## 7. Regression testing

Automated: `interp_pacing_test` passes; `stereo_test` passes; `vr_test` passes except three Deku Leaf flap checks
(`FlapDetector`, vr_test.cpp 1246, 1248, 1259) that already failed before today's changes (checked by building it with
the swing detector of `454049fb`). Tests that start the game were not run (they need the user's go-ahead). In the
headset: nothing yet.

## 8. Remaining (needs the headset)

- Virtual Desktop at 72 Hz over Wi-Fi with Streaming optimal: the log should show `streaming optimal: on`, about 72
  frames/s to the headset, `[interp] target 72 fps (2.4 frames a step ...)` on target, and the frame pacing line's lows
  near 72. Then 80 Hz; then the same wired, to compare.
- Whether the third/half-step motion within the 72 fps steps is visible (fast pans, sailing).
- Auto Mode's streamed profile through a busy scene (Forest Haven, Windfall) on Wi-Fi.
- The three Deku Leaf flap test failures.

## 9. Recommendations

- For players now: Virtual Desktop at 90 Hz (3 frames a step, already even) where the PC keeps up.
- Anisotropic filtering on in VR (it was off in the player's settings): less texture shimmer in head movement, which
  also eases the encoder; small GPU cost on a desktop GPU.
- The eye pictures were drawn larger than the headset's images (3688x3985 against 3072x3264 at internal resolution 3x):
  a choice of quality, not a fault; on a GPU near full with the encoder, "Headset image size" or a lower internal
  resolution gives the encoder room.
- An exact 5/12-step schedule (a blended record pass at a step's last frame) would make the 72 fps motion perfectly
  even; it touches the records of every interpolated system and needs its own picture comparisons.

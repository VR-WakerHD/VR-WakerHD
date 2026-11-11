# Streamed headsets (Virtual Desktop, a Quest on a PC)

A streamed headset gets the game's pictures compressed, over Wi-Fi or a cable: Virtual Desktop's own OpenXR runtime
(VDXR), ALVR, or a Quest on any PC runtime (Meta's Link and Air Link, SteamVR's Steam Link). Players on a Quest 3
through VDXR on Wi-Fi reported judder when looking around ("skips frames"); on a wired PS VR2 the same build ran clean
(2026-11-03).

## What is different there

The runtime's predicted display times (`xrWaitFrame`) move with the network. A Quest 3 log through VDXR on Wi-Fi had
them up to 155 ms apart. Two of the game's own rules read those times as the game's own lateness:

- **The frame-timing shift** (`vr_vulkan.cpp` `note_display`): after a few frames that look late it moves all the game's
  frames half a refresh later, at most every 5 s, a hitch each time. That log had several of them
  (`[vr] frames near the headset's deadline: moved half a refresh later`).
- **Auto's return to the full rate** (`interp_pacing.h` `auto_rate_step`): after 10 s at half the rate it tries the full
  rate again. That log went between 90 and 45 again and again, a change of smoothness each time, while Virtual Desktop's
  own frame smoothing (SSW) also works at half the rate.

## What the game does on a streamed headset

Detected per session (`openxr_session.cpp`, after the runtime's name and the Quest check; `Session::streamed`), on a
PC build only:
- **By name, streamed:** a Quest or Pico headset on any runtime, or a runtime whose name says Virtual Desktop, ALVR or
  Pico.
- **By name, wired (never streamed):** PS VR2 (`SteamVR/OpenXR : playstation_vr2`), Index and Vive (`lighthouse`,
  `index`, `vive`), Windows Mixed Reality, Pimax, Bigscreen.
- **Any other headset is measured:** its predicted display times in windows of 900 frames (10 s at 90 Hz). A window
  with 5 % or more of them off a whole number of refreshes makes it streamed from then on (a streamed runtime's times
  move with the network; a wired one's stay whole refreshes apart, late frames included). Four steady windows end the
  measuring. This covers a Steam Frame streaming PC VR through SteamVR, and streamers not named above.
- `WWHD_VR_STREAMED=1` or `0` forces it, with no detection.

The log says `[vr] streamed headset: yes`, `no (a wired headset)`, `not known: ... measured`, then `yes, from its
display times (...)` or `no (display times steady ...)`. Android and the Steam Frame's own build are not PC builds and
run as before.

- **No frame-timing shift.** The streaming runtime paces the frames it gets.
- **Auto holds half the rate longer**: 40 s before the full rate is tried again (not 10 s), doubled after each failed
  try up to 160 s (not 40 s), and back to 40 s after a try that holds (`kAutoBackoffStreamed`,
  `kAutoBackoffStreamedMax`). `interp_pacing_test` checks it.
- **Display-time steadiness is logged** every 10 s: `[vr] streamed display times (10 s): N frames, N uneven (%), the
  farthest N ms off`, where uneven means more than a quarter refresh off a whole number of refreshes. A player's log
  then shows whether the network or the game made the judder.

## GPU busy time (every VR session)

`[vr] GPU busy: N% of the time, N ms per frame shown` every 10 s: every command buffer is timed on the GPU while a VR
session runs, as dynamic resolution does (`dynres::watch_gpu`; two timestamps a command buffer, overlaps counted once).
On a streamed headset the line says so: its video encoder runs on the same GPU, and a GPU near 100 % leaves it no room
(late or dropped stream frames with the game's own frames on time).

## Frame rates the headset shows evenly

At half the refresh rate with no schedule for it, the game drew the fastest rate under it, shown unevenly: 45 fps at
108 Hz (frames held 2 and 3 refreshes in turn, judder in every turn; Steam Frame log). Now it draws the nearest rate the
headset shows evenly when that keeps at least 75 % of it (`interp_pacing.h` `vr_even_fps`): 36 fps at 108 Hz, 3
refreshes each. 72, 80, 90 and 120 Hz are unchanged (36, 40, 45, 60 already fit); 144 Hz keeps 60 (36 is too far
down). The full rate keeps its frames; the log says `shown unevenly` when it does not fit. Every platform, not only
streamed headsets. `interp_pacing_test` checks it; not yet tried at 108 Hz in a headset.

## Settings a player can try

Reported by other players for Virtual Desktop in general, not measured here:

- Virtual Desktop's SSW set to always on or off, not automatic.
- The stream's refresh rate matched to what the game holds (for example 90 Hz with the game at 45).
- A dedicated 5 or 6 GHz router near the play space.

Not yet tried with a Quest on Wi-Fi.

## Auto judged in gameplay only (2026-11-04)

Two logs of a Quest 3 player on Virtual Desktop over Wi-Fi (VDXR 1.1.0, Streamer 1.34.25, RTX 5080, build `50ac1f1`)
reporting stutter: in both, Auto (`vr.frameRate=auto`) went to half the rate at the title screen, unlocked drawing 38 %
and 79 % of its in-between frames there behind the shader preparation's hitch (an in-between pass probe of 92 ms). A
streamed headset then holds half the rate for 40 s (`kAutoBackoffStreamed`), counted in logic steps; with the file
select, loading and pauses the sessions never reached the first try: 45 fps all session, every frame shown twice on
the 90 Hz headset, while the GPU was busy 3 to 5 % of the time and the render thread 0.15 ms a frame.

Auto now judges only gameplay (`interp.cpp` `auto_rate_after_step`, `aspect::gameplay_camera`): out of it, its choice
and its waits stand still; coming back into gameplay at the unlocked rate, the first 3 s (`pacing::kAutoSettle`, the
scene's first draws and shaders) are not judged. `interp_pacing_test` checks the settle.

Also in those logs, not changed here: the headset's rate moved from 90 to 72 Hz mid-session (Virtual Desktop's own
setting; frame times up to 100 ms and the controllers lost while it changed); the eye pictures were drawn at the 1x
internal resolution (1230x1335, scaled to 2752x2940); up to 6.6 % of display times uneven in the first seconds.

## Streaming optimal: a frame on every refresh at 72 and 80 Hz (2026-11-05)

Two logs of the same Quest 3 player on Virtual Desktop over Wi-Fi (VDXR 1.1.0, RTX 5080), lag that other games did not
have: the stream ran at 72 Hz (`refresh rates offered: 72 Hz`), and the game drew 60 fps there (`drawn at 60 fps (no
schedule for its own rate; shown unevenly)`, 59.6 to 60 frames a second to the headset), so Virtual Desktop's encoder got
no new frame on every sixth refresh; 6.7 to 8.9 % of display times uneven. The PC was far from its limit (GPU 42 to 47 %
busy with the encoder, the game thread 0.02 ms a frame), tracking steady.

**72 and 80 fps schedules** (`interp_pacing.h` `is_fps_variable`, `interp.cpp` `plan_step_variable`): 12 frames over 5
logic steps and 8 over 3, steps of 3 and 2 frames (3,2,3,2,2 and 3,3,2) from the step slots (granules of 10 and 9 ticks,
a step 24), each step ending on its exact record pass as at 60 and 90 fps (the same passes, records and fractions); every
frame one refresh apart. The game's time moves a third or a half of a step a frame within a step (not exactly 5/12 and
3/8: a step cannot end between frames on these passes). The logic stays at 30 steps a second.

**Only on a streamed headset with Streaming optimal** (`vr.streamingOptimal`, off by default, Detect Headset turns it on for a streamed headset; Graphics in the launcher,
F1 > VR in the game; `interp::set_vr_stream_rates`: `Session::streamed()` and the setting, each frame): Capped at the
full rate and Unlocked draw 72 / 80 fps at 72 / 80 Hz; half the rate (36, 40), other refresh rates and wired headsets are
unchanged (60 fps at 72 Hz there). Logged: `[interp] streaming optimal: on ...`, and on a streamed headset drawn unevenly
`[interp] a streamed headset at N Hz with N fps drawn: the stream gets frames unevenly ...` with what makes it smooth.

**Detect Headset** (launcher): the game's probe line says `streamed=0|1` (by the runtime's and headset's names); a
streamed headset gets Streaming optimal on, the frame rate Capped at the refresh rate (not Auto Mode, whose switches
between the full and half rate change the stream's smoothness) and frame interpolation for VR on; a wired headset gets
Streaming optimal off and keeps Auto Mode. `interp_pacing_test` checks the rates and patterns. Not yet tried in a headset.

## Every frame-rate mode: wired optimal and streamed optimal (2026-11-05)

Streaming optimal (`vr.streamingOptimal`) chooses between two sets of rules for every frame-rate mode; off (or a
wired headset) is wired optimal throughout, as a wired headset always ran:

| | Wired optimal | Streamed optimal (a streamed headset, Streaming optimal on) |
|---|---|---|
| Capped (full), Unlocked at 72 / 80 Hz | 60 fps (no schedule for 72 / 80) | 72 / 80 fps, a frame every refresh |
| Capped (half) | 36 / 40 / 45 fps | the same |
| Auto Mode: window judged | 1.5 s (`kAutoWindow`) | 6 s (`kAutoWindowStreamed`): a hitch does not change the rate |
| Auto Mode: half the rate below | 90 % of the in-between frames (`kAutoDrop`) | 85 % (`kAutoDropStreamed`) |
| Auto Mode: back to full at | 98 % | 98 % |
| Auto Mode: wait before trying full again | 10 s, at most 40 s | 40 s, at most 160 s (`kAutoBackoffStreamed`) |
| The frame-timing shift (`note_display`) | on | off (the runtime paces; its times move with the network) |

Logged: `[interp] VR frame rate auto: streamed optimal (...)` or `wired optimal (...)` when Auto's profile changes.
Detect Headset keeps Auto Mode on both (streamed: Streaming optimal on and frame interpolation for VR on; wired:
Streaming optimal off). `interp_pacing_test` checks both profiles. Not yet tried in a headset.

## Depth for Virtual Desktop (2026-11-05)

`vr.vdxrDepth` (off by default; under Streaming optimal in the launcher's Graphics tab and F1 > VR; applies at the next
start): on Virtual Desktop's own runtime (VDXR, by the runtime's name) with Streaming optimal, each eye's depth goes
with its picture (`XR_KHR_composition_layer_depth`, the depth-only path of docs/VR_FRAME_SYNTHESIS.md), so Virtual
Desktop's frame smoothing (SSW) and reprojection follow head translation and depth when a frame is late or the network
stalls, instead of turning the last picture flat. `vr.frameSynthesis` (off by default on PC) still decides for every
other runtime.

The extension is chosen with the instance, before the runtime's name is known: on a PC with Streaming optimal it is
enabled whenever offered (`vdxrDepthOffered_`) and used only once the runtime turns out to be VDXR (`openxr_session.cpp`,
after the headset checks). Depth is not sent while the game's UI layer shows text or menus (the 2026-11-03 guard: in
VDXR's composition the UI quad went behind the person talked to), nor now while the GamePad screen or the item wheel is
shown (quads in front of the scene too). Logged: `[vr] frame synthesis: depth for Virtual Desktop (...)` and `[vr frame
synthesis] depth not sent while ... / sent again`. Not yet tried in a headset: check conversation text, the GamePad
screen and the item wheel show, and nothing smears in fast turns.

Its cost on the CPU (asked for: none on the drawing threads beyond what is needed): depth alone uses no motion pass
(nothing drawn twice). Per frame the render thread records two small passes at the frame's end (each eye's depth
into the runtime's half-size depth image). During the frame, `vr_framesynth::note_camera` reads the camera's planes and
counts the depth target from one camera draw in 32 per drawing thread (it had been every camera draw, 600 and more a
frame, each with a shared lock). With Streaming optimal or Depth for Virtual Desktop off, or on another runtime, none of
it runs (`vr_framesynth::active()` false: one branch per camera draw).

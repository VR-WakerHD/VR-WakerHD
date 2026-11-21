# VR frames paced by the runtime (plan, 2026-11-18)

## Why

In PC VR the game kept its own time: a virtual vsync on the PC's clock with the headset's period
(`gx2::lock_vsync_to_display`), its phase wherever it started, and xrWaitFrame called only at the swap, after the frame
was drawn. Nothing tied the phase to the runtime's frames. Under SteamVR, xrWaitFrame then returned at once (0.01 ms in
the 2026-11-18 frame logs) and the game ran on its own clock; the phase wandered (8.2 to 9.3 ms of the refresh over
16 s) and jumped after hitches (9.0 to 6.0 ms at a 29 s hitch), and frames that drifted onto the runtime's deadline
were shown late (reprojected: a double image) or moved a slot (the "runtime's frame smoothing on/off" lines). The
half-refresh move (`note_display`) and the fixed period were workarounds for the same drift.

## The change

After each OpenXR frame (a real frame, a refresh fill, a frame kept alive) the render thread asks the runtime for the
next one at once (xrWaitFrame), so the runtime holds the game every frame, as Monado does in OpenXR mode. The game's
vsync grid is re-anchored to the moment the runtime releases each real frame, so its flips and its 30 Hz logic steps
follow the runtime's display times. Gameplay, game speed and the game's own clock (OSGetTime) do not change.

`WWHD_VR_RUNTIME_PACING=0` keeps the old timing (a comparison run). PC VR only; Android and Quest are untouched.
Fast Forward and the other game-clock speed modes keep the old timer while they run.

## Phases

0. Baseline: a logged headset run on the old build (frame log and pose log, Capped 90 and Capped 45). A simulated
   runtime in the tests (`runtime/tools/vsync_pacing_test.cpp`): releases on its own clock, slightly off 89.91 Hz, with
   jitter.
1. Core: `Session::wait_frame` (xrWaitFrame, kept as the waited frame) and `Session::begin_frame` (xrBeginFrame, takes
   the waited frame; waits itself when none is waiting), so `predicted_display_time()` stays the begun frame's while
   the next is waited for (the end-frame thread's xrEndFrame reads it). The next xrWaitFrame after every frame's end (`ask_next`, on the
   end-frame thread: after its own xrEndFrame, or as a job after a frame the render thread ended, `end_thread::ask_async`); the
   vsync grid re-anchored to each real frame's release (`gx2::anchor_vsync_grid`, with the step slots moved by the same
   amount); the half-refresh move off in this mode. The camera keeps predicting one frame ahead of the newest display
   time (the game thread builds frame N+1 while the render thread presents N; gating it on the runtime would remove
   that overlap).
2. Frame planner: in this mode the game thread's waits (the vsync wait, waits for the render thread) are taken out of
   each pass's time, as its own sleeps are, so the planner compares work, not the headset's rhythm; a missed refresh
   (a real frame later than its slot) holds one in-between frame back for a second.
3. Audit and adapt: refresh filling, keep-alive during loads, turn and head movement smoothing, steady hands, Auto
   Mode, frame synthesis, controller poses, streamed headsets, session events, dynamic resolution and the GPU watcher.
4. Diagnostics: the timing lines that assume the old pacing; a line for where the runtime holds the game (xrWaitFrame
   or xrEndFrame) and how far the grid moved to follow it.
5. Verification in the headset (the owner): Capped 90 / 45, Auto, Steady 30, Unlocked; turning, steady hands; a load,
   the title; SteamVR with the Monado driver and Sony's, native Monado, Windows SteamVR. Before/after from the frame
   log: off-slot frames, missed refreshes, smoothing switches, frames a step at the title.
6. Docs (`docs/VR_FRAME_RATE.md`, "Paced by the runtime"), release note, commits per phase (local only).

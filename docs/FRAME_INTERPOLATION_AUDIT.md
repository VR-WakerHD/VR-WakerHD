# Frame interpolation and VR rendering: audit (2026-11-12)

Phase 1 of the frame interpolation brief: what the code does today, measured against what the brief asks for, before
anything is changed. Every statement points at the code it comes from. Status words: **implemented** (built and on),
**built, off** (built, off by default), **runtime-dependent**, **needs a headset test**, **not implemented**.

Nothing here has been run in a headset for this audit; the test status of each part is the one its own document records.

## Summary

The brief's central request, a fixed 30 Hz simulation with rendering decoupled and blended at the headset's rate,
**already exists** and has since before the VR fork (`interp.cpp`, `interp_pacing.h`). So do multiview, foveation,
motion vectors, depth export, Quest Application SpaceWarp, runtime counters and GPU timestamps. Rewriting them as the
brief's `TransformSnapshot` design would replace tested code with untested code for no gain.

What is actually wrong, and worth changing, is narrower:

1. **The blend fraction comes from a fixed schedule, not from when the frame is shown.** A frame that misses its
   refresh shows the world where it should have been a refresh earlier: uneven motion while the head stays smooth. This
   is the jitter PC players report with interpolation on, and why they find it smoother off.
2. **Auto Mode switches between full and half rate** on a computer near its limit, a hitch each time.
3. **Camera motion (locomotion, turning) is not given to the runtime's reprojection** when frames are reused, so at 30
   fps the camera steps 30 times a second while the head is smooth.
4. **Frame synthesis (AppSW) has never been run on a Quest**, and the Quest draws 20-30 fps, so whether it can hold 45
   is unknown. The runtime counters (added 2026-11-12) are the measurement that decides it.

## A. Simulation and game loop

| Question | Answer | Where |
|---|---|---|
| Main loop | The game's own main loop body (025F172C), run by the recompiled code on the game thread; hooked | `tools/recomp/hooks.txt`, `interp.cpp` |
| Simulation rate | **Fixed 30 steps a second** (the game's own); never changed (a standing rule) | `interp.cpp` header |
| Physics, animation | Inside the game's actor execute (`fpcEx_Handler` 025DE788), once a logic step | `interp.cpp` `hook_025DE788` |
| Camera | Logic: the game's camera actor each step. Drawn: `camera_draw` (024FFC40) every frame, blended between steps, then the VR head pose applied | `interp.cpp` `hook_024FFC40`, `vr_game.cpp` `CameraOverride` |
| Rendering rate | 30, 36, 40, 45, 60, 90 or 120 fps in VR (the pacer's schedules); the headset's refresh is the cap | `interp_pacing.h` `vr_mode_cap`, `vr_drawable_fps` |
| Coupled? | **Decoupled.** With interpolation on the main loop runs N+1 passes per logic step: the logic pass runs the step and draws at t = 1/(N+1); hold passes run no logic (no execute, create, delete, scene management, counters, audio, HD UI updates) and draw again at the next fraction; the last draws the step exactly and records it as the next step's "before" | `interp.cpp` header, `interp_pacing.h` |
| Several frames per step? | Yes: 1 in-between at 60, 2 at 90, 3 at 120 | `interp_pacing.h` |
| Interpolation exists? | **Yes, implemented.** Camera (eye, centre, up, fov), every model's joint matrices (`interp_joints.h`, with stale-joint and teleport cuts), effects (`interp_fx.cpp`), fades, trails, nodraw requests | `interp_*.h` |
| Extrapolation? | None (by design: no unbounded extrapolation) | |
| Teleports, cuts, spawns | Joint teleport cut (a joint moving too far, or to/from NaN, draws exact); stale joints draw at the current step; no-draw requests hold for all passes | `interp_joints.h`, `interp_nodraw.h` |
| Simulation tied to the refresh rate? | No. The game clock's virtual vsync is locked to the headset's period over 1, 3 or 5 steps where it fits (`lock_to_headset`), so logic steps land on refreshes; logic speed stays 30 | `vr_vulkan.cpp` `lock_to_headset` |

**The one design issue (point 1 above).** The fraction of a pass is `pacing::pass_t(phase, n)`: a fixed function of the
pass's place in the step (0, 1/3, 2/3 at 90 Hz), and `anchor()` and every blend read it. It assumes every pass reaches
the headset on its planned refresh. When the GPU or CPU misses one, the runtime shows the previous image again (or
synthesizes one), and the late pass's fixed fraction is displayed a refresh late. The brief's own formula,
`alpha = (renderTime - previousSimulationTime) / (currentSimulationTime - previousSimulationTime)` with `renderTime` the
frame's predicted display time, is the fix, applied to the existing blend rather than a new snapshot system.

## B. OpenXR implementation

| Item | Implementation | Where |
|---|---|---|
| Loader, instance | The loader opened at run time; `XR_KHR_vulkan_enable2`; the Vulkan instance and device created through OpenXR | `openxr_session.cpp` |
| Runtime detection | Runtime name and headset from the system properties; Quest (standalone or PC), Steam Frame, streamed (Virtual Desktop, Link) detected | `openxr_session.cpp`, `docs/VR_STREAMING.md` |
| Extensions | Enumerated; enabled only when offered: display refresh rate, cylinder layers, composition layer settings (Super Resolution, Android), recommended layer resolution (Android), performance settings (standalone), performance metrics (all), frame synthesis / space warp / depth (by `vr.frameSynthesis`), Android thread settings, hand interaction, Steam Frame controllers | `openxr_session.cpp` instance creation |
| Swapchains | Per eye colour; UI, panel, wheel, wrist, laser quads; motion vector and depth swapchains when frame synthesis is chosen. Remade only on size change, after waiting for the GPU and the end-frame thread (2026-11-11 crash fix) | `openxr_session.cpp`, `vr_vulkan.cpp` `ensure_swapchain` |
| `xrWaitFrame` / `xrBeginFrame` | Render thread, at the start of each headset frame (`frame_begin`) | `openxr_session.cpp` `begin_frame` |
| `xrEndFrame`, image release | **On their own thread** (the end-frame thread: images released, layers built, `xrEndFrame`); `WWHD_VR_END_FRAME_THREAD=0` keeps it on the render thread | `end_thread`, `openxr_session.cpp` `end_frame` |
| `xrLocateViews` | Game thread, once per frame for the camera (`game_views`, fresh), at `display_time()`: the render thread's last predicted display time plus the frames still queued, times the refreshes each drawn frame is shown for. The render thread locates its own only for frames drawn without the game camera | `vr_game.cpp` `display_time`, `game_views` |
| Pose submitted | **Exactly the pose the frame was drawn with** (its views' id travels with the frame's swap), so the runtime's reprojection corrects only the prediction error | `docs/VR_ARCHITECTURE.md` section 3 |
| Projection | Per-eye field of view since 2026-11-03 (`WWHD_VR_EYE_PROJECTION=shared` for the old symmetric one) | `docs/VR_EYE_PROJECTION.md` |
| Depth submission | `XrCompositionLayerDepthInfoKHR` per eye when frame synthesis is `auto`/`on` (on PC: depth only); **built, off** | `docs/VR_FRAME_SYNTHESIS.md` |
| Vulkan sync | Submission slots (6 on standalone), record worker, eye thread, atlas thread, scene drawing threads; timeline per frame | `docs/VR_PERFORMANCE.md`, `VR_SCENE_THREADS.md`, `VR_EYE_THREAD.md`, `VR_ATLAS_THREAD.md` |
| Streaming | Predicted display times move with the network on VDXR/Link; the frame-timing shift and the pose prediction take streamed runtimes into account | `docs/VR_STREAMING.md` |

**Latency note (not a defect).** The pose is located on the game thread when the frame's camera is drawn, one to four
frames ahead of display (the queued swaps counted). There is no late latching (re-locating on the render thread and
patching the matrices just before submission). The runtime's reprojection corrects the remaining prediction error, which
is the standard approach; late latching would need the eye uniforms rewritten after recording, a large change to the
record worker for a small gain.

## C. Existing VR rendering features

| Feature | Status | Where / notes |
|---|---|---|
| Multiview (`VK_KHR_multiview`) | **Implemented, on by default** (PC and standalone); draws both eyes from one stream where a target pairs; falls back per eye elsewhere | `docs/VR_MULTIVIEW.md`; never turned off without asking (standing rule) |
| Stereo replay | Implemented: eye 1 replays eye 0's commands with its own targets where multiview cannot | `gfx/vulkan/stereo.cpp` |
| Asynchronous timewarp / reprojection | **Runtime-provided** (every runtime); nothing of ours competes | |
| Positional timewarp | **Runtime-dependent**: needs depth (built, off) | `VR_FRAME_SYNTHESIS.md` |
| Motion vectors | **Built**: the motion pass draws every depth-writing draw and the background again with previous and current data, per eye, 6DoF head, locomotion, skinning, the boat, enemies; used by DLSS and frame synthesis | `gfx/vulkan/motion_pass.h`, `docs/VR_DLSS.md` |
| Quest AppSW (`XR_FB_space_warp`, `XR_EXT_frame_synthesis`) | **Built, off; never run on a Quest** | `vr/vr_framesynth.h`, `gfx/vulkan/vr_framesynth.cpp` |
| Dynamic resolution | **Built, off by default**; forced off on Quest (Adreno crashed after rescales of multiview targets) | `docs/VR_DYNAMIC_RESOLUTION.md`, `dynres.cpp` |
| Foveated rendering | **Implemented**: shading rate map / fragment density map; Medium on Quest | `docs/VR_FOVEATION.md` |
| Frame interpolation | **Implemented** (section A) | |
| Frame pacing | **Implemented**: schedules for 120, 90, 60, 45, 40, 36, 30; Auto (full while frames fit, else half); clock locked to the headset | `interp_pacing.h`, `docs/VR_FRAME_RATE.md` |
| GPU timestamps | **Implemented** (the frame's GPU time) | `gfx/vulkan/backend.cpp` |
| Runtime counters (`XR_META_performance_metrics`) | **Implemented 2026-11-12**: app/compositor GPU and CPU time, GPU use, logged every 10 s; needs a headset test | `openxr_session.cpp` `poll_perf_metrics` |
| Upscaling | Implemented: Quest Super Resolution, SGSR1, CAS (+FXAA), DLSS on RTX PCs; SGSR2, FSR1 by choice | `docs/VR_UPSCALING.md` |
| UI as quad layers | Implemented; the game's UI layer only sent on frames the game drew UI | `vr_vulkan.cpp` |

## D. Camera and gameplay systems that interpolation must not disturb

All of these are drawn through the same blended passes and already handle them:

- **Head tracking**: never interpolated with the game; the view's anchor (Link's eyes) is blended by `pass_t`
  (`vr_game.cpp` `anchor`), the head pose added on top at its predicted time.
- **R-stick turning, lock-on, body follow**: rig turns advance per frame by real time (`advance_rig_turn(dt)`), the body
  yaw per step.
- **Arm IK, sword, shield**: posed per step (`link_arms`) and per pass from the posed hand (`frame_correct`, `g_locks`),
  so held items stay on the hands at every in-between frame (docs/VR_HELD_ITEMS.md "One piece").
- **Weapon collision**: the tracked sword's sweep is written at the logic step (`link_executed`), unaffected by drawing.
- **HUD, wrist HUD, dialogue**: quad layers, not blended.

## E. Diagnostics already in place (Phase 2)

| Asked for | Present | Where |
|---|---|---|
| CPU frame/game time, GPU time | Yes, per frame, summarised every 10 s | `vr_vulkan.cpp` `FrameTimes`, `[vr] ... per frame` lines |
| xrWaitFrame wait, submission, release, acquire | Yes (`wait`, `acquire`, `queue`, `submit`, `release`, `join`) | `vr_vulkan.cpp` `FrameTimes` |
| Application fps, refresh rate | Yes | `[interp] VR frame rate`, overlay |
| Missed deadlines, slips | Yes: frames near the deadline, display-time slips | `note_display`, `[vr] frames near the headset's deadline` |
| Reprojection statistics | Runtime counters (2026-11-12) where offered; SteamVR's own frame timing otherwise | |
| Motion pass cost | Logged when the motion pass runs | `motion_pass` |
| CSV logging, on-screen overlay | The performance overlay (F1); the log's 10 s summaries. **No CSV export** | |
| Percentiles (p95, p99) | Partly: xrWaitFrame p50 logged; no p95/p99 of frame time | |

## F. Platform notes

- **Windows and Linux PC VR**: the same Vulkan and OpenXR code; SteamVR offers depth but no motion vectors, so the only
  runtime-side smoothing is its reprojection and Motion Smoothing (the player's choice in SteamVR). Never stack ours on it.
- **Quest 3 through Virtual Desktop**: VDXR's own timing and SSW; the game does not enable `XR_FB_space_warp` on PC
  builds (the PC build enables only what the runtime offers, and frame synthesis is off).
- **Quest 3 through SteamVR / Link**: as PC VR, streamed timing rules apply.
- **Quest 3 standalone**: 90 Hz, Steady 30 FPS by default (every frame three refreshes), no interpolation, frame
  synthesis off; the Quest draws 20-30 fps.

## G. Recommended changes, in order

| # | Change | Platforms | Cost / risk | Status |
|---|---|---|---|---|
| 1 | **Blend at the display time**: each pass's fraction from its predicted display time against the logic step times (clamped to [0, 1], never extrapolating), so a late frame still shows the world where it belongs when it is shown | PC VR (interpolation on) | Small code, touches the pacer: test at 72, 80, 90, 120 Hz | Not implemented |
| 2 | **Steadier Auto Mode**: hold a rate longer, drop to half only on sustained missed deadlines and the runtime counters, never during a cutscene or scene change | PC VR | Small | Not implemented |
| 3 | **Camera reprojection on reused frames**: submit depth, and on refreshes that show the previous frame again pass the camera's movement since it was drawn (locomotion, turning) as its pose, so the runtime slides it on | PC VR at 30 or 45; Quest at Steady 30 | Moderate, VR frame loop only | Not implemented |
| 4 | **Quest AppSW test build**: frame synthesis on, with the runtime counters, to see whether a Quest can hold 45 | Quest standalone | None to build (exists); needs a headset | Built, off |
| 5 | **p95/p99 frame times and a CSV export** of the 10 s summaries, behind an environment variable | All | Small | Not implemented |

Not recommended: a new snapshot/transform system (exists as the pass blend), late latching (large change to the record
worker for little gain), our own motion-vector frame generation on PC (SteamVR takes no motion vectors from games, so it
would be a whole warp pass of our own), dynamic resolution on Quest (crashed; skipped 2026-11-12 until the counters show
the GPU is the limit).

The implementation document and benchmarks follow once a change is chosen and measured; no benchmark numbers are
claimed here.

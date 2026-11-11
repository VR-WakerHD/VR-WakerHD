# VR mode: testing

What is verified where. "Verified on hardware" means it ran on a headset; "implemented" means it is built and
unit-tested where possible but has not been run with the game.

## Automated (no headset)

`ctest -R vr` runs `runtime/tools/vr_test.cpp`:

| Area | Checks |
|---|---|
| Pose conversion | OpenXR forward/right/up to the game's heading convention, scaled points around the anchor, round trip game <-> OpenXR, head orientation to game direction, look rotation <-> look-at |
| Angles | s16 game angles, wrapping, headings |
| Projection | asymmetric eye projection puts the FOV edges on the picture edges, symmetric union of both eyes, the game's fovy / aspect for it |
| Configuration | parsing, clamping, unreadable / non-finite / trailing-garbage values keep defaults, unknown keys, swing threshold order, round trip |
| Bindings | defaults, remapping, unknown buttons, unbinding, round trip, recentre gesture (hold time, once per hold) |
| Locomotion | stick remap to the head's heading (length kept), snap turn edge and direction, smooth turn rate and dead zone, head offset limit |
| Arm IK | bone lengths kept, wrist on target, elbow towards the pole, out-of-reach clamp (no stretching), target inside the shoulder, target on the pole line, body yaw dead zone and rate, shoulder placement |
| Swing | tip velocity, idle motion never a swing, start after the minimum time, hysteresis end, cooldown, hitches and teleports ignored, horizontal / vertical / thrust / rising classification incl. body yaw, non-finite input |
| Sweep | blade from grip pose, a 60-degree swing between frames covers a target the two blades miss, attack vector, no false contact |
| Hit ledger | one record per target per swing, reset per swing |

The rest of the suite (607 tests) checks that the desktop game's renderer, input, mods and editor logic are
unchanged.

## Renderer smoke test (GPU, no game files)

`wwhd --renderer-smoke` exercises the Vulkan renderer, including the stereo paths `stereo::XR` changes.
With `WWHD_VR=1` it also goes through the runtime (instance and device through `XR_KHR_vulkan_enable2`,
headset GPU choice, session, swapchains) and, at the end, runs 270 frames of the OpenXR frame loop on the
headset (`vrvk::smoke`): wait/begin, locate views, sync actions, acquire both swapchain images, a GPU clear
of each, release, `xrEndFrame` with a projection layer, a vibration on both controllers.

## Results of this work

Machine: Windows 11, NVIDIA GeForce RTX 5080, llvm-mingw 20260922 (the release toolchain), SteamVR 2.18.4's
OpenXR runtime, PlayStation VR2 headset. No game dump was available, so the game itself was not run.

| Test | Result |
|---|---|
| Baseline (before VR changes): build | builds |
| Baseline: ctest | 606 of 607 pass; `bc_compute` fails on this GPU (native BC decode differs from the CPU reference on 7 formats; CI runners have no GPU and never run it) |
| Baseline: `--renderer-smoke` | PASS |
| With VR: build | builds; `openxr_loader.dll` built from the pinned SDK, only Windows system DLL imports |
| With VR: ctest | 607 of 608 pass (the new `vr` test passes; `bc_compute` fails as in the baseline) |
| With VR, VR off: `--renderer-smoke` | PASS |
| With VR, `WWHD_VR=1`: runtime + Vulkan binding | **verified on hardware**: runtime and headset found, Vulkan instance and device created by the runtime, the headset's GPU chosen, STAGE space, two 3224x3288 sRGB swapchains, session reached `running`; the renderer smoke test passed on that device |
| With VR, `WWHD_VR=1`: frame loop | **verified on hardware**: runtime session states ready -> synchronized -> visible -> focused; 270 of 270 frames shown and submitted with both eye images and a projection layer, 89.4 frames/s against the headset's 11.11 ms (90 Hz) display period; head tracked in every frame |
| With VR, `WWHD_VR=1`: controllers | **verified on hardware**: SteamVR bound the PS VR2 Sense controllers to the suggested Oculus Touch profile; both tracked in 204 of 270 frames (the binding arrives about 0.7 s after the session starts). Haptics were sent; whether they were felt was not checked |
| A first run where the session did not become visible in time | the smoke test reports "frame loop only" (no failure) and logs the runtime's session state |
| Game: camera, movement, sword, body, events | **not run** (no game dump on this machine) |
| Linux | **not built** in this work |

## Manual test procedure (headset and your game)

Prerequisites: your game built as in the README, the OpenXR runtime running and active, the headset awake,
the play area set up (standing) or a seated position.

1. **Start and detection.** `wwhd --vr`. The log has `[vr] OpenXR runtime`, `[vr] headset`, `[vr] session
   idle -> running`. The headset shows the title screen in front of you; the window shows the left eye.
   Start without the runtime: the log says why and the game plays on the desktop.
2. **Recentre and height.** Load a save on Outset. Look around: you see from Link's eyes, facing where he
   faces. Turn your body, hold both stick clicks 0.6 s: the view turns so you face Link's direction again.
   Crouch: the view lowers; stand on tiptoe: it rises. `vr.heightOffset` raises it (seated).
3. **Looking around.** Head rotation and leaning are immediate (no lag, no smoothing). Lean far: the view
   stops at about 45 cm from Link. Check for stereo depth (near objects, Link's hands) and no doubled HUD.
4. **Movement and turning.** Left stick: Link runs where you look; light push walks. Right stick: 30-degree
   snap turns; `vr.turn=smooth`: smooth. Turning changes only your view direction, not Link's body.
5. **Controller tracking.** Both controllers tracked; buttons per VR_CONTROLS.md (A talks, ZL targets, ZR
   shields, + pauses).
6. **Arms.** Expected limitation: Link's arms follow his animation; no head, hair or hat in front of you.
7. **Sword grip.** Draw the sword (B or a swing): the sword is in your hand, aligned with the controller;
   adjust `vr.gripPitchDegrees` / `vr.gripOffset*` until it feels right.
8. **Slow and fast swings.** Slow movements do not attack. A deliberate swing makes Link attack (combos
   continue with repeated swings). Fast swings through an enemy still hit (swept collision).
9. **Hits.** Stationary target (the training dummy / Orca's logs, Bokoblins on Outset's forest path): hits
   register where your blade is, the enemy reacts as in the original, the controller vibrates once per
   enemy per swing. Moving enemies: the same. Swinging when no attack is active (between swings) does
   nothing.
10. **Shield.** Shield-hand grip raises the shield; attacks from the front are blocked as in the original.
11. **Scene transitions and cutscenes.** Doors, loading zones, the Pirate Ship cutscenes: the view follows the
    game's camera while you can look around; after the event the view returns to Link's eyes, recentred to
    his direction after a stage change.
12. **Shutdown and restart.** Quit from the game and from the runtime (exit SteamVR while playing): the game
    continues on the desktop. Restart with `--vr`.

Report with the log (`[vr]` lines), the headset, runtime and GPU.

## Fixes (docs/VR_FIXES.md)

Status after the fixes, on the same machine. "Verified" = observed in a running game or on the headset;
"unit-tested" = `ctest -R vr`; "built" = compiles and runs without the headset, not yet seen in VR.

| Fix | Status | Evidence |
|---|---|---|
| 1 Stereo | eye images measured: both eyes render the game; menus (2D layouts) at zero disparity as expected; the right eye's sRGB flag fixed (its captures were darker) | eye captures from the game in VR (file select); **in-game 3D parallax not yet captured** (the headset left the session) |
| 2 90 Hz | built + unit-tested (`cap_fps_vr`; the desktop rule for 90 Hz monitors unchanged); mirror window without vsync in VR | **not yet measured on the headset**; check the `[vr] timing:` line |
| 3 Menu zoom-out | built + unit-tested (`menu_eye`, `camera_kind`, `panel_pose`) | not yet seen in VR |
| 4 Gameplay camera | regression test pins the transform (`test_gameplay_camera_regression`); gameplay code path unchanged | — |
| 5 Left stick | built + unit-tested (dead zone, controlled angle); hook on `dCam_getControledAngleY` compiled into the game | not yet played; check the `[vr] movement:` line |
| 6 Menu button | Plus already pressed; the GamePad menus now shown as a panel | not yet seen in VR |
| 7 Arms | built + unit-tested (`pose_arm`: reach, lengths, pointing, blending, invalid input); hook on `jointCB1` compiled into the game | not yet seen; check `[vr] arm joints` lines (bone lengths) |

Regression: full suite 607 of 608 (`bc_compute` as before), `--renderer-smoke` passes with VR off and with
`WWHD_VR=1` while no headset is connected (VR skipped, desktop play), the game builds and starts on the desktop when
the runtime has no headset.

### Headset checks for the fixes

1. `wwhd --vr` on the file select: the menu appears smaller (about two thirds of the view) and farther; captures with
   F10 save both eyes (`..._RightEye.png`) with the same brightness.
2. Load a file, stand on Outset: close objects (Link's hands, the fence) show depth; `[vr] eyes:` reports the eye
   offsets (about 6.3 game units apart for a 63 mm IPD).
3. `[vr] timing:` after 10 s: frames to the headset near its refresh rate (SteamVR's video settings decide 90/120 Hz),
   logic at 30 steps/s. Walk: the world moves at the normal speed.
4. Push the left stick forward while looking left: Link walks left; release, look right, push: he walks right. The log
   has `[vr] movement: the game's angle is the camera's heading + 0x....` once.
5. Press the menu button: the pause/collection menu appears on a panel ahead; navigate with the sticks; close it: the
   panel goes, the view is the gameplay view as before.
6. Move both controllers: Link's arms follow (left with left), elbows stay below, hands do not snap when a controller
   is covered and found again. `[vr] arm joints 6/7/8 ... 10/11/12` lines give the bone lengths (about 10 to 20 units).

### Headset results for the fixes (PS VR2 through SteamVR 2.18.4, RTX 5080, the player's European game)

Four sessions, the player in the headset; evidence from the `[vr]` log lines and both eyes' captures.

| Fix | Result on hardware |
|---|---|
| 1 Stereo | **verified**: per-region disparity in the title scene (ship's figurehead 7 px, distant sea 1 px, 2D logo 0 px) and in gameplay on Outset (near objects large, far island small); both eyes the same brightness. Found and fixed during the test: Link's hair and bangs (separate models) filled the first-person view |
| 2 90 Hz | **verified**: 89.9 frames/s to the headset at 90 Hz on the title and menus, logic 30.0 steps/s in every 10 s window (no speed-up); gameplay on Outset 48 to 70 frames/s (performance bound: an in-between frame takes 10 to 21 ms with both eyes) |
| 3 Menu zoom-out | not compared side by side (needs a before/after look in the headset) |
| 4 Gameplay camera | recentre and the head-tracked camera ran in gameplay; no regression reported |
| 5 Left stick | **verified**: input reached the game (stick values in the log), Link walked from the cliff onto the pier; the controlled-angle hook found the offset 0x0000 |
| 6 Menu button | **verified**: Plus reached the game, the GamePad menu panel appeared (twice) |
| 7 Arms | joint check passed (both arms 17.0 / 18.3 units); a hand is visible in first person; following the controllers **not yet confirmed** |
| Session loss | **verified**: SteamVR ended the session; the game continued on the desktop and restored the frame rate settings |

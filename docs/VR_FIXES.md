# VR mode: fixes after the first headset tests

Seven problems were reported from the first play test (SteamVR, PS VR2, RTX 5080, the European game).
This document records what was found, what changed, and how each fix is checked. Architecture:
[VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).

## Baseline

- Branch `vr-openxr`, two commits on top of v0.4.0; no uncommitted changes before this work.
- The game was built from the player's extracted game (`code/cking.rpx` = European version 0) with the release
  toolchain (llvm-mingw); the VR unit tests and the full suite were green except `bc_compute` (GPU-specific,
  pre-existing).
- Tools used to measure: `WWHD_TEST_SCREENSHOT=frames` (with VR, every screenshot also saves the right eye as
  `..._RightEye.png`), scripted presses (`WWHD_PRESS`), the `[vr]` log lines.

## 1. Stereo in both eyes

**Measured.** Both eye images were captured from the running game (file select). Both eyes render; layouts
(2D menus) have zero disparity between the eyes, as they should (the existing stereo renderer keeps
layouts at screen depth). The right eye's capture was much darker than the left (mean 111 vs 44).

**Causes found.**
- `stereoRight.srgb` (the right eye's "the scan holds sRGB values" flag) was never set: only the TV and
  GamePad screens get it from `GX2SetTVBuffer`. Anything that composes the right eye by its own flag (the
  eye capture, the present compose path) applied the transfer curve twice. The headset copy itself was
  correct (a blit between sRGB-typed images), so this explains the capture, not the headset.
- Everything the player sees in menus and on the title screen is head-locked 2D at screen depth covering the
  whole field of view (no depth at all), and the item / pause / map menus were not shown in the headset at all
  (they are on the GamePad screen, see 6). A large part of what read as "no 3D" was these screens.
- The eye pictures are drawn at the game's internal resolution (1280x924 at 1x in VR) and stretched to the
  headset's 3224x3288 per eye: blurry, which weakens the depth impression. Raise Graphics > Internal resolution.

**Changes.** The right eye takes the TV's sRGB flag. Per-eye diagnostics (`[vr] eyes:` every 10 s: each eye's
offset in game units and millimetres, the rendered field of view, the swapchain size, how many frames carried
the game camera's views). Gameplay capture is part of the test below.

## 2. 60 fps cap, 90 Hz

**Causes.**
1. Frame interpolation is off by default: the game draws 30 frames per second.
2. Interpolation caps its rate to the **desktop monitor's** refresh rate (`interp::display_hz`, 60 Hz here), and
   `pacing::cap_fps` treats every display below 118 Hz as 60 Hz.
3. The mirror window presents with FIFO (vsync) on that 60 Hz monitor; each present blocks the render thread
   for up to a monitor refresh, so the headset could never get more than 60 frames.

**Changes.** While a VR session runs: the headset's refresh rate (from OpenXR's predicted display period) is the
display rate interpolation plans for, and rates from 88 Hz up allow 120 fps interpolation (paced: in-between
frames that do not fit are skipped, the 30 Hz game logic keeps its speed); interpolation is switched on at 120 fps
for the session if it was off (`vr.autoFrameRate`, not saved); the mirror window presents without waiting for
the monitor (mailbox, else immediate). `XR_FB_display_refresh_rate` is used when the runtime offers it
(`vr.refreshRate`, 0 = the runtime's choice). `[vr] timing:` every 10 s: frames submitted per second, the
headset's rate, the game's swaps and logic steps per second (the logic must stay at 30).

## 3. Menus zoomed out by 50 %, 4. the gameplay camera unchanged

**Causes.** Menu scenes (title `sea_T`, file select `Name`) used the event camera path: the game's camera
position with head rotation, and their 2D layouts fill the whole field of view.

**Changes.** A separate menu path (`vr_camera.h`): in menu scenes the game camera's distance to its target is
multiplied by `vr.menuZoomOut` (1.5) along its own direction, and the TV layouts are drawn at 1/1.5 of their size
(same centre). The GamePad-screen menus (6) are shown on a panel 1.5 times the base distance. Gameplay keeps the
same code path; its transform moved unchanged into `gameplay_head()` with a regression test pinning its output.

## 5. Left stick movement

**Cause.** Link turns the stick into a world direction with `dCam_getControledAngleY(camera)` (024F8018):
the camera's *controlled* angle, which the game holds while the stick is pushed so Link does not spiral as the
follow camera swings round. The VR code remapped the stick against the *drawn* camera direction instead; the two
drift apart while moving, so Link curved away from where the player looked. There was also no dead zone.

**Changes.** A hook on `dCam_getControledAngleY`: during VR gameplay it returns the head's (or the left hand's)
horizontal heading in the game's convention (the constant between the game's angle and the camera's view heading
is measured at run time); the stick goes to the game unchanged except for a radial dead zone (`vr.stickDeadZone`).
Swimming, climbing, sailing and every movement restriction stay the game's.

## 6. Menu button

**Cause.** The pause, item, map and bag menus of WWHD are on the GamePad screen. The headset only showed the TV
picture, so Plus opened the menu on a screen the player could not see.

**Changes.** The GamePad screen is shown in the headset as a panel (an OpenXR quad layer) while a GamePad menu is
open (`vr.gamepadScreen=menus`, default; `always`, `off`), using the menu state the port already reads for its
automatic picture-in-picture. The menu button stays Plus (the game's own edge detection, so holding it does not
repeat). Profiles without a menu button: see VR_CONTROLS.md.

## 7. Arms follow the controllers

**Cause.** The IK solver existed but was not applied to Link's skeleton.

**Changes.** `daPy_lk_c::jointCB1` (02403150) runs right after `mpCLModel->calc()` in Link's execute and writes
joint matrices (the game's own foot IK; in HD `setAnmMtx` marks the matrices changed so the skin is rebuilt from
them); everything after it (hand positions, the hands model, the items' attachment) reads them. A hook after it
poses each arm (`vr_arm.h`; joints upper arm 6/10, forearm 7/11, hand 8/12, the numbers checked at run time from
bone lengths and logged) towards the controller on the same side:
- the wrist goes to the controller's offset from the player's shoulder (estimated from the head), scaled by Link's
  arm length over the player's (`vr.realArmLength`), from Link's own shoulder: the arm stays attached and its reach
  plausible; the solver clamps unreachable targets (no stretching);
- the elbow stays on the side the animation bends it to (no flips);
- the hand turns with the forearm, then so its pointing axis points where the controller points (its roll about
  that axis is not taken from the controller);
- tracking that starts or stops blends over 0.15 s; an inactive controller leaves the animation (no snapping to
  the origin); `vr.armIK=0` turns it off.

The sword stays where the first version put it: drawn at the sword hand's controller (`vr.trackedSword`) and swept
from it into the game's attack collision. Link holds his sword in his *left* hand; tying the sword to his hand would
cross the arms of right-handed players. With `vr.leftHanded=1` the sword controller and Link's sword hand are the
same.

The arms are posed once per logic step (30 Hz) and drawn with the game's frame interpolation; the hands therefore
lag the controllers by about a step.

## Test matrix

See VR_TESTING.md, section "Fixes".

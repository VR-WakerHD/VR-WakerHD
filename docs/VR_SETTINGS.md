# VR settings

Every VR setting, its unit, range and default. The definitions live in one place,
`runtime/src/vr/vr_config.h` (numbers, ranges) and `runtime/src/vr/vr_bindings.h` (buttons); the game, its
settings overlay (F1) and the VR launcher ([VR_LAUNCHER.md](VR_LAUNCHER.md)) all use them.

The defaults are the settings the port was tuned with in the headset (PS VR2 through SteamVR, 2026-10-25): smooth
turning, Detached Arms, the world at `vr.unitsPerMetre` (not sized to Link), a 90 Hz refresh, the menu panel at
1.7 m, and the game starting in VR when a headset is there. A settings file that already has a key keeps its value.

## Where they are stored

In the game's own settings file, `settings.ini`, as `key=value` lines:

| Install | File |
|---|---|
| Release folder (portable, `portable.txt`) | `<release>/data/user/settings.ini` |
| Windows, source build | `%APPDATA%\WWHD\settings.ini` |
| Linux, source build | `$XDG_CONFIG_HOME/wwhd/settings.ini` (default `~/.config/wwhd/settings.ini`) |
| Linux AppImage | `~/.config/wwhd/settings.ini` |
| any | `WWHD_USER_DIR` (folder) or `WWHD_SETTINGS` (file) override |

The VR launcher keeps its own state (the game's path) in `vr-launcher.ini` next to it.

**Format and versioning.** `vr.format=1` is written with the VR settings. A file without it (older versions) reads the
same way; a file from a later format keeps every key this version knows. Unknown keys are ignored and kept;
unreadable values (`vr.menuDistance=abc`, `nan`, `1.5x`) keep the default (the launcher lists them); numbers outside
their range are clamped to it; the swing end speed is kept below the start speed. A key written twice takes the
last value, as the game reads it.

**Who writes what.** The launcher rewrites only `vr.*` keys and the four game graphics keys it shows (`resScale`,
`fxaa`, `aniso`, `perfOverlay`, only when changed there); all other lines stay as they are, in their place, through a
temporary file that replaces the old one. The game rewrites the whole file (from its in-memory copy) whenever one
of its settings changes, so the launcher does not save while a game it started is running.

**When changes apply.**
- From the launcher: at the game's next start.
- From the game's settings overlay (F1 > VR headset): at once, except `vr.enabled` (next start) and
  `vr.renderScale`, `vr.refreshRate`, `vr.autoFrameRate` (the next VR session, i.e. the next start).
- The menu panel's settings apply the next time the panel opens.

## General

| Key | Default | Range / values | Meaning |
|---|---|---|---|
| `vr.enabled` | 1 | 0, 1 | start in VR (`--vr` / `--no-vr` / `WWHD_VR=1\|0` override for one start) |
| `vr.format` | 1 | | settings format version (written, not edited) |
| `vr.eyeCrop` | on | auto, on, off | each eye submitted as its own field of view (on: every runtime; auto: all but SteamVR on Windows); fixes the bubble look on Quest 3 / VDXR (docs/VR_MENU.md) |
| `vr.texturePacks` | 1 | 0, 1 | runtime texture packs (a `textures` mod package from Texture Studio, docs/mod-manager.md) are used; does nothing unless such a package is enabled; applies at the next start; set in `settings.ini` |
| `vr.texturePackBudgetMB` | 0 | 0 .. 65536 | MiB of video memory the replacement images may take; 0 = automatic (half of the graphics card's memory budget, no more than is free once the VR session runs, at least 1 GiB); set in `settings.ini` |

## Menu panel (the GamePad screen: pause, item, map and bag menus)

| Key | Default | Range / values | Unit | Meaning |
|---|---|---|---|---|
| `vr.gamepadScreen` | menus | menus, always, off | | when the panel shows |
| `vr.menuPlacement` | gaze | gaze, head, front | | where you look when it opens (then fixed in the room); following the head; ahead of the recentred forward |
| `vr.menuDistance` | 1.7 | 0.4 .. 4 | m | from the head to the panel |
| `vr.menuHeight` | -0.08 | -0.8 .. 0.8 | m | above (+) / below (-) eye level |
| `vr.menuScale` | 1 | 0.25 .. 3 | x 0.8 m | the panel's width |
| `vr.menuCurvature` | 0 | 0 .. 1 | | 0 flat, 1 curved around the head (needs `XR_KHR_composition_layer_cylinder`; flat otherwise) |

The menu distance is its own setting: it never moves the gameplay camera, and the camera settings never move the
panel. See [VR_PAUSE_MENU.md](VR_PAUSE_MENU.md).

## Camera (menu scenes, cutscenes)

| Key | Default | Range | Unit | Meaning |
|---|---|---|---|---|
| `vr.menuZoomOut` | 3 | 1 .. 4 | x | title and file select: the game's camera this much farther from what it frames, their layouts this much smaller |
| `vr.cutsceneZoomOut` | 1 | 1 .. 3 | x | cutscenes: the game's camera this much farther from what it frames |
| `vr.cutsceneFollow` | 1 | 0, 1 | | cutscenes: the game's camera with your head's rotation (0: a flat, head-locked picture) |

The gameplay camera (your head at Link's eyes) has no zoom setting.

## Body, comfort and locomotion

| Key | Default | Range / values | Unit | Meaning |
|---|---|---|---|---|
| `vr.heightOffset` | 0 | -1 .. 1 | m | added to your calibrated eye height (seated play: raise it) |
| `vr.unitsPerMetre` | 100 | 40 .. 250 | game units / m | world scale when `vr.matchLinkBody` is off (the launcher's and Settings > VR's World scale slider: moving it sets this and turns the match off) |
| `vr.matchLinkBody` | 0 | 0 / 1 | | world scale = Link's arm / `vr.realArmLength` (docs/VR_ARM_ANIMATION.md) |
| `vr.maxHeadOffset` | 1.0 | 0.05 .. 2 | m | how far you may lean or walk from Link before the view stops (walls stop it sooner) |
| `vr.eyeForward` | 0 | -0.3 .. 0.3 | m | first person: the view in front of (+) or behind (-) Link's eyes (the game's eye point) |
| `vr.roomScale` | 1 | 0, 1 | | first person: leaning or walking in your play space moves Link with you, his body under the view (while he stands or walks on the ground) |
| `vr.startThirdPerson` | 0 | 0, 1 | | normal play starts in the third-person view (the right stick click toggles) |
| `vr.hudInset` | 0.3 | 0.15 .. 1 | | normal play: the game's edge HUD this share of the way from the view's centre to the edges (docs/VR_HUD.md) |
| `vr.hudSize` | 0.5 | 0.2 .. 1.5 | | normal play: the game's edge HUD at this share of its size |
| `vr.shieldOffsetX`, `vr.shieldOffsetY`, `vr.shieldOffsetZ` | 0, 0, -0.13 | -0.4 .. 0.4 | m | the shield moved in the off-hand controller's grip space from its place (+x right, +y up, +z towards you); right-handed play only |
| `vr.stickOffsetX`, `vr.stickOffsetY`, `vr.stickOffsetZ` | 0.03, 0, 0.10 | -0.3 .. 0.3 | m | a picked-up weapon (the stick) moved in the controller's grip space from its handle at the hand (+x right, +z towards you); right-handed play only |
| `vr.swordWristStraight` | 1 | 0 .. 1 | | the sword hand's wrist in line with the forearm (1) or as the controller holds the sword (0) |
| `vr.swingAcrossDistance` | 0.12 | 0.05 .. 0.6 | m | a side slice attacks once the hand has carried this far across |
| `vr.swingDownDistance` | 0.15 | 0.05 .. 0.6 | m | a chop attacks once the hand has carried this far down (`vr.swingFullDistance` and `vr.swingFullDegrees` are no longer used) |
| `vr.armHearts` | 1 | 0, 1 | | the wrist HUD's hearts on top of the left wrist ([VR_WRIST_HUD.md](VR_WRIST_HUD.md)) |
| `vr.wristItems` | 1 | 0, 1 | | its item buttons (X, Y, R) under the left wrist |
| `vr.wristRupees` | 1 | 0, 1 | | its rupees on top of the right wrist |
| `vr.wristHudAlong` | 0.07 | -0.05 .. 0.2 | m | the panels' place from the controller's grip towards the elbow |
| `vr.wristHudLift` | 0.035 | 0 .. 0.1 | m | and off the wrist's axis |
| `vr.wristHudScale` | 1 | 0.5 .. 2 | | their size |
| `vr.turn` | smooth | snap, smooth | | right-stick turning |
| `vr.snapDegrees` | 30 | 10 .. 90 | degrees | snap turn |
| `vr.smoothDegreesPerSecond` | 120 | 20 .. 360 | degrees/s | smooth turn at full stick |
| `vr.moveDirection` | head | head, hand | | left stick forward: where you look / where the left controller points |
| `vr.moveSpeed` | 1 | 0.2 .. 1 | x | left stick scale |
| `vr.stickDeadZone` | 0.15 | 0 .. 0.6 | | radial dead zone of both sticks |
| `vr.batonHands` | on | | | conducting with the Wind Waker, the hands play it too: the left the notes, the right the beat ([VR_WIND_WAKER.md](VR_WIND_WAKER.md)) |
| `vr.batonHandReach` | 0.12 | 0.05 .. 0.4 | m | how far a hand is held out from where it was when conducting began for a direction (held until within 60 % of it) |
| `vr.showBody` | 0 | 0, 1 | | show Link's head in first person |
| `vr.posture` | standing | standing, seated | | recorded only (no effect yet; use the height offset) |

Recentring: both stick clicks held 0.6 s, the settings overlay's Recentre button, or automatically at each stage
change.

## Hands, sword, arms

| Key | Default | Range | Unit | Meaning |
|---|---|---|---|---|
| `vr.leftHanded` | 0 | 0, 1 | | the sword in the left hand |
| `vr.armIK` | 1 | 0, 1 | | Link's arms follow the controllers |
| `vr.detachedArms` | 1 | 0, 1 | | Detached Arms: the hands reach where the controllers are even beyond Link's shoulders' reach (the arm moves out from the shoulder), the wrists turn as far as the controllers; at once, also while playing ([VR_ARM_ANIMATION.md](VR_ARM_ANIMATION.md)) |
| `vr.realArmLength` | 0.6 | 0.3 .. 1 | m | your shoulder-to-hand length (scales your reach to Link's) |
| `vr.trackedSword` | 1 | 0, 1 | | the sword drawn at the sword hand's controller |
| `vr.physicalSword` | 1 | 0, 1 | | while the sword hand swings, the blade hits and cuts along its tracked path without Link's attack animation (TPVR's physical sword); full swings still attack |
| `vr.itemWheel` | 1 | 0, 1 | | the item wheel: reach over the shield hand's shoulder, choose with the right stick, bring the hand forward to put the item on a button ([VR_ITEM_WHEEL.md](VR_ITEM_WHEEL.md)) |
| `vr.itemWheelButton` | x | x, y, r | | the item button the wheel puts its choice on |
| `vr.bodyFollowDegrees` | 30 | 10 .. 90 | degrees | standing still in first person, Link turns to face where you look once your head has turned this far from his body; he turns until within 5 degrees ([VR_BODY_FOLLOW.md](VR_BODY_FOLLOW.md)) |
| `vr.gripOffsetX/Y/Z` | 0, 0, -0.04 | -0.3 .. 0.3 | m | the sword's root in the controller's grip |
| `vr.gripPitchDegrees` | -40 | -90 .. 90 | degrees | the sword's tilt from the grip |
| `vr.bladeLength` | 0.85 | 0.3 .. 1.5 | m | blade length for swing detection |
| `vr.swingStartSpeed` | 2.6 | 0.5 .. 10 | m/s | tip speed that starts a swing |
| `vr.swingEndSpeed` | 1.2 | 0.2 .. 8 | m/s | tip speed that ends it (below the start speed) |
| `vr.swingMinTime` | 0.05 | 0 .. 0.3 | s | time above the start speed before it counts |
| `vr.swingCooldown` | 0.25 | 0 .. 1.5 | s | before the next swing may start |
| `vr.hapticStrength` | 0.6 | 0 .. 1 | | vibration on hits |

Button bindings (`vr.bind.*`): [VR_CONTROL_MAPPING.md](VR_CONTROL_MAPPING.md).

## Headset and performance

| Key | Default | Range | Unit | Meaning |
|---|---|---|---|---|
| `vr.refreshRate` | 90 | 0 .. 240 | Hz | ask the headset for this rate where the runtime lets applications choose (`XR_FB_display_refresh_rate`); 0 = the runtime's choice |
| `vr.renderScale` | 1 (0.65 on a standalone headset) | 0.5 .. 2 | x | the headset resolution: the eye pictures' size relative to the runtime's recommended eye image (see "Headset resolution") |
| `vr.autoFrameRate` | 1 | 0, 1 | | frame interpolation on (120 fps, paced) for the session when it is off; not saved |
| `vr.debugCollision` | 0 | 0, 1 | | reserved (the collision visualisation is not implemented) |

The game's own graphics keys the launcher also shows: `resScale` (internal resolution 1, 1.5, 2, 3), `fxaa`, `aniso`,
`perfOverlay` (the performance overlay).

## Headset resolution (`vr.renderScale`, 2026-11-08)

The eye pictures are drawn at the runtime's recommended eye image size times `vr.renderScale` (the VR tab's
"Headset resolution"; the launcher's slider), not at the internal resolution: `[vr] headset resolution 100% of the
runtime's 3400x3468: eye pictures 3400x3468` at the session's start.

- **PC VR** (default 1): the headset's images are that size too, so the picture is copied 1:1, no stretch either way
  (before: 1280x720 times the internal resolution and an aspect stretch, e.g. 3840x2771 resampled into 3400x3468).
  Above 1 the picture is shrunk into the images (smoother edges, more GPU); below 1 it costs less.
- **Standalone headsets** (Quest, Steam Frame; default 0.65, the user's choice: 1092x1144 on a Quest 3, 1.4x the
  pixels the 0.75x internal resolution drew, sharper for some GPU time; format 18 moves format 17's 0.55 to it): the headset's images keep the runtime's size and the smaller picture is upscaled into them (Quest
  Super Resolution at the picture's own size, docs/VR_UPSCALING.md; SGSR1).
- Quick choices beside the slider (2026-11-09; `vr::kHeadsetResolutionPresets`): 0.65x, 0.70x, 0.75x and 1.00x on a
  standalone headset (Quest 3: 1092x1144, 1176x1232, 1260x1320, 1680x1760), 0.75x to 1.50x on a PC. The slider
  keeps the value to 0.05 steps (`snap_headset_resolution`), so a controller's drag lands on one.
- How (`gfx/vulkan/surfaces.cpp` `latch_headset_resolution`): with each eye's own projection the screen targets' aspect
  factors become the picture's size over the game's 1280x720 at the internal resolution. The internal resolution still
  sizes what is not screen-shaped (shadow maps, the light atlas); dynamic resolution still lowers the picture. With the
  shared projection (`WWHD_VR_EYE_PROJECTION=shared`) or `WWHD_VR_HEADSET_RES=0` the pictures follow the internal
  resolution as before.
- Settings format 17: a standalone headset's saved `vr.renderScale` (which sized only its images) goes to the new
  default (format 18: and a saved 0.55, format 17's default, to 0.65); a PC's is kept (its images keep their size, the pictures now match them).
- Applies at the next VR start. Not yet seen in a headset.

# VR wrist HUD

Four small panels on your wrists, the only HUD the VR view shows of the game's (besides the action prompt and
dialogue: [VR_UI.md](VR_UI.md)):

| Panel | Where | Shows |
|---|---|---|
| Hearts | on top of the left wrist (the back of the hand's side), level and turned to your eyes | one row of hearts, as many as Link's maximum, filled by quarters |
| Item buttons | under the left wrist (the palm's side) | the items on X, Y and R with their letters; an empty button dimmed |
| Magic | on top of the right wrist, level and turned to your eyes | the game's green meter across its whole box, filled by the magic left; shown once Link has magic |
| Rupees | under the right wrist (the palm's side), level and turned to your eyes | a rupee and the count, left to right |

Each panel shows while it faces your eyes (within 45 degrees, staying up to 60) and is within 0.9 m of them, in
first-person normal play: not in third person (it shows the game's HUD), events, cutscenes, menus or the telescope. Each can be switched off (`vr.armHearts`,
`vr.wristItems`, `vr.wristMagic`, `vr.wristRupees`), and all four moved and sized together (`vr.wristHudAlong`, `vr.wristHudLift`,
`vr.wristHudScale`). They are in the launcher (Wrist HUD) and the in-game menu.

**Straight and readable** (asked for in the headset): the hearts (17 cm wide) and the rupees (11.5 cm) sit nearer the
wrist (a third of `vr.wristHudLift`), but are turned to your eyes with their row level (`level_facing`), so a rolled wrist does not tilt
them. When they show is still decided by the wrist turning towards you. The item buttons follow the wrist.

## How

- **What it shows is the game's:** read from the save at every pad read (`vr_game.cpp` `update_wrist_hud`). That is
  the life and maximum in quarter hearts (`+0x22`, `+0x20`), the rupees (`+0x24`), and the item numbers the game has on
  X, Y and R (play's `mSelectItem`, `+0x5BBB`). Nothing is kept apart from the game, so it updates as soon as the game
  changes them. The icons are the game's own (BtnItemIcon_00, as the item wheel's), by the game's icon table.
- **Where it goes:** from the controllers' grip poses in tracking space (OpenXR's grip: -Z forward through the fist,
  +X normal to the palm, out of the left palm and into the right one), not from Link's animated arm
  (`vr_wrist_hud.h` `wrist_panel_pose`). Each panel sits `vr.wristHudAlong` towards the elbow and `vr.wristHudLift`
  off the wrist's axis. It faces out of the back of the hand (hearts, rupees) or the palm (items), with its up along
  the forearm towards the fingers. Its frame is right-handed on either hand, so text is never mirrored: turn a wrist
  towards your eyes and it reads left to right. No smoothing: it is exactly where the controller is.
- **How it is drawn:** one picture (512 x 560) with a part for each panel, drawn on the CPU (`vr_canvas.h`, shared
  with the item wheel) only when what it shows changes (`vr_wrist_hud.cpp`). The render thread copies it into its
  swapchain (`vulkan/vr_vulkan.cpp`), and the session submits one OpenXR quad layer per shown panel, each a part of the
  picture, blended by its alpha (`openxr_session.cpp`, `kWrist`).
- **What it replaced:** the game's own HUD panes moved onto Link's arms (aspect.cpp's wrist panes and their owners,
  gx2_core.cpp's projection for them, the renderer's eye offset for layout panes, vr_game.cpp's wrist frames from his
  posed hand joints). That path depended on the game's layout timing, its duplicate panes and his animation, and
  showed duplicates and flicker. It is gone; the game's gameplay HUD is simply not shown in VR.

## Tests

`vr_test` `test_wrist_hud`:
- hearts by quarters and their count;
- the picture's parts;
- each panel's side, up, handedness and place, and both wrists reading the same way;
- a moved grip;
- facing, with its hysteresis.

**Not checked without a headset:** how they look and read on your wrists, their sizes, and the angles they show at.

## The magic bar, and the rupees under the right wrist (2026-11-09)

Asked for: the magic meter on the wrist HUD, on top of the right wrist as the hearts are on the left, and the rupees
moved to the inside of the right wrist. The magic is the save's (`+0x34`, its maximum `+0x33`: 0 before the first
meter, 16, 32 doubled), read with the rest at every pad read; the panel shows only once the maximum is above 0. It is
drawn as the game's meter, a green bar with a white rim, across the whole box whatever the maximum (`magic_bar_length`;
at first half the box for the first meter, as the game draws it, which left the box half empty in the headset) and
filled by the magic left (`magic_bar_fill`), 17 cm wide like the hearts and as near the wrist. The rupees face out of the right palm
(`wrist_panel_pose`: -X in the right grip's space) at the items' full lift, still level and turned to your eyes. Its
switch is `vr.wristMagic` (on), in the launcher and the in-game menu. Not yet tried in the headset.

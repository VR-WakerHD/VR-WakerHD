# VR: the game's HUD in normal play

## Which HUD shows where

| State | Hearts, item buttons, rupees | Action prompt | Other gameplay HUD |
|---|---|---|---|
| VR, first or third person | the wrist HUD, while a wrist faces you ([VR_WRIST_HUD.md](VR_WRIST_HUD.md)) | the game's, drawn `vr.hudInset` of the way to the edges | not shown ([VR_UI.md](VR_UI.md)) |
| Events, cutscenes, conversations | not shown | the game's own rules | not shown; dialogue text smaller around the centre (`vr.textZoomOut`) |
| Menus (GamePad panel), telescope | not shown | unchanged | not shown |
| Not VR | the game's | the game's | the game's |

There is one camera mode state: `g_thirdPerson` in `vr_game.cpp`, toggled by the right stick click, with the start
view from `vr.startThirdPerson`. The arm hearts (`vr::game::arm_hearts`) and the HUD inset (`vr::game::hud_inset`) both
read it, together with the game's own state (`aspect::gameplay_camera()`: not an event, cutscene, title or file
select), the telescope and the menu panel. No separate HUD flag is kept, so switching views changes both at the HUD's
next layout calculation, with no frame in between.

## Third person: the HUD was outside the lenses

Nothing in the VR code hid the HUD in third person. Captures show the game's complete HUD in the eye picture: hearts,
item buttons and their prompts, rupees. The problem was where it was drawn. The HUD keeps its place at the picture's
edges (`aspect.cpp`, the `Hud` rule in `aspect_panes.h`), and in VR the eye picture spans the headset's whole field of
view (123 x 106 degrees on the test headset). The hearts were about 59 degrees left and 51 degrees up from the view's
centre and the buttons and rupees were in the other corners, outside what the lenses show. In the headset the HUD
looked missing.

Now, in normal play (first and third person; reported in the headset: the HUD was not in view), the HUD panes at the edges (root children with the `Hud` rule) are moved
`vr.hudInset` (default 0.3, range 0.15 to 1) of the way from the view's centre to where the game puts them
(`aspect.cpp` `calculate_aspect_pane`, `vr::game::hud_inset`). At 0.3 the corners are about 27 and 20 degrees from the
centre. The first default, 0.5 (about 43 and 34 degrees), was still out of view in the headset. They are also drawn at `vr.hudSize`
(default 0.5, range 0.2 to 1.5) of their size: at the game's size, in place, the HUD looked too large in the headset.
In VR the game's gameplay HUD is not shown any more: the wrist HUD ([VR_WRIST_HUD.md](VR_WRIST_HUD.md)) shows the hearts, items and rupees, and the action prompt keeps this placement.

What stays the same:
- the HUD's animations and contents (the game's own: life, items, rupees, prompts);
- panes the game places at actors (targeting markers) and everything without the `Hud` rule;
- events, menus and the telescope (`hud_inset` is 1 there); in first person the hearts stay on the arm;
- the desktop and non-VR play.

The setting is in the launcher (Camera and comfort > Player camera) and in Settings > VR.

## Tests

- **Captures** (`WWHD_VR_TEST_THIRD_PERSON=1`) before the change: the complete HUD in both eyes, at the picture's
  corners.
- **Log:** `[vr] HUD: drawn 30% of the way to the view's edges at 50% of its size`.
- **Confirmed in the headset** (by the player): the HUD's position (`vr.hudInset` 0.3) and size (`vr.hudSize` 0.5).
- **Still to check:** after switching views both ways, and while taking damage or changing items.

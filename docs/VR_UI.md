# The VR view's UI

What of the game's 2D UI the VR view shows, and where, is decided in one place: `runtime/src/vr/vr_ui.h` (pure:
`classify`, `action`) and `vr_ui.cpp` (the live `state`). `aspect.cpp` carries the decisions out while
it calculates the layouts' panes. The renderer applies the layout mode, the smaller drawing in events and menus and
the telescope's eyepiece (`gx2_core.cpp`, `vulkan/draw.cpp`, `vulkan/vr_vulkan.cpp`).

## What the VR view shows

| UI | Where | Code |
|---|---|---|
| Hearts, item buttons, rupees | The wrist HUD: three panels on the wrists | `vr_wrist_hud.h`, [VR_WRIST_HUD.md](VR_WRIST_HUD.md) |
| The action prompt ("Speak", "Open", "Climb") | The game's, drawn `vr.hudInset` of the way to the view's edges | `vr_ui.h` (`ActionPrompt`), `vr_game.cpp` `hud_layout` |
| Dialogue and cutscene text | Smaller, near the middle of the view (`vr.textZoomOut`, `vr.textHeight`) | `vr_game.cpp` (layout mode), `vr_ui.h` (`MessageAnchor`) |
| The telescope's eyepiece | Full size, covering the eye | `vr_game.cpp` (layout mode) |
| The GamePad's menus | A panel in front of you (`vr.gamepadScreen`) | an OpenXR quad layer |
| The item wheel | Over the shield hand's shoulder | an OpenXR quad layer |

## Not shown

**The game's gameplay HUD**, in VR, first and third person, in play and in events: every pane with an aspect HUD
rule, and everything inside one (`vr_ui.h` `classify`, by the pane and its parents). That covers the hearts and
magic, the rupees, the item buttons and their prompts, keys, the compass and clock, arrows, timers, gauges and the
place name.
- The wrist HUD shows the hearts, items and rupees instead.
- The action prompt and the dialogue's box are kept.
- The boat's views, the Red Lion's and the Free Roam Camera (docs/VR_SAILING_VIEWS.md), show none of the game's HUD
  and not the action prompt either (`State::boatView`); the Free Roam Camera has no label any more (its "Free Roam"
  label, `vr_wrist_hud.h` `kWristLabel`, off since 2026-11-06: `vr_sail_view.h` `kFreeRoamLabel`) and Link is not drawn there.
- Hidden panes are calculated with alpha 0 (the game's alpha again after), so nothing else of theirs changes.
- `[vr] HUD: <element> not shown in VR` names each one the first time.
- Without VR, the game's HUD is untouched.

**The telescope's overlay** (Telescope_00: its round mask, zoom gauge and stick prompts) only while you look through
it. Putting the telescope away, the game fades the overlay out while its camera returns (about 2.6 s), and it showed
shrunk in the middle of the view meanwhile (seen in the headset). It was still seen after the first fix, as a small
frame in the middle. The game draws the overlay again while it fades without calculating it, so it kept its last look.
So what the VR view does not show is not drawn either (`aspect.cpp`, the `Pane::Draw` hook): for the HUD and the
overlay alike.

**The 2D particles** (`dPa_control_c::draw` groups 2Dfore 7 and 2Dback 8: the HUD's sparkles, a rupee's or a button's
twinkle, screen-space flashes), in VR. Drawn in the picture's 2D space, they floated in the view as stars where the
HUD was. The world's particles and the menus' are drawn (`vr_ui.h` `draws_particle_group`, `mods/vr_hooks.cpp`).

**Third person** shows the game's own HUD, as on the TV (drawn `vr.hudInset` of the way to the edges), without its
sparkles (they flashed in the view as stars there too, seen in the headset), and no wrist HUD. First person shows the wrist HUD and not the game's HUD.

## The view toggle

A short click of the right stick toggles first and third person in normal play. A click while the stick is pushed,
or while Link climbs, is ignored: a click while turning or moving had switched the view by accident.
`[vr] view: the right stick's click ignored (...)` logs it.

## Tests

`vr_test`:
- `test_vr_ui`: the areas from a pane and its parents (including look-alikes), and the actions with no VR session,
  in play and in events;
- `test_wrist_hud`: the wrist HUD.

# VR sailing views (the King of Red Lions)

While Link sails the King of Red Lions on the Great Sea, the right stick's click offers two more views after first and
third person, the view on the **King of Red Lions' neck** and the **Free Roam Camera**:

`First person → Third person → Red Lion's neck → Free Roam Camera → First person`

Everywhere else, the click still switches only between first person and third person, as in docs/VR_CONTROLS.md
("Third-person view").

History: the boat had three views (in front of the mast, at the side of the boat, on the King of Red Lions' neck),
switched off on 2026-11-03 at the player's request (`kBoatViews`). On 2026-11-06 the side view was replaced by the Free
Roam Camera (`kFreeRoamView`), the mast view was deleted, and the neck view was turned on again (`kNeckView`) before
the Free Roam Camera in the cycle.

Status: built and covered by `vr_test`. **Not yet tried in the headset** (headset checklist below).

## The Free Roam Camera (`vr_sail_view.h` `FreeRoamCamera`, `kFreeRoam`)

| control | in the Free Roam Camera |
| --- | --- |
| left trigger | zooms in: the camera moves towards the boat's middle along the line it started on, faster the harder it is pulled |
| right trigger | zooms out, the same way |
| right stick | left and right slide the camera sideways; up moves it forward along the boat (past Link, towards the bow), down back towards the stern; it never turns. Taking out the bow, the boomerang or the hookshot switches to first person (their motion controls) |
| left stick | steers the boat as always (the game's tiller reads the stick itself, not the camera: `d_a_ship_exec.cpp` `mStickMAng`) |
| right stick click | the next view (first person) |

(The neck view comes before it: the click in the neck view goes to the Free Roam Camera.)

- **The start.** 300 units behind and 250 above the boat's middle on its deck (halfway between the mast and Link's seat),
  facing the bow, level. You look down with your head to see the boat; the view itself is never tilted.
- **Following the boat.** It moves and turns with the boat as drawn (blended between logic steps), but not with its
  pitch or roll (`boat_level_to_world`): the horizon stays level, and a camera far behind the boat does not bob with
  the waves.
- **The limits.** The zoom goes from 120 units (clear of the boat) to 2000; the slide 1500 units each way. Zoom 600
  units a second and slide 400 at full trigger or stick; a trigger resting below 10 % does nothing; both triggers at
  once cancel out.
- **Your head.** Tracked in full, as in the other boat views (`HeadController::boat_view_pose`).
- **The triggers and the stick there only.** While the Free Roam Camera is shown, neither trigger's button reaches the
  game (no lock-on, no item wheel) and the right stick's crouch and camera turn are not given. In first and third
  person on the boat, and everywhere else, the triggers and the right stick are as before.
- **No HUD, no label, no Link.** The game's HUD and its action prompt are not shown there (`vr_ui.h`
  `State::boatView`, as in the Red Lion's view); menus and dialogue are as always. Link's whole body is not drawn
  there in normal play (his model, what he holds, his shadow: `mods/vr_hooks.cpp` daPy_Draw skipped,
  `vr_sail_view.h` `free_roam_hides_link`; asked for, 2026-11-06); his logic runs as always, and an event or a talk
  shows him again. The red "Free Roam" label shown at the top of the view until then is not shown any more
  (`kFreeRoamLabel` false; its drawing, `vr_wrist_hud.h` `kWristLabel`, is kept, unused).
- **The reset.** Every time it begins (clicked into, or after sailing ended and you click back) it starts again from
  its first place. The log says `[vr] free roam: begins at ...`, and `[vr] free roam: zoomed in/out as far as it goes`
  at the zoom's ends.

All of its numbers are in one table, `kFreeRoam` (`vr_sail_view.h` `FreeRoamOffsets`).

## When the boat's views are offered (`vr_sail_view.h` `sailing_eligible`)

All of the following must hold, read at each pad read and each frame:

| condition | read from |
| --- | --- |
| Link rides the boat | player status 0 `SHIP_RIDE` (play +0x5CD8, bit 0x10000) |
| the boat exists | play's ship pointer (`mpPlayerPtr[2]`, play +0x5B3C), only read while he rides; its position finite |
| the boat is sailing | `daShip_c::mCurMode` (+0x635) is 0 waiting at the tiller, 1 steering with the sail, 2 paddling |
| the open world | the stage is `sea` (the Great Sea) |
| normal play | `aspect::gameplay_camera()`: no event, cutscene, talk, title or conducting |
| no menu, no zoom | no GamePad menu (`gfx::display_gamepad_menu_open`), not the telescope or the Picto Box |

Sailing also counts the boat's tools at sea: the cannon (mode 9) and the salvage arm with the grappling hook (10, its
lift 11). Until 2026-11-06 they ended the boat's views the moment the cannon or the hook came out (the player's log:
`[vr] view: third person (the boat's views end: the boat is not sailing ...)`), so they could not be used there.
Not sailing (modes 3 to 8 and 12 to 16): getting on (`READY_FIRST/SECOND`), getting off (`GET_OFF_FIRST/SECOND`),
events, talking to the King of Red Lions, tornadoes, warps, whirlpools, being thrown.

When any condition stops holding while one of the boat's views is shown, the view goes **at once to third person**
(the game's camera), and the click again cycles only first and third person. The boat's view is not remembered: the
next time you sail you click through to it again. The log says why: `[vr] view: third person (the boat's views end:
a menu)`. A click in third person while on the boat but not sailing logs `[vr] view: the boat's views are not offered
(...)`.

## The mode state (`vr_sail_view.h` `ViewCycle`)

One state, `g_view` in `vr_game.cpp`, under the VR lock. It changes in only two places: the right stick's click
handler (`ViewCycle::click`, after the existing `ClickToggle`, so one click advances exactly one mode and a held click
or the recentre gesture does nothing) and `end_boat_view_unless_sailing` (`ViewCycle::update`, each frame in
`CameraOverride` and each pad read). `g_thirdPerson` is now just a copy of `mode != FirstPerson` for the code that
reads it without the lock. So the boat's views behave like third person everywhere else: Link's head is drawn, the
game's HUD is shown and there are no motion controls.

## The boat's frame and geometry

The boat's frame is the one its body model is drawn in. `daShip_c::setMtx` builds that frame as
T(`current.pos`) · Ry · Rx · Rz (`shape_angle`; `vr_sail_view.h` `boat_rotation`). It is read from the actor's
fields, which only the logic changes, once a logic step (`BoatFrames::sample`). Between steps it is blended at the
step fraction (`interp::pass_t`) as the drawn model is: position by lerp, angles the short way round. A new boat
actor (new scene, the boat recreated) starts again without blending from the old one, and getting off clears it.

The geometry comes from the boat's own models, in its frame (`measure_boat`): each joint's world position is
multiplied by the inverse of the body model's base matrix. The joints are the mast (body joint 6, `J_FN_MAST`) and
the figurehead's head (head-model joint 8, `J_FN_ATAMA`); Link's seat is his position. The geometry is kept once it
has held still for 10 steps, so the mast's and the head's animations are over. It is believed only if it looks like
the boat (`plausible_boat`) and is kept per boat actor. Until then the defaults in `BoatGeometry` are used. The log
line is `[vr] boat: measured in its own frame: ...`.

## The neck view (`kNeckView`)

Its anchor is in `kSailAnchors` (`vr_sail_view.h`; boat frame +X its left, +Y up, +Z the bow): 5 units ahead of and 45
above the figurehead's head joint, just over the head, so looking down shows its eyebrows and head; facing the bow
(2026-11-06, from the headset: 60 behind and 55 above was behind the horns, then 60 ahead and 40 above too far
forward, then 15 ahead and 55 above almost right: moved a little back and down). The anchor is where your head is at
the moment you switch to it (`HeadController::begin_boat_view`), with your head tracked in full; it moves with the
boat (its pitch and roll too: you stay at your place on the neck) but only the boat's turn turns the view, never its
pitch or roll, so the horizon stays level. Looking round is as in first person: turn your head freely, or turn the
view with the right stick (`vr.turn`: smooth at `vr.smoothDegreesPerSecond`, or snap by `vr.snapDegrees`) about its
place on the neck (`HeadController::boat_turn`; Link and the first-person mapping are not turned). Each time the
view begins it faces ahead again. The triggers are the game's, as in third person. None of the game's HUD is shown
there, not the action prompt either (`vr_ui.h` `State::boatView`, asked for 2026-11-06), and no label. The log line is
`[vr] boat view: anchored at ...`.

## Tests (`runtime/tools/vr_test.cpp` `test_sailing_views`)

- normal play cycles only first ↔ third; sailing cycles first, third, the neck view and the Free Roam Camera, and
  wraps;
- leaving sailing in a boat view goes to third person, and the next click goes to first person; first and third are
  never changed by it;
- a cutscene, an event or a talk (no play camera), a menu, the telescope, getting on or off, the other ship modes, not
  riding, another stage: no boat views;
- the boat's rotation matches the model's base matrix order; the blend between steps (and across the ±180° wrap); a
  new boat or no boat leaves no stale frame;
- the geometry: only believed when plausible, kept when still, measured again for a new boat;
- the Free Roam Camera: its start (behind Link, above the figurehead, facing the bow); each trigger zooms its way
  along the start's line, partly pulled slower, resting or both at once not at all; the zoom's and the slide's
  limits; the stick slides right and up without zooming; nothing for no time or bad input; the reset; it follows the
  boat's position and turn but not its pitch or roll;
- 6DoF: the view starts at the anchor and follows the head's translation (scaled) and rotation exactly; the boat
  turning turns it about the anchor; the first-person mapping and calibration are unchanged;
- stereo: in the boat's views the eyes keep their separation, centred on the view, along its right axis.

## Known limits (to check in the headset)

- **The neck view's clipping.** Its anchor is placed from the measured head joint, but nothing checks it against the
  horns; adjust `kSailAnchors` (`neckAhead`, `neckHigh`).
- **Clipping.** Nothing checks the Free Roam Camera against the world: near an island it can go into the rocks, and
  slid forward or sideways while zoomed in it can pass through the sail or the figurehead. Its height changes only with
  the zoom (at least about 75 units above the deck at the nearest), so it stays above the sea.
- **The start.** 300 behind and 250 above are estimates of a little higher than the game's sailing camera; adjust
  `kFreeRoam`.
- **Speeds.** 600 units a second zoom and 400 slide are first guesses; adjust `kFreeRoam`.
- **Leaving for any interruption** (a menu, talking to the King of Red Lions, the Wind Waker) drops to third person by
  design, and the Free Roam Camera starts again from its first place when you click back.

## Headset checklist (Capped 90, `WWHD_VR_FRAME_RATE=full`)

1. On land, in a dungeon or in a town: the click goes first ↔ third person only; the triggers lock on and open the
   item wheel as before.
2. Get on the boat: no boat views while getting on; once seated at the tiller, third person → the neck → Free Roam →
   first person. Check the log's `[vr] boat view: anchored at ...` and `[vr] free roam: begins at ...` lines.
3. The neck view: where it stands (just over the head: looking down shows its eyebrows and head; not inside it), the
   sea ahead;
   steady with the boat while sailing and turning, a level horizon; the right stick turns the view (smooth or snap,
   as in first person) and the head looks round freely.
4. Free Roam: no HUD, no label, Link not drawn; where it starts (above and behind the boat, the boat
   in view when you look down), the left trigger
   zooms in and the right out smoothly, the right stick slides it sideways and forward past Link / back without turning; no lock-on, item wheel or crouch
   meanwhile.
5. Sail, turn hard, sail through waves and Swift Sail: the camera follows with no lag against the drawn boat, no 30 Hz
   steps, and a level horizon that does not bob.
6. Head movement in all six degrees of freedom: tracking stable, stereo aligned, comfortable.
7. Zoom and slide, click away to first person, click round to Free Roam again: it starts at its first place.
8. Open the menu, talk to the King of Red Lions, play the Wind Waker or use the telescope: each goes to third person.
   The cannon and the grappling hook in the neck view and Free Roam: the view stays. The bow, the hookshot and the
   boomerang there: first person at once, and their motion controls work at the tiller (Link stays seated); A hops,
   the right stick down never does. Free Roam: Link not seen (check the hookshot's chain and the
   boomerang in flight are).
9. Get off the boat while in the neck view or Free Roam: third person straight away, no jump, freeze or bad view; then first person on
   land as before.
10. Warp (Ballad of Gales), enter a cave or island stage from the boat: no boat views there, nothing stale.

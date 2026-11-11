# VR mode: controls and settings

The full settings reference is [VR_SETTINGS.md](VR_SETTINGS.md); the launcher is [VR_LAUNCHER.md](VR_LAUNCHER.md).

How VR mode works and what it changes: [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).

## Starting in VR

- `wwhd --vr` (or `Wind Waker HD.exe --vr`): try VR for this start. `--no-vr`: the desktop for this start.
- `vr.enabled=1` in the settings (`settings.ini`, see below): try VR at every start.
- `WWHD_VR=1` / `WWHD_VR=0` (environment) overrides both.

The OpenXR runtime must be running with the headset awake (SteamVR, Monado, WiVRn, Windows Mixed Reality,
Meta Quest Link / Virtual Desktop through their OpenXR runtimes). The log (`data/captures` /
`wwhd.log`) has a `[vr]` line saying why VR did not start when it does not, and the game plays on the
desktop then.

The game window keeps showing the left eye (a mirror for others watching).

## Default controller layout

Hands: the **sword hand** is the right controller, the **shield hand** the left; `vr.leftHanded=1` swaps
them (the sword is then in the left hand; sticks keep their roles).

| Action | Controller | GamePad button / effect |
|---|---|---|
| Move | left stick | left stick (radial dead zone `vr.stickDeadZone`); forward is where you look (`vr.moveDirection=hand`: where the left controller points) |
| Walking backwards | left stick back (more than 100 degrees from forward; it ends forward of 80 or let go) | Link keeps standing, his model facing the way you face every step (so he and you never disagree), and VR moves him the stick's way, up to 6 game units a step (`vr.moveSpeed` scales it), as far as walls let him; the game gets no stick meanwhile, so it never turns him round. No leg animation (not seen in first person). ZL and lock-on are the game's as always; locked on, on a ladder or swimming, the game's own movement |
| Turn | right stick left/right | smooth turn (the default) or snap turn (`vr.turn=snap`, `vr.snapDegrees`, default 30) |
| Sword | swing the sword-hand controller | B (the game picks the cut: combos, finishers as usual) |
| Sword (button) | sword hand secondary button (B on Touch, B on Index) | B |
| Interact / talk / open | sword hand primary button (A on Touch, Cross on PS VR2) | A |
| Target (lock-on) | shield hand trigger | ZL |
| Shield / crouch | right stick down (held) | ZR |
| Item on R | sword hand grip | R |
| Item on X / Y | shield hand primary / secondary (X / Y on Touch) | X / Y |
| Item wheel | with nothing in Link's hands, hold the sword hand trigger, right stick to choose, let go to take it out; or reach over the shield hand's shoulder, right stick to choose, hand forward to put it on X (`vr.itemWheelButton`); right stick click cancels | the item on the button, as the game's item menu ([VR_ITEM_WHEEL.md](VR_ITEM_WHEEL.md)) |
| Pause menu | menu button (left controller on Touch, PS VR2 Sense through SteamVR, Vive, WMR) | + (the pause, item, map and bag menus appear on the GamePad panel in front of you) |
| Map / Minus | sword hand stick click | − |
| Recentre | hold **both** stick clicks for 0.6 s | — (not sent to the game) |
| Aim bow, hookshot, boomerang | point the hand that holds it (the right stick still turns, on land and on the boat) | the game's aim, set from the hand (docs/VR_AIMING.md) |
| Wind Waker: notes / beat | left stick or left hand held up/down/left/right; right stick or right hand held left/right | left stick / right stick, as on the GamePad (no moving or turning while conducting; [VR_WIND_WAKER.md](VR_WIND_WAKER.md)) |

**The GamePad screen in the headset.** WWHD draws its pause/collection, item, map/sea chart and bag menus (and the
Wind Waker's song list) on the GamePad screen. In VR that screen appears as a panel in front of you while one of
those menus is open (`vr.gamepadScreen=menus`; `always` keeps it up, `off` never shows it). It is placed where you
look when it appears, `vr.menuDistance` away (1.125 m), and stays fixed in the room until the
menu closes. Navigate it with the sticks and buttons as on the GamePad.

**Changing the menu button.** Plus is pressed by the `menu` action. Bind it to another button with
`vr.bind.menu=<button>` (on the game side), or move the physical button in the runtime's binding editor (on the
controller side). Holding the button does not repeat: the game reacts to the press, as on the GamePad.

Interaction profiles suggested to the runtime: Khronos simple controller, Oculus Touch (Quest, Rift and
most emulations), Valve Index, HTC Vive, Windows Mixed Reality. Runtimes map other controllers to one of
these; their binding editors (e.g. SteamVR > Controllers > Manage controller bindings) can change any
physical binding. The **Valve Index** has no menu button: bind "Menu" in SteamVR's binding editor.

**PS VR2 Sense controllers (SteamVR).** SteamVR's OpenXR runtime has no interaction profile for them (2.18: none
from Sony), so it maps the game's Oculus Touch bindings onto them in compatibility mode: the controllers pose as Touch
controllers, with Touch models and Touch-shaped poses, and a saved copy of that mapping bound the left "menu" to an
input the Sense controller does not have and a *touch* of Create to the menu.
`runtime/assets/steamvr/wwhd_playstation_vr2_sense.json` is a native binding instead (no compatibility mode; the game
still sees the Touch profile):

| Game action | Left | Right |
|---|---|---|
| Trigger | L2 | R2 |
| Grip | L1 | R1 |
| Stick, stick click | left stick | right stick |
| Primary (X / A) | Square | Cross |
| Secondary (Y / B) | Triangle | Circle |
| Menu | Create (press) | Options (press) |
| Poses | the Sense controller's `handgrip` and `openxr_aim` | |
| Vibration | both | |

SteamVR uses it when the game's binding setting points at it: with SteamVR closed, in
`~/.local/share/Steam/config/steamvr.vrsettings`, section `system.generated.openxr.the wind waker hd.wwhd`,
`"playstation_vr2_sense_250820_CurrentURL_openxr" : "file:///<repository>/runtime/assets/steamvr/wwhd_playstation_vr2_sense.json"`.
Back to another binding: SteamVR > Controllers > Manage controller bindings > The Wind Waker HD, or the value the
`..._PreviousURL_openxr` key keeps. Players without the repository can take it from there once it is published to
the Workshop from SteamVR's binding editor. Not yet tried in the headset.

Change which GamePad button an action presses with `vr.bind.<action>=<button>`; actions: `mainTrigger`,
`mainGrip`, `mainPrimary`, `mainSecondary`, `mainStickClick`, `offTrigger`, `offGrip`, `offPrimary`,
`offSecondary`, `offStickClick`, `menu`; buttons: `A B X Y L R ZL ZR Plus Minus Home Up Down Left Right
LeftStickClick RightStickClick`, or empty for nothing. Example: `vr.bind.offTrigger=L`.

## Settings

Saved with the other settings (`settings.ini` in `data/user` for releases, `%APPDATA%\WWHD` or
`~/.config/wwhd` for source builds) as `key=value` lines. Unknown or unreadable values keep the default;
numbers are clamped to the ranges below.

| Key | Default | Range | Meaning |
|---|---|---|---|
| `vr.enabled` | 0 | 0/1 | try VR at start |
| `vr.posture` | standing | standing, seated | (recorded for the overlay; recentring works the same in both) |
| `vr.heightOffset` | 0 | -1 .. 1 m | added to the eye height (seated: raise the view) |
| `vr.unitsPerMetre` | 100 | 40 .. 250 | world scale: game units per real metre (smaller = you feel larger) |
| `vr.leftHanded` | 0 | 0/1 | sword in the left hand |
| `vr.turn` | smooth | snap, smooth | right stick turning |
| `vr.snapDegrees` | 30 | 10 .. 90 | snap turn angle |
| `vr.smoothDegreesPerSecond` | 120 | 20 .. 360 | smooth turn speed at full stick |
| `vr.moveDirection` | head | head, hand | what "forward" on the left stick means |
| `vr.moveSpeed` | 1 | 0.2 .. 1 | left stick scale (the game walks below about half) |
| `vr.renderScale` | 1 | 0.5 .. 2 | headset image size relative to the runtime's recommendation |
| `vr.maxHeadOffset` | 1.0 | 0.05 .. 2 m | how far you may lean/walk from Link's body before the view stops (walls stop it sooner) |
| `vr.gripOffsetX/Y/Z` | 0, 0, -0.04 | ±0.3 m | sword root in the controller's grip space |
| `vr.gripPitchDegrees` | -40 | ±90 | sword tilt forward from the grip |
| `vr.bladeLength` | 0.85 | 0.3 .. 1.5 m | blade length for swing detection (the game's own length is used for hits) |
| `vr.swingStartSpeed` | 2.6 | 0.5 .. 10 m/s | tip speed that starts a swing |
| `vr.swingEndSpeed` | 1.2 | 0.2 .. 8 m/s | tip speed that ends it (kept below the start speed) |
| `vr.swingMinTime` | 0.05 | 0 .. 0.3 s | time above the start speed before it counts |
| `vr.swingCooldown` | 0.25 | 0 .. 1.5 s | after a swing, before the next can start |
| `vr.hapticStrength` | 0.6 | 0 .. 1 | vibration on sword hits (0: off) |
| `vr.cutsceneFollow` | 1 | 0/1 | events: game camera position + your head's rotation (0: flat, head-locked) |
| `vr.showBody` | 0 | 0/1 | draw Link's head too in first person |
| `vr.trackedSword` | 1 | 0/1 | draw the sword at your hand (0: where Link's animation holds it) |
| `vr.debugCollision` | 0 | 0/1 | reserved for the collision visualisation (not implemented yet) |
| `vr.menuZoomOut` | 3 | 1 .. 4 | title and file select appear this much farther; gameplay unaffected |
| `vr.gamepadScreen` | menus | menus, always, off | the GamePad screen as a panel in the headset |
| `vr.menuDistance`, `vr.menuHeight`, `vr.menuScale`, `vr.menuCurvature`, `vr.menuPlacement` | see VR_SETTINGS.md | | the menu panel (VR_PAUSE_MENU.md) |
| `vr.cutsceneZoomOut` | 1 | 1 .. 3 | cutscenes: the game's camera this much farther |
| `vr.refreshRate` | 0 | 0 .. 240 Hz | ask the headset for this rate where the runtime supports choosing it (0: the runtime's) |
| `vr.autoFrameRate` | 1 | 0/1 | switch frame interpolation on (120 fps, paced) for the VR session when it is off; not saved |
| `vr.stickDeadZone` | 0.15 | 0 .. 0.6 | radial dead zone of the controllers' sticks |
| `vr.armIK` | 1 | 0/1 | Link's arms follow the controllers |
| `vr.detachedArms` | 0 | 0/1 | Detached Arms: the hands reach the controllers beyond Link's reach, the wrists turn freely |
| `vr.realArmLength` | 0.62 | 0.3 .. 1 m | your shoulder-to-hand length (scales your reach to Link's) |

Comfort: smooth turning, head-relative movement and a world scale near 100 are the defaults.

Frame rate: while VR runs, the headset's refresh rate (not the monitor's) decides how many frames interpolation
draws: up to 120 fps on 90 Hz and faster headsets (frames that do not fit are skipped, the game logic stays at 30
steps per second), up to 60 on slower ones. The window mirror never waits for the monitor. SteamVR chooses the
headset's rate in its own video settings; runtimes with `XR_FB_display_refresh_rate` take `vr.refreshRate`. The
log reports `[vr] timing:` every 10 seconds. A sharper picture: raise Graphics > Internal resolution (the eye
pictures are drawn at the game's internal resolution and scaled to the headset's).

## Tracking: hands that stopped matching the controllers (2026-10-25)

Reported: the hands sometimes stopped matching the controllers until a recentre.

**How a hand gets its place.** Link's wrist goes where you see your controller: his eyes (the camera) plus the
controller's offset from your real head, through the same mapping as the camera (`g_map`: the tracking space's origin
and turn into the game; `vr_arm.h` `eye_wrist_target`). His arm reaches for it from his own shoulder. A target his arm
cannot reach is pulled back along the line from the eyes (`keep_in_view`), and the hand no longer sits on the
controller.

**Causes found:**
- **Events, cutscenes and third person left the mapping stale.** They move and turn Link: a conversation turns him to
  the speaker, a cutscene puts him elsewhere. You may also walk or turn in the room meanwhile. First person then
  resumed with the old mapping, so his body was away from, or turned from, where the camera and your controllers were,
  and his arms could not reach. A recentre fixed it, which is why it helped. The mapping was set again only at a new
  stage, at the start, or by a recentre.
  - **Now:** when first person resumes after an event, a cutscene or third person, the view is anchored at your head,
    facing Link's way, as TPVR calibrates at every first-person activation (`vrCutsceneFaceCamera`: it snaps to
    Link's facing on the way out of events). The camera cuts there anyway.
  - The height setting and the rest of the calibration are kept; a menu (the GamePad panel) does not re-anchor.
  - Log: `[vr] first person again: the view anchored at your head, facing Link's way`.
- **Positions the runtime only estimates were taken as tracking.** Out of its cameras' view, a runtime keeps reporting
  a controller's position as *valid* but no longer *tracked* (`XR_SPACE_LOCATION_POSITION_TRACKED_BIT` cleared): its
  estimate from the controller's motion sensors, which drifts. Only the valid bits were checked, so the hands slid
  away with the estimate.
  - **Now** (`vr_tracking.h` `track_pose`): once a hand has been tracked this session, an estimated position counts
    as a gap. The last tracked pose is held, with the fresh orientation, for up to 2 s, and after that the estimate is
    used as the best there is.
  - Back from a gap, the hand blends to the tracked pose over 0.1 s instead of jumping. In normal tracking nothing is
    filtered.
  - A runtime that never reports the tracked bit behaves as before.

**Not changed (what TPVR does too):**
- one snapshot of the head and both controllers per input read, at the predicted display time, in one reference
  space;
- the controllers are never smoothed.

**Diagnostics:** every 10 s with the input summary,
`[vr] tracking: head at most X m from the view's anchor; hands beyond Link's reach a / b times (at most ... units);
positions only estimated by the runtime a / b reads; n re-anchors after events`. A hand that drifts with few
estimated reads and many beyond-reach counts is the mapping (lean or facing); many estimated reads is the runtime
losing the controller.

**Still open:** walking a long way in the room without an event in between. The camera leans with you up to
`vr.maxHeadOffset`, while Link's body stays where it is, so his arms stop reaching. A recentre (both stick clicks) puts
it right. The `head at most X m` line shows when that is the case.

## Tracking gaps

Inside-out tracked controllers (PS VR2, Quest, Windows Mixed Reality) lose their position for moments: held behind
the body, close to the headset, or out of the cameras' view. The runtime then reports the pose without the
position-valid flag. Every such read used to count as "not tracked": Link's arm fell back to the game's animation
and snapped back a moment later.

Now each hand keeps its last valid grip and aim pose through a gap of up to 2 seconds (`vr_tracking.h`
`hold_pose`, used by `openxr_session.cpp` `sync_input`):

- If the runtime still reports a valid orientation, the hand turns with it while its position is held.
- If `xrSyncActions` fails for a moment, the hands are held the same way. The buttons read as released.
- The headset's pose that the arm IK uses is held for 2 seconds as well.

Only a longer loss lets the arm fall back to the animation, blending over 0.15 s; it blends back in the same way
when tracking returns. Outside normal play (cutscenes, menus) the arms keep the game's animation as before.

**Estimated positions.** A position the runtime reports valid but not tracked is its estimate from the
controller's own sensors while out of the cameras' view. Once a hand has been tracked, such a read counts as a gap
(`track_pose`), except on a Quest (`Session::quest`): Meta's estimate is what its runtime shows, and holding the last
tracked pose instead froze a lowered hand for up to 2 s and then jumped (a PC Quest 3 log had the left hand held for
half of some 10 s windows). There only a missing position is a gap.

The input summary in the log (every 10 s) counts the reads in which each hand was held through a gap, and how many
times it was lost after the 2 s: `poses held through tracking gaps L / R reads, lost L / R times`.

**Jumps no head makes (every headset).** A PSVR2 on SteamVR (Linux) that lost the room while looking down at the
floor reported the head and both controllers 30 to 40 m away, marked as tracked, for a few seconds until it found its
map again (SteamVR's vrserver log: `[HMD] Lost`, `[VrTracker2] tracking lost`). The view flew off with them (into
the sky) and Link "caught up" 40 m. Now (`Session::locate_views`):
- A head position farther from the last good one than 0.5 m plus 2 m for every second since is held like a gap: the
  last good position, turned with the head. Back near it, tracking is followed again. Still away after 10 s, the new
  position is taken (the runtime moved its origin).
- While a jump is held, that reach keeps growing, but not downwards: there only the reach at the jump counts
  (`vr_tracking.h` `head_out_of_reach`). In a PSVR2 run (2026-10-31) the tracker put the head 1.44 m away, 1.25 m of
  it lower; the reach grown to 1.45 m after 0.47 s took that as "back", and the view sank through the floor ("looks
  like he's falling through the level"). Upwards it grows as before, since that is how the tracker corrects an
  estimate that drifted down.
- The view never goes under the ground Link stands on: at least 15 units (about 15 cm) above his feet, or half his
  eye height when that is lower (crawling), whatever the tracking says (`vr_camera.h` `hold_above_floor`). It catches
  what the jump rule cannot, such as an estimate sinking slower than a head moves. The 10 s summary logs
  `view held above the ground Link stands on N times`.
- Back from a held head (a gap or a jump), the view eases from where it was held to the tracked position over 0.2 s
  instead of snapping (`head_ease`).
- To test all three in the headset: `WWHD_VR_TEST_HEAD_JUMP=jump` fakes this run's glitch (0.72 m across, 1.25 m
  lower at once) for 3 s every 20 s; `WWHD_VR_TEST_HEAD_JUMP=sink` lowers the head 1.25 m over 3 s, then snaps it
  back. The view should stay at Link's eyes (jump) or stop at the floor (sink), and come back smoothly. The log marks
  each with `[vr] test: head tracking ...`.
- A controller more than 1.5 m from the last good head position is a gap too (held up to 2 s, as above).
- A head without a valid position is held and turned in the same way on every headset (was a Quest only; elsewhere the
  view fell back to the game's camera for those frames).
- A recentre the runtime announces (`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`) clears the last good
  position on every headset, so a new origin is not taken for a jump.

In the headset (PSVR2, 2026-10-29) it held the view through four 41.7 m jumps of 3.6 to 4.2 s. Their cause was the
PSVR2's saved room map (`~/.local/share/Steam/config/playstation_vr2/sceMapDb.bin`): parts of it about 41.6 m apart,
the tracker latching onto the wrong one after losing the room (also without the game running); a new play area set up
ended them. What remains is the tracker losing the room every 10 to 30 s for 3 to 4 s (mostly looking at the floor):
its estimate drifts meanwhile and snaps back 0.5 to 1 m when the room is found again. SteamVR reported every one of
those reads as tracked (POSITION_TRACKED set), so the game cannot tell the drift from real movement; only the snap is
held (briefly). That is the room's lighting and texture, or the driver, not the game.

## Grips close the hands; right stick down crouches

**Grips.** (Since 2026-11-03 the hands are closed by default in first person, docs/VR_HELD_ITEMS.md "One piece": the grips no
longer need pressing.) Each controller's grip closes the hand of Link's that controller drives: left grip, left hand; right
grip, right hand. Both work at once and independently, and left-handed mode changes only which hand holds the
sword.

- **How it is read.** OpenXR's `squeeze` value is latched closed above 0.6 and opened again below 0.45, so it does
  not flicker around one threshold (`vr_bindings.h` `GripLatch`).
- **How it is drawn.** Link's hands have no finger bones; the game shows whole-hand shapes. A closed grip sets the game's hand
  index for that hand to its closed mesh (left 3, right 8: the game's own closed hands from Link's hands model), for his
  draw only. The game's values are
  put back before his next logic step, as with the sword's shapes (docs/VR_HELD_ITEMS.md), so his logic never sees
  them.
- **Holding something.** A hand holding something keeps the game's shape for it (the sword's grip, the shield's).
- **What it does not change.** The hand's tracked position and orientation, the arm IK and the held items.
- **Limits.** The shapes switch at once; there are no in-between finger poses to blend through. A controller lost
  for more than 2 s or a failed input sync opens the hands.

The grips' GamePad buttons are separate settings: the sword hand's grip still presses R. Settings saved before
format 2 had the shield hand's grip on ZR (crouch); that saved default is cleared once (`migrate_bindings`), since
the right stick crouches now. A binding chosen in the launcher is kept.

**Right stick down: crouch** (hold, the game's own crouch: ZR, as "ZR Crouch" on the HUD).

- **Which stick.** The physical right controller's stick, whatever the handedness: it is the turning stick.
- **Threshold.** It presses below -0.6, with OpenXR's y +1 up and -1 down, while the stick points more down than
  sideways, and releases above -0.4 (`StickDown`). Drift, up, left and right never crouch, and a sideways push for a
  turn does not crouch.
- **Rebinding.** It is a binding of its own (`vr.bind.rightStickDown`, default ZR) and can be changed in the
  launcher.
- **Where it is off.** While the right stick aims an item (bow, hookshot: it aims there), through the telescope,
  outside normal play (events, cutscenes) and while a GamePad menu is open.
- **The body and the view.** The game's crouch moves Link's body and his collision as in the game. The view keeps
  following your head at Link's eye height, which the camera follows when it changes for good: a crouch lowers it.
  Releasing the stick stands him up, as releasing ZR does. Snap turning on the same stick's X is unchanged.

## Third-person view (right stick click)

A short click of the **physical right stick** switches between first person and the game's own third-person
camera, in normal play. It switches once per click, when the click is released (`vr_bindings.h` `ClickToggle`).
Holding it does nothing, and a click while the left stick is also clicked does not count, because both clicks held
are the recentre gesture. It is off while looking through the telescope and while a GamePad menu is open; events,
cutscenes and menus keep their own cameras in either mode. The click itself no longer reaches the game. It was the
sword hand's stick click on Minus for right-handed play; a binding chosen in the launcher can put Minus on another
action.

While Link sails the King of Red Lions on the Great Sea, the click goes first person → third person → **the King of
Red Lions' neck** → **Free Roam Camera** → first person. The neck view stands just over the figurehead's head (look down
to see its eyebrows and head), facing ahead, with no HUD; there you look round as in first person, with your head or by turning with the right stick (smooth or
snap, `vr.turn`). The Free Roam Camera starts above and behind the boat, with no HUD, no label and Link not drawn; the left trigger zooms it in towards
the boat, the right trigger zooms it out, and the right stick slides it left and right, and forward past Link (up) or back (down). There, and only
there, the triggers and the right stick do nothing else (no lock-on, no item wheel, no crouch); the left stick steers
the boat as always. It starts again from its first place every time you enter it; Link is not drawn there. Leaving
sailing goes back to third person at once (docs/VR_SAILING_VIEWS.md); the cannon and the grappling hook do not end it.

**On the boat** (only while Link rides the King of Red Lions, in every view; nowhere else; `vr_sail_view.h`
`boat_controls`, asked for 2026-11-06):
- **A hops, only while sailing** (at the tiller, steering, paddling; not with the cannon or the salvage arm out, nor
  getting on or off). The right controller's A also presses ZR then, the boat's hop (the game's R-lock trigger: the
  sail up, the boat faster than 30 with wind of 20 or more). A's own press stays the game's (get off, talk, hold to stop). The right
  stick down does not press ZR on the boat (no hop, no crouch): it is for the camera there, in every view.
- **Left grip + left stick = the D-pad.** Holding the left controller's grip, the left stick's strongest direction
  past half a push presses D-pad up, down, left or right (up plays the Wind Waker); the boat is not steered meanwhile.
  The grip alone presses nothing on the boat (third person off the boat: the same, below).
- **The item wheel in the Red Lion's view.** The right trigger with nothing in Link's hands (or the reach over the
  shoulder) opens the item wheel there too, as in first person (`vr_sail_view.h` `wheel_view`); choosing the bow, the
  boomerang or the hookshot then takes the view to first person (below). Not in Free Roam, whose triggers zoom it.
- **Weapons in the boat's views go to first person.** Taking out the bow, the hookshot or the boomerang in the neck
  view or the Free Roam Camera switches the view to first person at once (`vr_sail_view.h`
  `boat_view_to_first_person`, asked for 2026-11-06), where the first-person motion controls work on the boat as on
  land: VR archery for the bow, the left hand's pointing and the triggers for the hookshot and the boomerang, the
  boomerang's downward swing. Click back to a boat view afterwards. Aiming on the boat never slides Link by the stick
  (the land's moving while aiming: he sits, and the stick is the boat's). The cannon and the salvage arm keep the
  boat's views. The bow's fixes apply to the boat's bow aim (0x8D) as to the land's (0x94): the left hand stays drawn,
  the arrow follows it, the bow keeps its place in the hand (`vr_sail_view.h` `bow_aim_proc`). The arms stay on the
  controllers at the tiller however you look round (`ship_ride_proc`; they had taken the game's animation when the head
  looked more than 100 degrees from the bow). `[vr] item wheel on the boat: ...` logs why a trigger press there did not
  open the wheel. The boomerang: its laser is on the left hand while Link holds it, as the hand's own 15 m line (never
  drawn out to the game's far sight), and off from its release until the catch (`vr_sword.h` `aimed_item`, the boat
  keeps Link in its boomerang action while it flies, with nothing equipped); caught, the game puts it away
  on the boat, so VR takes it out again (`[vr] boomerang: caught on the boat, taken out again`) and it can be thrown
  again as on land (`vr_sail_view.h` `BoatBoomerangCatch`). Put away before a throw (A or B), it stays away.
  While it is out the right stick turns the view, on land and on the boat, as with the hookshot (the hand aims it;
  Link is in its aiming action the whole time it is out). X, Y and the right grip are the item buttons everywhere, so the cannon, the grappling hook (salvage),
  the boomerang and the bow come out from whichever one they sit on. `[vr] boat: ...` logs the hop and the D-pad.

- **First person** (as before): the view at Link's eyes, his head hidden (docs/VR_FIRST_PERSON_CAMERA.md).
- **Third person**: the game's own camera, with its follow, framing, collision, and the swimming and climbing
  cameras. The headset turns the view relative to where you looked when you switched, as in cutscenes; its
  position does not move the camera. Link is drawn whole: his head is not hidden and the close-camera flag is not
  set, so the game's next skeleton update shows his head again. The left stick moves him relative to the game's
  camera, as on the GamePad, and the right stick's X turns that camera instead of snap turning; the right stick
  down still crouches.
- **No motion controls in third person** (as asked in the headset): the arms play the game's animation (fading out
  over 0.15 s, the log says `arm stops: third-person view (motion controls off)`), the sword and other held items
  stay where the game holds them, real swings no longer attack or move the hit capsules (the game's own sword
  collision; attack with the bound button, B by default), and the grips do not close the hands. Buttons and sticks
  work as usual. Everything comes back with first person; the swing detector starts afresh at each switch, so the
  hand's movement meanwhile is not taken for a swing.
- **The triggers fire the bow** (third person, on land and on the boat; asked for 2026-11-06): with the bow out,
  holding either trigger draws it (its item button held) and letting go shoots, as the game's own button
  (`vr_sword.h` `triggers_fire_bow`). Lock-on (ZL) and the item wheel's trigger are not given meanwhile. First person
  keeps VR archery.
- **Left grip + left stick = the D-pad** (third person only, as on the boat; asked for 2026-11-06): in normal play,
  holding the left controller's grip makes the left stick's strongest direction past half a push the GamePad's D-pad
  up, down, left or right (`vr_sail_view.h` `left_grip_dpad`, `boat_dpad`), and Link does not move meanwhile; let go
  and the stick moves him again. The grip alone presses nothing there now (until then it was D-pad up: now grip and
  stick up). Outside normal play (an event, a menu, conducting) the grip alone is still D-pad up. Its button binding
  (`vr.bind.offGrip`, or `vr.bind.mainGrip` left-handed) does not reach the game in third person, as before. (It had put
  the Wind Waker on the item wheel's button and taken it out.)
- **Back to first person**: the first-person camera at his eyes as before, without recentring. His head is hidden
  again from the next draw.

The mode is one state (`g_thirdPerson` in `vr_game.cpp`), used by the camera override, the movement angle, the right
stick and Link's draw. It is not saved; each start is in first person. `WWHD_VR_TEST_THIRD_PERSON=1` starts in third
person (test captures).

Tested: `vr_test` covers the toggle (one per release, none while held, none with the recentre gesture). In a VR
capture in third person, Link is shown whole from behind, head and cap included, by the game's camera, and the
camera followed him as the scripted stick walked him off the pier into the sea. With the sword drawn in third person, both arms switched off and Link ran with the game's own animation. Reported in the headset: the toggle works both ways, third person without motion controls and first person with them again. The click itself and switching both
ways still need the headset.

## Baba Buds (2026-11-03)

Asked for in the headset: in first person, dropping into a Baba Bud and being shot out of it is shown in third person,
and first person comes back on landing. The view switches by itself (`vr_bud_view.h`, applied in `vr_game.cpp` at each
pad read):
- **Third person** when Link steps into a bud (his action `VOMIT_READY` 0x96, then `VOMIT_WAIT` 0x97 inside and
  `VOMIT_JUMP` 0x98 shot out; decomp `daPyProc_*_e`), from first person in normal play only.
- It stays third person while he is in the bud, shot out, or falling from it (`FALL` 0x27), and while an event or talk
  holds the game's camera meanwhile.
- **First person again** when he opens the Deku Leaf to glide (`FAN_GLIDE` 0x93: its glide pose, the leaf in both hands
  above his head) or lands (`VOMIT_LAND` 0x99, or any other action). The view is anchored at the head facing his way, as
  after an event ("first person again").
- Already in third person: nothing changes. A right stick click meanwhile hands the switch back to the player.

The log says `[vr] Baba Bud: third person (action ...)` and `first person again (action ...: gliding / landed)`.
`vr_test` checks the switches. Not yet tried in the headset.

## Leaning into walls (VR fork)

Once a game step, before Link's logic, the game's own line check (`cBgS::LineCross`, as its actors check sight lines)
runs from Link's eyes towards where your head's lean puts the view (`vr_game.cpp` `free_distance`, `wall_lean_update`):

- **Room scale:** Link is moved only as far as is free from his eyes, and the tracking origin takes the same move. It
  took the whole move before, so against a wall the view stayed at his body while your head went on, and the world
  slid with your head.
- **The view:** its lean from the origin is held 6 units (6 cm: the near plane and the eyes' spacing) short of a wall in that direction until the next
  step, in every first-person state, so leaning into a wall does not put the eyes through it. Past that point the
  picture holds while your head goes on (the compositor still turns it with your head).
- **Room scale really moves Link now:** his execute starts with `current.pos = l_debug_keep_pos` (kept from the end
  of the one before), so a move written to his position alone was undone; the walk and the turn about his eyes
  write both (`set_link_xz`), as his facing already did. The origin moves by what he took after his logic
  (`room_taken`), so where his body stops at an object the rest of the lean stays in the view, up to 6 cm from it.
- **Leaning back** stops 8 cm behind his eyes along his facing (still inside his head), so the view never shows his
  body from behind. The tracking summary logs the standing walks and what he took of them every 10 s.
- Vertical movement (crouching, standing) is not checked. Builds without the game functions check nothing, as
  before. `vr.maxHeadOffset` (1 m by default, was 0.6) still limits the lean where nothing is in the way.

## Parry attacks (2026-11-04)

Reported: a parry attack could not be done in first person. The game parries (`daPy_lk_c::changeSpecialBattle`) when A
is pressed with Link locked on to an enemy whose parry window is open, the action prompt the parry's (do status 0x1A,
play +0x5BB7 in HD; +0x4917, the GameCube's place, was read until 2026-11-05 and the window was never seen); a swing pressed B, never A, and the prompt is part of the HUD that VR does not show. Then, by the
enemy, he jumps over it (action 0x5D), rolls round it (0x60) or leaps (0x62), and each move starts its finishing cut
itself (`procBtJumpCut_init`, `procBtRollCut_init`, `procBtVerticalJumpCut_init`).

In first person now (`vr_game.cpp` `g_parry`, all on the game thread; nothing on the render thread):
- **The window**, read once a step in `link_executed`: first-person play, locked on (`daPy_lk_c` +0x65B0) and the
  parry's prompt. Lock on as in the game (the shield hand's trigger).
- **A swing in it parries**: the swing's press is A instead of B (`g_bParry`), from the swing's start in any direction
  (2026-11-05), or a swing up to `kParryBuffer` steps before the window opens. The A button still works too.
- **Easier to start** (2026-11-05): the window opens 12 frames early, closes 12 late and reaches twice the enemy's
  distance (8, 8 and 1.5 before); a flick of the sword hand in it (0.4 of the start speed) parries, no swing needed;
  a swing up to 24 steps (0.8 s) before it opens parries as it opens.
- **Link stays at the enemy** (2026-11-05): through the parry's cut and the landing after it, his own move away from
  the enemy is dropped each step once he is 60 units from it (`set_link_xz`); the enemy's own steps back move only it.
- **The lock taken again** (2026-11-05): when the game lets go of its target with ZL still held, ZL is let go for one
  pad read 2 steps on and every 15 after, 3 times, so the game takes the nearest target again.
- **The lock-on holds through it** (2026-11-05): the view keeps its lock while the game keeps its target, through the
  parry's moves and cuts, and a shield-held lock lets go only after the target has been gone 8 steps, never in a
  parry's move. `[vr] lock-on: the game's target lost` logs any loss with Link's action.
- **The cue**, the game's prompt not shown: a pulse on the sword hand as the window opens, a light one every third of a
  second while it stays open.
- **The view in the move** keeps his standing eye height and the eye point in front of him (the jump itself is
  followed with his feet), not his head turning over in the flip or roll.
- **The finishing cut is your swing** (hooks on the three cuts' starts, `vr::game::parry_cut_wait`): in the move it
  waits until you swing; the game asks again each step, so the cut starts with your swing. With no swing in 1.5 s
  (`kParryCutWait`) the game's own cut; a jump that lands first ends with no cut. A swing in the move presses nothing
  else.
- Third person and the GamePad keep the game's parry with A.
- **The view faces the enemy through the move** (first player test, 2026-11-04: "the camera keeps facing away from the
  enemy", so the finishing swing could not be aimed). The lock-on's view (`LockOnView`) let go at the parry's start:
  its actions were the lock-on's own (8, 9, ...), not the parry's 0x5D..0x64. It now holds through them
  (`lock_on_keeps`), and in them the view's base turns to the locked-on enemy's position rather than Link's facing
  (he jumps over it facing his take-off's way and lands behind it), at 10/s instead of the lock-on's 4/s
  (`RigTurn::response`) to keep up with the move. The head still looks round freely.
- **The finishing cut is the game's** (second player test, 2026-11-04: "it deflects every time"). In the move the
  physical sword armed the attack's capsules along the tracked blade as an ordinary cut, which the parried enemy
  blocks, and the game's own finishing cut had its capsules moved to the tracked blade too. Now in a parry's move the
  physical sword does not cut and the game's cut keeps its own capsules (it is made to strike the enemy's back);
  the swing only starts it, from the swing's start (`e.started`) rather than its end, so it fits the jump's short
  window from its height to the landing.
- **Easier to land** (asked for, 2026-11-04). The enemy's parry window (`fopEn_enemy_c` `mBtStartFrame` ..
  `mBtEndFrame`, its attack's frames, within `mBtMaxDis`) is widened in first person for Link's two checks only (hooks
  on `setSpecialBattle` 023EC370, the prompt, and `changeSpecialBattle` 023E5400, the start; the enemy's own values
  put back after each call): it opens 8 frames earlier and closes 8 later (`kParryEarly`, `kParryLate`), and reaches
  1.5 times as far (`kParryReach`). A full swing up to 0.3 s before it opens parries as it opens (`kParryBuffer`).

Logged: `[vr] parry: the window is open`, `a swing in the parry's window`, the move, `the finishing cut waits for your
swing` and how it was finished. Not yet tried in the headset.

## The vertical slash (2026-11-04)

Asked for: locked on to an enemy, a downward swing with the sword should be the game's vertical slash. The game picks the
cut by the left stick (`daPy_lk_c::changeCutProc`): locked on, B with the stick in the middle is `procCutA`, the vertical
slash; with the stick held it is a side or forward cut. A swing presses B with the stick as it is, so walking while
swinging gave the other cuts. Now a full downward swing with a target locked on holds the stick in the middle for its
press's two steps (`vr_game.cpp` `vertical_press`, after `thrust_press`), on the game thread. Side swings and thrusts
are as before; a swing in a parry's window parries. Not yet tried in the headset.

## Locked-on sword moves (2026-11-04, Orca's training)

A player's full run of Orca's training in first person: the horizontal slice worked; the vertical slice did not (passed
in third person); the first thrust counted and every later one got "You thrust by targeting...", one deflected; parries
were mostly "No! You can only perform a parry by ... pressing A when you see the A!"; the jump attack was deflected.
Causes and changes (`vr_game.cpp`, game thread only):
- **No blade cuts of its own while locked on**: the physical sword armed the attack along the tracked blade as an
  ordinary cut, which Orca and guarding enemies refuse and deflect. Locked on, only the game's own moves cut.
- **Each swing is the game's move**, as the GamePad's stick with B names it (`vertical_press`, `thrust_press`): a side
  swing holds the stick to its side (the side cut), a downward one the middle (the vertical slash, `procCutA`), a thrust
  forward (`procCutF`).
- **Repeated thrusts and vertical slashes**: each starts the combo again (`daPy_lk_c` +0x68E0, its count, set to 0
  before the press). The 4th cut of a combo is a finisher (`procCutEA`/`EB`), which Orca refused as a thrust; on the
  GamePad a pause between presses ends a combo, which quick real thrusts never had.
- **Parry**: a swing within 12 of the locked-on enemy's attack frames before its (widened) parry window presses nothing
  and parries as the window opens (the buffer is now 0.6 s), instead of an attack Orca counts as wrong.
- **The jump attack**: a downward swing begun with the sword hand above the head (3 cm under the eyes or higher), locked
  on while the game offers the jump attack (do status 0x51, the A prompt), presses A (`procJumpCut`).
- Logged: `the jump attack (A)`, `held for the parry`. Not yet tried in the headset.

**Easier to register, locked on** (training test 2, 2026-11-04: the vertical slash rarely came, thrusts sometimes
missed). While locked on the swing detector (`SwingDetector::set_locked_on`, `vr_swing.h`) uses looser rules, as the game
then takes only the side cut, the vertical slash and the thrust:
- A top-to-bottom chop counts as the vertical slash within about 60 degrees of straight down (down more than 0.6 times
  across) after 0.10 m, and is judged before the side cut, so a diagonal chop is no longer a side cut.
- A thrust counts after 0.08 m forward, with the blade within about 65 degrees of the path, locked on or not (asked
  for: "thrust should happen lock on or lock off"; not locked on the game's forward thrust is B with the stick forward
  too, `changeCutProc` procCutF). A path more forward than down and than across is judged as a thrust first and waits
  as long as it stays so; a chop must go more down than forward. A thrust starts the combo again either way, so the
  4th is a thrust, not the finisher. (Fix, same day: the first version let a thrust that dipped on its way out become
  the vertical slash at 0.10 m down before it was 0.08 m forward, and thrusts got harder.)
- The jump attack needs the sword hand 0.15 m above the head at the swing's start (`kJumpAttackAbove`; it was head
  height, so an ordinary overhead chop became the jump attack instead of the vertical slash).
Not locked on, the settings' distances and rules are unchanged.
A side swing locked on presses plain B with the stick as you hold it, the game's normal combo slash (asked for after
a play test; it used to hold the stick to the swing's side for the side cut).
The vertical slash itself needs the lock-on, as in the game: not locked on, a downward swing is the plain slash
(B as it is), never the vertical slash or the jump attack (`g_bVertical` set only locked on).

## Spin attacks (2026-11-04)

During the spin attack and the Hurricane Spin (`daPyProc_CUT_TURN_e` 0x55, `daPyProc_CUT_ROLL_e` 0x56 and its end 0x57)
the view turns round with Link in first person, as in the Majora's Mask VR example the user pointed to: the view's base
follows his model's facing (`spin_attack_proc`, `kSpinTurnResponse` 30/s, turned each frame like the ladder and lock-on
turns), the body keeping its yaw from the spin's start so the head still looks round freely. The charge (0x58) and
walking charged (0x59) do not turn him. Smooth turning waits during the spin. When it ends Link faces where he began
and the view is free again (`[vr] spin attack: ...` in the log).
The turn is taken from his model as drawn, not his facing: the spin attack turns him in its animation and leaves
`shape_angle` as it was, and the Hurricane Spin adds its turn to the facing only at its end (`procCutTurn`,
`procCutRoll`), so following the facing did not turn the view at all (reported in the headset, 2026-11-04). The view
follows the line between his shoulders (the clavicles' origins, joints 5 and 9, which the arm tracking never moves):
its turn since the spin began, added to the facing he began with. The log's end line says what it followed and how
far it turned.
Since the play tests (2026-11-04) the turn is the game's own: the spin attack makes one whole turn over its
animation's cutting frames, 3 to 18.5 of `mFrameCtrlUnder[0]` (Link + 0x5898 + 4), by its frame, so at the sword's
own speed, the way the game's sword tip swept round him (taken in `link_executed` before the tracked blade is written
over it); the Hurricane Spin follows his facing plus `m34EC` (Link + 0x6934), the turn the game adds to his model each
step. The view turns at 60/s, about 10 degrees behind; the hands follow the controllers, which turn with the view.
(The shoulders could not be read while his body is hidden, and his facing does not move in the spin attack.)
On by default; `vr.spinCamera` ("Turn the view in spin attacks": F1 > VR, and the launcher's Comfort section) off
keeps the view still while he spins.

## Shield down, shield up (2026-11-04)

In first person with the shield on Link's arm, the shield hand works the lock-on:
- Hand down by the hip (0.55 m under the head): the shield is lowered, and a lock-on this feature started is let go.
- Hand brought up again in front of the body (above 0.40 m under the head, within 0.30 m of the middle): the shield
  is raised and Link locks onto the nearest enemy. ZL is held for him while the hand stays up, the game choosing the
  target. With nothing to lock onto the game takes Z for its target-less walk (`daPyProc_ATN_MOVE_e` 7) and the hold
  is let go at once. The hold ends when the lock does (the enemy gone) or when no lock came within 10 steps. (The
  first version asked the attention list first, `mLockonCount` at play + 0x4564 + 0xD4: it read 0 at every raise of
  a play test beside a lockable enemy, and the raise did nothing.)
Once locked on, the lock holds until the hand drops 3 cm under the height where it locked (asked for, 2026-11-04; 5 cm at first, 3 cm
kept). Let go while the stick is pushed back (the distance release, moving away), the stick is held in the
middle until the next step, so the game does not turn Link round to walk away and the walk backwards takes him from
there, still facing the way he faced (he had turned round and back, 154 degrees, a hitch at every distance release).
The other half of that hitch: Link's frame (the eye point's) was his movement direction while locked on, so backing
away it turned 175 degrees and the eye point was held and glided; locked on (actions 8, 9, side steps, guard, back
jump) and standing (4) it is now the way his model faces (`frame_faces_model`, vr_ik.h).
And for a second after a lock-on ends, standing, he turns to the head smoothly (`kAfterLockTurnSeconds`, about half a
second for half a turn) instead of at once: the head had looked away from the target while he faced it, and at the
release he turned 155 degrees in one step. The arms stay on the controllers through that second.
It also lets go when the target is more than 500 units away (about 5 m, `kShieldLockMaxDistance`; 700 at first), and at once when
the game locks onto someone more than 70 degrees from where you look (`kShieldLockFrontDegrees`, judged as the lock
comes): with several people about it chose one at the side or behind and turned Link round to them. While locked on,
the arms stay on the controllers (not "facing away"; moving back locked on, they took the animation and the sword and
shield left his hands).
The arms also stay on the controllers for half a second after a lock-on ends and while the stick is pushed back:
moving back as the lock let go, the game turned Link round for a step or two before the walk backwards turned him
back, and the arms took the animation between. Raised again only 6 cm above where it was lowered (the fixed 0.47 m
sat 1 cm from the lowering 3 cm under the lock and the shield flickered down and up).
The log has `[vr] shield: lowered / raised ...` lines. The shield hand's trigger still locks on as before.
Lowering and raising are judged by the motion as well as the heights (a play test: only 5 of many lowers and raises
reached the fixed heights): down when the hand drops 0.12 m below its highest of the last 0.8 s and is below the chest
(0.25 m under the head), up when it rises 0.12 m over its lowest and is back above 0.60 m under the head, within 0.50 m
of the middle (eased, same day). `[vr] shield hand: ...` lines record the hand whenever it moves 5 cm, for tuning.

## Swing starts (2026-11-04, play test)

A thrust, the hand pushed forward along the blade, starts a swing at 0.6 of the start speed (`kThrustStartShare`): in a
play test three or four thrusts and the vertical slashes tried never started a swing at all (nothing logged). When
the sword hand moves within half of the start speed but no swing starts, the log says so once a second with the
hand's fastest speed and direction (`[vr] swing: none started; ...`), for tuning from real motions.

The cause, found with that log in the next play test: the swing detector was timed by the clock while the pad is
read up to every 4 ms, more than once a frame, and the controllers' poses change once a frame (predicted for its
display time). Every second read saw no movement, 0 m/s, which undid the start's 0.05 s count, and the next one a
frame's movement over 4 ms (7 to 11 m/s logged for ordinary thrusts). The detectors (swings, the off hand's, the
leaf's flaps) now measure between the poses' display times and skip a read with the same pose time.

## Spin speed (2026-11-04)

`vr.spinSpeed` ("Spin speed", 0.5x to 1.0x, default 1.0x: the game's own; F1 > VR under the spin checkbox, and the
launcher's Comfort section) slows the spin attack and the Hurricane Spin together with the view's turn:
- at the start of the spin attack (0x55), the Hurricane Spin (0x56) and its end (0x57), each of which sets its
  animation then, the rate of Link's lower and upper body animation (`mFrameCtrlUnder[0]`, `mFrameCtrlUpper[0]`,
  Link + 0x5898 and + 0x58B8, `mRate` first) is scaled once;
- in the Hurricane Spin, the turn the game adds each step (`m34EC`, Link + 0x6934) is scaled by the same.
The view follows the spin attack's animation frame and the Hurricane Spin's `m34EC`, so his model, his sword and the
view stay in step at any speed. The spin attack takes longer (its whole turn and hit window stretch). The Hurricane
Spin lasts the same number of steps (the game counts them), so at 0.7x it makes about 70% of its turns.

## Facing after a door (2026-11-04)

Reported with a video: entering a room through a door, first person came back facing the door just walked through, not
into the room. The door's entry event showed Link inside facing into the room; when first person resumed the view was
anchored again (`recenter`, after every event) facing `g_link.heading`, his frame's heading as of his last step, which in
some actions is his travel direction; the body then follows the head while he stands, so he was turned to the door too
(the game's "Open" prompt came up). Now:
- `recenter` faces the way his model faces (`shape_angle`, read from him at that moment). The recentre button does too.
- A new stage (a door, a cave, a load) anchors the view again on its first first-person frame, whether or not an event
  came between (`[vr] new stage ...: the view anchored at your head, facing Link's way`).
- (2026-11-05, still facing the door in the headset) The body no longer follows the head before the view is anchored
  again: while the game's camera is still on after an event (the door's entry; the recentre comes on the next camera
  frame) and in a new stage until its recentre (10 s at most). Followed then, the head's yaw went through the old room's
  mapping and turned Link 180 degrees to the door ("Link turned 180 degrees at once" in the log before "recentred"),
  and the recentre anchored the view to that. Once he is first standing or walking in your hands the view is anchored
  once more (`[vr] in your hands ...`).
Game thread only.

## Fast swings: the hand's return ignored, early swings queued (2026-11-05)

A player's log (SteamVR, build `a56f39d`): 9 of 21 locked-on downward swings did nothing. Most came 1 to 3 logic steps
after another attack began, the hand only 0.09-0.12 m down: the hand coming back from a thrust or side cut, detected as
a downward swing while Link was still in his attack; the rest were real swings made faster than his attacks (a quarter
of the gaps between full swings were 8 steps or less, an attack about half a second). `vr_game.cpp`:
- **The hand's return:** a full swing within `kPhantomSteps` (5 steps, 0.17 s) of the last full swing presses nothing
  (not in a parry's window). `[vr] swing: a full ... ignored: the hand coming back`.
- **Early swings queued:** a sword press that starts no sword move is pressed again, the same move, as soon as Link's
  attack is over (no cut, sword swing, spin or parry move), for `kQueueSteps` (20 steps, 0.67 s) at most; then dropped.
  `[vr] swing: a queued ... pressed` / `dropped`. Not for a parry, the jump attack or the hammer.
Not yet tried in the headset.

## The parry's roll closes the gap (2026-11-05)

A player's log: every parry's roll ran, but its cut ended 98-113 units from the enemy ("When you press A Link does the
roll but it's like I'm too far away to reach him"). The hold that kept him from moving away covered only the cut, and
only stopped him going farther. Now, from the parry's own move on (the jump over 0x5D, the roll round 0x60, the leap
0x62, its cut and landing: `parry_move_proc`), his move away from the enemy is dropped and, farther than
`kParryStayReach` (60 units), he is drawn in towards it, `kParryPull` (10 units) a step at most, straight at the enemy
he is locked onto. The log's `[vr] parry: the cut over` line adds how far he was drawn in. Not yet tried in the headset.

## The roll's cut turns about the enemy (2026-11-09)

A player's video and log against Bokoblins: "Link jumps away from the enemy, not towards the enemy". Every parry was the
roll (0x60), and every cut's move away was held back by 680 to 1000 units. The game's roll cut (`procBtRollCut`, 0x61)
leaps sideways at 20 units a step round the point the roll hands it (`m370C`, +0x7314), facing that point. That point
is the middle of the roll's circle, not the enemy, so the cut carried Link round and away from a spot beside the enemy,
facing past it. Each step of the cut, that point is now the enemy's position (`kRollCutCentre`), so the cut circles
and faces the enemy; the hold and the pull above still apply. Logged once a cut: `[vr] parry: the roll's cut turns
about the enemy (its own centre was N units from it)`. Not yet tried in the headset.

## Sidle (2026-11-09)

A player: sidling was too hard to start in first person; with "Sidle" up on A, A should start it. The game offers
SIDLE (do status 0x10) only on a step that Link's collision was pushed by the wall he faces (`setFrontWallType`'s 2,
`ChkWallHit`), and reads A on that same step (`checkNextAction`). With a GamePad the stick is held into the wall while
A is pressed; in first person you walk up to the wall, let go of the stick and press A, and by then the push and the
offer were gone. Now (`vr_game.cpp`, after Link's execute) A pressed within 1 s of the offer (`kSidleGrace`, 30 steps),
with nothing else on A (do status blank), Link standing or walking (actions 4 to 6) within 25 units of where it was
offered, in first-person play, not locked on and carrying nothing, starts the sidle as the game does:
`procWHideReady_init(0, &m3724)` (023ED788), with the wall's point and angle kept from the offer. Logged: `[vr] sidle:
A N steps after its prompt ...: started`. Not yet tried in the headset.

## Parry on its own (2026-11-09)

Asked for: "when player is prompted by A the game should auto roll for the player and track the enemy in 1st person
mode and stay locked on the enemy while doing so, and then the game should just complete the attack on its own". In
first-person play, locked on, while the parry's window is open (do status 0x1A) and Link is not yet in a parry's move,
A is pressed for him (2 steps down, 1 up, again until the move begins: the game takes A only with the enemy's attack
frame past its window's start). The roll, jump or leap is the game's, the view and the lock-on hold on the enemy through
it (`LockOnView`, as before), the roll's cut turns about the enemy and he is kept within reach (above), and the
finishing cut is the game's own at once (`parry_cut_wait`, `kParryAutoCut`: it no longer waits for a swing). The
sword hand's pulses still mark the window. Logged: `[vr] parry: the window is open: A pressed for you`. Not yet tried
in the headset.

**Your A first (2026-11-10).** In the headset the parry happened without any press ("the player needs to press A first
for it to trigger"). Now nothing is pressed until you press A in the window (the physical A: the sword hand's primary
button, read by its action, so the presses VR makes never count). That press is latched: A is pressed for you, 2 steps
down and 1 up, until the move begins or the window closes (the game takes A only with the enemy's attack frame past its
window's start). The move, the view held on the enemy and the finishing cut stay the game's own. A swing in the window
still parries as before. Logged: `[vr] parry: A pressed in the window: the parry taken`.

**The finishing cut is your swing again (2026-11-10).** Asked for: "after A is pressed then allow the player follow and
execute the attack". `kParryAutoCut` is false: after your A the game makes the roll, jump or leap (the view held on the
enemy, Link kept within reach), and its finishing cut waits for your swing in the move (`parry_cut_wait`), the game's own
cut after 1.5 s without one (`kParryCutWait`), as before 2026-11-09.

**Only A starts a parry (2026-11-10).** A player's logs: of 7 parries, 5 began from a swing just before the window
(the 0.8 s buffer) and 2 from a flick of the sword hand, none from A; "they always expected A to be pressed first".
`kParryBySwing` is false: a swing's start or a flick in the window, a full swing in it, and a swing just before it
(buffered, or held for it) no longer start a parry. A swing in the window is an ordinary attack. Your A starts it as
above, and a swing in the parry's move still makes the finishing cut. The window's log line says `A parries`.

**Nearer for the finishing cut (2026-11-10).** A player's test against Bokoblins in Dragon Roost Cavern: every parry
rolled, but each cut his own swing began ended 63-70 units from the enemy and moved it only about 100 units, while the
one cut that clearly landed (the game's own, 621 units) ended at 41. Link is now held within `kParryStayReach` 40 units
(60 before) and drawn in up to `kParryPull` 15 units a step (10 before). The cut-over line also says how far from the
enemy the cut began and whether its attack registered a hit (`the cut began N units from it and hit something /
nothing`), so the next log shows whether the cuts land. Not yet tried in the headset.

## Blocking (audit, 2026-11-10)

How the game blocks (decomp): Link guards while `daPy_lk_c::checkPlayerGuard` is true: the shield-up guard pose on his
upper body (`ATNG` 0x16 / `ATNGHAM` 0x1B, set while the attention lock is held with a shield and no attack or item
going: `checkGuardAccept`), the crouching guard (0x0C) or a guard slide (0x0D, 0x6D). Guarding turns on his body
cylinder's TG shield (`OnTgShield`), which turns an attack from within its front range (`mShieldFrontRangeYAngle`, his
model's facing plus his body's turn) into a block (`checkSuccessGuard`: the TG shield hit flag). The shield model's
place plays no part: it is the guard state and his facing.

What VR does to it:
- Third person: nothing. The shield hand's trigger is ZL, as the GamePad's; the game's guard as intended.
- First person: the shield raised (above, "Shield down, shield up") holds ZL and locks onto the nearest enemy; locked
  on, the game raises the guard itself. VR does not turn Link while he is locked on (the body follows the head only
  standing or walking unlocked, actions 4 and 5), so his facing, the guard's front, is the game's: towards the target.
  The physical sword is off while locked on. A swing presses the game's attack, which lowers the guard for that
  attack, as on the GamePad. The guard ends when the lock does: the shield hand lowered 3 cm under where it locked
  (`kShieldUnlockDrop`), the target more than 500 units away, or the enemy gone.
- Logged (`[vr] guard: ...`): the guard up and down with the view, the lock-on and what held ZL (the shield raised or
  the trigger), each attack the shield blocked, and each hit taken with whether he was guarding. A test's log shows
  whether every hit came with the guard down.

**The block in first person: shield raised, grip held (2026-11-10).** The original game's block is the GameCube's R,
the GamePad's ZR: held with a shield, Link crouches behind it (`procCrouchDefense`, R status DEFEND) and blocks from the
front. Asked for: "raise shields then shield hand grip lowers us to guard". In first person, with the shield raised
(the shield hand up, "Shield down, shield up"), holding the shield hand's grip holds ZR: the block, as long as it is
held; let go, he stands again. The right stick held down is ZR as before, in either view: that is the block in third
person, as the original game's R. Logged: `[vr] shield: the grip held with the shield raised: the block` / `the grip let
go`. Not yet tried in the headset.

**Third person: the right grip is the D-pad, the left grip guards (2026-11-10).** Asked for: "L grip toggles Dpad so lets
change that to R grip to toggle dpad and then L grip can guard instead". In third person on foot, in play: the right
grip held makes the left stick the D-pad (`dpad_grip_side`), and the left grip held is the game's block, ZR (Link
crouches behind the shield, the original game's R; `left_grip_guards`). Outside play (an event, a menu) the right grip
alone is D-pad up (the Wind Waker), as the left grip was. The right grip's own R is not given there. On the boat
nothing changes: the left grip is the D-pad and the right grip is R. The right stick held down is still ZR in either
view. Logged: `[vr] third person: the left grip held: the block` / `let go`. Not yet tried in the headset.

**The boat's D-pad on the right grip too (2026-11-10).** Asked for: "swap the grips for boat controls dpad too so they
match 3rd person mode". On the boat, in either view, the right grip held makes the left stick the D-pad (it was the
left grip), and the right grip's R is not given there. The left grip does nothing on the boat (it guards only in third
person on foot). Not yet tried in the headset.

**The boat's R on the left grip (2026-11-10).** Asked for: "what did R grip do before? make L grip do it now". The right
grip was R, the item on R (and, at the shoulder in first person, the sword's draw, which it still is). On the boat,
where the right grip is now the D-pad, the left grip held is R, in either view; not with the left hand at its shoulder
(the item wheel's reach) or the wheel up. In third person on foot the left grip stays the block, so R is not on a grip
there. Logged: `[vr] boat: the left grip held: R`. Not yet tried in the headset.

**Third person: the right trigger is R (2026-11-10).** Asked for: "make R trigger the item button on 3rd person". In
third person, on foot and on the boat, the right trigger held is R, the item on R (the right grip that was R is the
D-pad there now). Not with the bow out (either trigger still draws and shoots it) nor in the boat's Free Roam Camera
(the triggers zoom it). On the boat the left grip is R as well. Logged: `[vr] third person: the right trigger held: R`.
Not yet tried in the headset.

**The boat's left grip: a D-pad with the right stick (2026-11-10).** Asked for: "let left grip toggle dpad too then, but
make it left grip + rstick dpad" (instead of R, which the right trigger is in third person). On the boat, in either
view, the left grip held makes the right stick the D-pad: pushed past half its reach, its strongest direction presses
D-pad up, down, left or right, and the camera keeps still meanwhile. The right grip with the left stick stays the D-pad
too. Not with the left hand at its shoulder (the item wheel's reach) or the wheel up. Logged: `[vr] boat: the left grip
held, the right stick is the D-pad: ...`. Not yet tried in the headset.

**First person: the raised shield guards by itself when locked on (2026-11-10).** Asked for: "when the player raises his
shield and is locked on to an enemy then the player should also be in guard mode; if there is no enemy then the player
needs to hold the shield up and shield grip to guard". With the shield raised and its lock-on on an enemy, ZR is held
(the game's crouching guard), as the shield grip does; with nothing locked, the raised shield plus its grip guards,
as before. The guard is let go while he attacks: a swing's or a parry's press (A or B), the parry's move, and 0.5 s
after either, so the cut is made standing. Logged: `[vr] shield: raised and locked on: the guard (ZR held)`, `an
attack: the guard let go for it`, `the locked-on guard ends`. To check in the headset: the parry's prompt still comes
while he guards locked on. Not yet tried in the headset.

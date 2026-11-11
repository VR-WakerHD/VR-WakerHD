# VR Skull Hammer

The hammer is held, turned and swung by the sword hand with the sword's code. Its attacks are the game's own.

## How the game swings it (decomp: `d_a_player_main_03.cpp`, `_04.cpp`, `_07.cpp`, `d_a_obj_shmrgrd.cpp`)

- **The trigger.** It swings from the item button it is on (X, Y or R), not from B (`checkNextMode`). With the left
  stick pushed to Link's side it makes a side swing; otherwise it makes a front swing.

  | procedure | id | cut type | timing |
  |---|---|---|---|
  | side swing | 0x51 | 0x11 | hits between animation frames 25 and 36 (rate 1.2 from frame 4) |
  | front swing windup | 0x52 | 0x12 | frames 2 to 28 at rate 1: 0.9 s, no attack |
  | front swing | 0x53 | 0x12 | the attack; checks the ground under the face, and where the face is below it, a quake, dust and the stop |
  | front swing end | 0x54 | 0x12 | |

- **Power.** `setAtParam(AT_TYPE_SKULL_HAMMER, 4, ...)`, with capsule radius 50.
- **Hammer-only reactions** come from the attack type and Link's cut type. For example the hammer switches
  (`daObjShmrgrd_c::set_damage`):
  - a front swing or jump cut from above pushes them down;
  - a side swing knocks them sideways.
- **Collision.** It uses the sword's three capsules (`setSwordAtCollision`), built from `mSwordTopPos` and `m36C4`.
  `execute` takes those from the hammer's base matrix: `l_execute_hammer_top` (160, 70, 0) is the face and
  `l_execute_hammer_root` (8, 0, 0) is at the hand. The shaft runs along +X. Measured in the game, in the model's root
  frame: face 160 70 0, head 160 0 0, root 8 0 0 (`[vr] hammer: face ...`).

## Held in the sword hand (`vr_sword.h` `melee_item`, `vr_game.cpp`)

The hammer is a tracked weapon, like the sword and the stick, so it uses the sword's whole chain. Nothing new samples
the controller or writes the hammer's model:

1. The sword hand's controller pose in tracking space (`sword_base`: its aim orientation, its grip position).
2. The world mapping, `g_map`, which the arms and the sword use.
3. **The grip.** `measure_held` measures it once per step, after Link's execute: the hammer's root frame in the frame
   of the hand the game holds it in. It is mirrored onto the other hand for right-handed play (`mirror_grip`). The arm
   IK turns the sword hand so this grip holds while the hammer follows the controller.
4. **The hammer's own orientation** (`held_sword_rotation`):
   - its shaft is along where the controller points, the same as a blade (with `vr.gripPitchDegrees`);
   - its face points away from the controller's top, so a downward flick of the wrist leads with the face;
   - `vr.hammerRollDegrees` (default 0; launcher and F1 > VR, "Skull Hammer roll") turns it about the shaft.
   - The axes are measured from the hammer's own model, not copied from the sword's: shaft = head − root,
     face = face − head.
5. **The draw** (`link_draw_begin`) places it from the posed hand and the grip at an absolute target, on every pass.
   The same many-pass placement fixed the sword's position flicker (docs/VR_HELD_ITEMS.md, "Placement every pass").
   Right-handed, the gripping hand shape moves with it.

The previous flicker and floating had two causes, both found and fixed in docs/VR_HELD_ITEMS.md: more than one
mapping writing a held item, and placements relative to where the last pass left a model. The hammer goes through the
single writer that fixed them. Before this change it stayed in the game's grip on the posed hand, and only its
collision followed the controller, through a mapping of its own.

## Swinging (`vr_game.cpp`: swing detection, `hammer_press`, `hammer_windup`)

- **Detection** is the sword's `SwingDetector`, with its calibrated thresholds unchanged. Only a full swing attacks:
  downward, left to right, or right to left. The sword's state rules apply: no attack while it is being drawn or
  sheathed.
- **The press.** A full swing presses the hammer's own item button for two logic steps instead of B. The button is
  found from the play's item numbers. A horizontal swing also pushes the left stick to the swing's side for those two
  steps, so the game makes its side swing and side-swing cut type. A downward swing makes the front swing.
- **The windup.** The hand has already made it. The game's windup would land the hit about a second after the hammer
  came down, so for a swing the hand made (and only then: the controller's own button keeps the game's timing):
  - the front swing's windup animation is ended (rate 0), and the game moves to the swing at its next step;
  - the side swing is moved to frame 24, just before its hits.

  Every procedure, attack parameter, cut type, voice and sound still runs.
- **No physical cut.** The sword's physical cut (hits without the game's attack) is not used for the hammer
  (`melee_attack` returns 0), because the hammer's reactions need its own cut type. Holding or moving the hammer never
  hits.

## Collision (`link_executed`)

While the game's attack is set, the three capsules are moved to the tracked sweep, as for the sword:
- [0] now;
- [1] the face's path since the last step;
- [2] from the head to the previous face.

For the hammer each segment starts at the **head's centre** (on the shaft at the face's distance), not at the hand.
So the handle and the hand do not hit, and fast swings are covered between steps. The radius (50), attack type,
power, cut type and hit groups are the game's. So are the game's hit checks (`dCcS`) and each target's own reactions,
including duplicate-hit rules: the game resets its hit list at each swing's `setAtParam`. Hits vibrate the sword hand
once per target per swing (`HitLedger`).

The face position (`mSwordTopPos`) is written from the tracked hammer. That is what the front swing's ground check
reads at the next step, so the quake comes where the real hammer meets Link's ground.

All of this runs in Link's execute, once per logic step. Interpolated frames only draw: they place the model and
swing nothing.

## Tested (desktop, no headset)

- `vr_test`, `test_skull_hammer`: the button lookup, the side-swing stick, the windup skip (which procedures, which
  frames, NaN), the head geometry, and the setting.
- Desktop stereo (`WWHD_VR_TEST_KEY=850:14:0:33` puts the hammer on X; `WWHD_VR_TEST_HAMMER=2100:v,2400:r,2700:l`
  makes full swings as the sword hand would):
  - **Downward:** windup 0x52 at step 2101, ended → swing 0x53 with the attack set at 2102 (one step after the press,
    instead of 26), cut type 12. The face hit the pier: the game's dust ring (frame capture), the attack off, then 0x54.
  - **Right / left:** side swing 0x51, cut type 11, attack set one step after the press for 9 steps (the game's
    window), back to 0x4.
  - **The controller's own button** (a native press): windup 0x52 at 3003, swing 0x53 at 3029 (26 steps), unchanged.
- The whole suite passes. Outside VR nothing changes: swings exist only with a tracked hand. The test aid is off
  unless its variable is set.

The desktop has no controllers, so the draw, the grip and the tracked collision ran on the game's own placement.
Those need the headset.

## Still to check in the headset

- The hammer in the sword hand:
  - its face direction and roll (tune `vr.hammerRollDegrees`);
  - the handle in the fist, with no gap;
  - steady while turning the head, walking and swinging, and over repeated swings;
  - left-handed and right-handed.
- Swings:
  - slow, fast and short ones (only full swings attack);
  - stationary and repositioning (no hits);
  - downward, horizontal and diagonal;
  - rapid repeats, and switching between the sword and the hammer.
- Hits: enemies, hammer switches (pushed down by downward swings), pegs, the ground quake. Hits land where the head is,
  not the handle.
  - The quake needs the face below Link's ground; whether a natural downward swing reaches it at the game's scale is
    to be seen.
- Tracking loss mid-swing, recentring, menus and events.

# VR held items: sword, hilt, shield, telescope

(The bow, hookshot, boomerang and grappling hook are held the same way; their pointing aim and laser: docs/VR_AIMING.md. The Skull Hammer is held and swung as the sword: docs/VR_SKULL_HAMMER.md.)

## How the game holds things (measured at run time)

Link is left-handed. The game attaches what he holds to his hand joints after his arms are posed
(`daPy_lk_c::jointCB1`, where the VR arm IK runs), so the items follow the posed hands:

- `mpEquipItemModel` (+0x4440): what is in his hand. The sword's blade model has 4 joints, with its root on joint 8,
  his left hand. Other held items (the telescope, ...) go in the same field.
- +0xCF8: a 1-joint model on the same hand as the blade, the sword's hilt.
- +0xE80: a 6-joint model on joint 12, his right hand, the shield.
- With the sword sheathed or an item stowed, the models sit elsewhere (on the back) and are not on a hand.
- `[vr] held` lines (`WWHD_VR_TRACE_HELD=1`) log the model, the game's hand, whether it is a sword, the hand it is
  drawn in and the handedness, at most once a second.

## The problems found

1. **Wrong hand.** The tracked-sword code moved `mpEquipItemModel` to the dominant controller in the raw tracking
   space, not into Link's posed hand. In right-handed mode the blade went to the right controller while the hilt
   (+0xCF8, a separate model) stayed in Link's left fist: the blade was in one hand and the hilt in the other.
2. **Flicker of the sword and the telescope.** The same code ran for whatever was in `mpEquipItemModel`. For the
   telescope, and for models that are not in a hand at all, it built a blade from the game's sword tip (stale for a
   telescope), so the item was turned and placed arbitrarily, often into the camera. During attacks, the blade axis
   came from the tracked tip (written by the swing) against the animation's root, so the sword flipped direction
   from frame to frame. The placement also used `g_map`, whose anchor two callers set differently (the view's
   interpolated anchor and the swing's step anchor), so the item jumped between them. Its matrices were not marked
   changed for the GPU copy either.

Not the cause: depth, near-plane clipping, culling and per-eye state. The item was drawn where these computations
put it, and the trace showed the transforms jumping.

## Now (`vr_game.cpp`)

- **One handedness rule:** `dominant_side()`, `vr.leftHanded`. It decides the sword's buttons
  (`openxr_session.cpp` `sync_input` uses the same setting), swing detection, haptics, the swing's collision, and
  the hand the sword is drawn in.
- **`measure_held` (each logic step, after Link's execute):**
  - which hand the game attached the item to: its root within 3 units of hand joint 8 or 12;
  - whether the item is a sword: the game's sword tip and blade root stay fixed in the model's own frame while the
    hand moves. Proven sword models are remembered per stage. A telescope's stale tip does not move with it, so a
    telescope is never treated as a sword;
  - the tip and blade root in the model's frame.
- **`link_draw_begin` (every pass), moving models rigidly:**
  - the sword (blade and hilt together): the grip in Link's sword hand, the dominant side's hand as the arm IK posed
    it, with the blade along that controller (`held_sword_rotation`). The hilt gets the same move as the blade, so
    they stay one sword;
  - any other held item: the game's own attachment. When the dominant hand is not the game's, it is moved rigidly
    from the game's hand to the dominant one;
  - right-handed: the shield moves from the right hand to the left (the sword hand and the shield hand swap);
  - anything not on a hand: untouched (sheathed, stowed, cutscenes, menus);
  - the moved models' matrices are marked changed, as the arms' are.
- **Hand shapes:** when the sword moves to Link's right hand, the gripping hand shape goes with it and the left hand
  opens. Link's hand indices mLeftHandIdx / mRightHandIdx (+0x68DC / +0x68DD, see the sword grip section) are set at his draw and stay set until just before his next
  execute (`link_executing`, in the daPy_Execute hook), where the game's values are put back, so his logic never
  sees the change. Setting them during his execute made the sword disappear: they are game state. Restoring them at
  the end of his draw instead made the left hand flicker between open and the game's fist (reported in the
  headset), because the frames between logic steps are drawn from what the game built without calling his draw. With the shapes kept until the next execute: the left hand stays open, with no flicker (reported in the headset).
  The hand the shield is moved to closes round its grip (`g_drawShieldHand`, `apply_hands`), as the game closes its
  own shield hand with the sword out (`setDrawHandModel`: the sword with a shield, right hand 8); it had stayed open
  beside the shield (reported in the headset, 2026-11-03).
- **The swing's collision:** uses the same blade as the draw. The grip is in the posed sword hand and the rotation
  comes from the same function, so the hit capsules and the drawn blade agree. Without a posed hand (arm IK off),
  the collision falls back to the controller in the tracking space through a mapping of its own; `g_map` is no
  longer changed there.

## Tested

- VR, right-handed: the sword is detected (`model ... is a sword`), the game's hand is 8 and it is drawn in hand
  12. A capture shows the hilt and blade together in Link's right hand. Reported in the headset: no more flicker of
  the sword and the telescope.
- VR, left-handed: drawn in hand 8, the game's own hand, oriented by the left controller.
- `vr_test` passes.
- Reported in the headset, right-handed: the right hand grips the sword, the left hand is open.
- Still to check in the headset:
  - the shield in the left hand when right-handed;
  - the telescope in the dominant hand;
  - fast swings hitting (the collision now starts at the posed hand).

## The sword grip

**The rig.** Link's hands have no finger bones. Each hand is one joint (8 left, 12 right) with whole-hand shapes the
game's draw picks from a hands model (`daPy_lk_c::setDrawHandModel`): fixed ones for the held item (`mEquipItem`
+0x69B0), else `mLeftHandIdx` +0x68DC (0 open, 3 the fist) and `mRightHandIdx` +0x68DD (0 open, 8 closed around a
grip). Earlier versions wrote +0x69B1 / +0x69B3, the held item's low byte and the next field, not hand shapes. The fingers' curl is the
game's own sword-grip shape. What made the grip unconvincing was the wrist: the sword turned with the controller's
grip pose while the hand turned with the arm IK (the controller's aim and roll), so the hilt sat at an angle to the
fist.

**Now the hand is the reference, and the controller decides the hand through the sword:**

1. **The game's grip** (`measure_held`, each step). This is the sword's root frame in the frame of the hand the
   game holds it with: the game's own attachment, a constant of the model.
2. **The other hand's grip** is the game's grip mirrored across Link's body with his rig's convention. His left
   hand's axes, reflected, land on his right hand's as (+1, -1, +1); measured from the animation pose of both hands,
   with dot products of 0.99 to 1.00 (`[vr] hands mirrored`). The sword's axis across the body is flipped back, so
   the mirrored grip is a rotation again (`mirror_grip`).
3. **The arm IK** (`link_arms`) places the wrist as before. For the sword hand only, the hand is then turned so the
   grip holds while the blade follows the controller (`held_sword_rotation`). It blends with the arm's tracking
   weight, so it fades in and out with tracking.
4. **The draw** places the blade and hilt from the posed hand and the grip. Interpolated passes follow the hand.
5. **The swing's collision** uses the same hand and grip.

**States.**

- Only with a drawn sword in a hand (measured each step) does the grip turn the hand.
- Sheathed, stowed, or with another item: the hand keeps the arm IK's pose and the game's shape.
- During attacks the sword still follows the controller. The game's attack animation does not move the hands
  while they are tracked.

**Handedness.**

- Left-handed: the game's grip on the game's hand. Nothing is mirrored and the sword stays where the game attaches
  it.
- Right-handed: the mirrored grip on the right hand, with the hand shapes swapped for Link's draw only.

**Tested.** VR, right-handed, controllers still. A capture shows the right fist closed around the hilt's wrapped
handle, the guard just above it, the pommel below, the blade along the controller, and the left hand open. Still to
check in the headset: moving and rotating the controllers, swings, and left-handed mode.

## An item moved to the other hand: its mirror axis

An item other than the sword moved to the player's hand (right-handed: the Wind Waker, the telescope from Link's left
hand to his right) is the game's attachment mirrored across his body, flipped on its own axis that points across the
body. That axis was chosen again each step from where the item is in the game's hand now. With that hand on its
controller, a turn of the hand changed it, and the item flipped round in the other hand (reported in the headset: the
Wind Waker, as the left hand reached out to the left to play a note). It is now chosen from the animation's pose of
the game's hand (`link_arms`, before the arm IK), summed over the steps the item is held (`Held::acrossVotes`), as the
shield's axes are.

## Placement every pass (no position flicker)

Link is drawn several times per logic step: once per frame shown, three per step at 90 fps. The draw used to move
the sword, hilt and shield only when they were still exactly on the game's hand. After the first pass of a step
they were already moved, so the later passes skipped them. Depending on what frame interpolation did with them,
those frames showed the items where the last pass left them or back on the game's hand: the sword's position
flickered from time to time (reported in the headset).

Now:

- **The attachments are measured once per step, after Link's execute** (`measure_held`), before any draw moved
  them: the held item in its hand's frame, the hilt in the blade's frame, and the shield in its hand's frame.
- **Every draw pass places each model at an absolute target from those** (`place`). The result is the same however
  many passes there are and wherever the model was left. The hilt is always placed from the blade's target, so the
  two cannot separate.
- **The game's hand gets a little slack:** 6 units instead of 3 for the hand the item was on the step before. For a
  moment of an animation the item does not drop out of the hand.
- **A hilt or shield found where the draw last put it** (the game did not attach it again that step) keeps its
  previous attachment.

The input summary logs how many draws placed a held item (`[vr] held: placed in N draws in 10 s`). In a VR check it
was 576 to 585 draws per 10 s, every frame drawn.

## The Deku Leaf

In the left hand in both left- and right-handed play (`vr_sword.h` `fixed_item_side`), as the hookshot, the grappling
hook and the boomerang are (asked for in the headset). The game holds it in Link's left hand too, so right-handed it is
no longer moved to the sword hand.

While Link holds it (`mEquipItem` 0x34):
- **The left trigger is its item button** (pressed while the trigger is held), not Z-targeting (`filter_pad`, as for
  the boomerang).
- **A quick swing of the left hand up or down** presses its button for two logic steps (`vr_swing.h` `FlapDetector`:
  faster than `vr.swingStartSpeed`, more up or down than across, `vr.swingDownDistance` that way, once per swing).
  The sword's swing detection judges only swings down and across, so the leaf has its own.

What the press does is the game's: a fan swing on the ground, the glide opened in the air. Taking the leaf out is as
before (its item button or the item wheel).

## The Master Sword's glow

The Master Sword's light (the full-power sword's glow, and the light-saber effect of the Chance mode and the soup) is a
model of its own, `mpSwordModel1` (Link +0x4978). Each step the game copies the blade's base matrix into it
(`setItemModel`), and it calculates the glow's joints in Link's draw (`updateDLSetLight`). The draw moved the blade
and hilt, so the glow stayed where the game holds the sword, in Link's left hand (reported in the headset,
right-handed).

Now each draw pass moves the glow's base matrix with the blade (`link_draw_begin`), before the game calculates it
later in the same draw. It moves from the blade's root as it is now, so later passes of a step move it no further. Its
drawn joints follow the hands every frame with the blade (`frame_correct`). The light-saber emitter follows the
glow's joint: the game's particles are calculated earlier in the frame than Link's draw, so the emitter takes the
glow from the frame before.

## Why an arm stops following its controller

The log has one line each time an arm stops following its controller or starts again, with the reason:

- `stops: not normal play (an event, cutscene or menu)`: the game's camera shows Link from outside, so his arms
  keep the animation;
- `stops: the controller's pose was lost for more than 2 s` or `the headset's pose ...`: a tracking gap longer than
  the hold (docs/VR_CONTROLS.md, "Tracking gaps");
- `stops: no controller input`, `not recentred yet`, `arm IK is off`, or `Link's arm joints did not check out`.

The game writes its log to `captures/wwhd.log` by default (since 2026-11-03; Settings > Graphics > "Write a log file"
off, or `WWHD_LOG_FILE=0`, turns it off).

## The sword's tilt in the hand

The blade points along the controller's grip direction (through the fist), tilted by `vr.gripPitchDegrees`. The
hand follows the sword through the game's grip (above), so this angle is how the sword and Link's fist sit against
your real hand. The default was −40: the blade pointed 40 degrees below the line through the fist and did not look
held (reported in the headset); −10 and +20 were still too low. It is +45 now, tilted up out of the fist. Settings
format 5 moves a saved −40, −10 or +20 (the earlier defaults) to +45 once; any other value is kept. "Sword tilt" in
F1 > VR changes it live while holding the sword. +45 confirmed in the headset. "Sword tilt" in the launcher and in F1 > VR adjusts it,
positive up.

## Steady while running

The sword went up and down while Link ran (reported in the headset). The cause: the running animation lifts and
drops Link's shoulders with every stride, measured at 7 to 16 units a second, and his arm, hand and sword hung from
that shoulder. It was worst at full reach, where the hand is clamped to the reach around the shoulder. The hand's
target against the eyes was steady (0.02 to 0.9 units per step at 14 units per step of running), and frame
interpolation blends the sword with the body (its cut threshold is 400 units), so neither was the cause.

Now (`link_arms`) the upper arm's root is held at a settled place in Link's own frame (his position and heading).
A 9-unit dead zone holds it through the stride's bob, and it follows real changes of posture (crouching, climbing,
swimming) over 0.3 s. The clavicle turns to put the shoulder there (at most about 34 degrees); what a turn cannot
reach (the bob along the clavicle) moves the arm up to 6 units. Measured while running: the shoulder held exactly
(0.00 units from its place); only posture changes moved it. `WWHD_VR_TRACE_HAND=1` logs the shoulder's bob and the
hand against the eyes.

## Drawing and sheathing (`vr_sword.h`)

**One state, from the game's own facts.** `SwordMachine` keeps one state:

- **Sheathed**: not in a hand.
- **Drawing**: a shoulder draw was asked for.
- **Held**: the sword's model is on a hand.
- **Attacking**: held, with the game's attack set.
- **Sheathing**: it just left the hand.

It follows the game: a draw that does not reach the hand within 1.5 s gives up, and leaving the hand settles to
Sheathed after 0.5 s. A sword drawn another way (the game, an event) becomes Held. The log lists every change
(`[vr] sword: sheathed -> drawing`).

**Gates**, in first person with motion controls (third person plays as on the GamePad):

- A real swing presses B (an attack) only while the sword is held. A swing with the sword sheathed does nothing.
- The attack button (B) draws the sword as in the game, with its slash (asked for in the headset). Only a real
  swing of the controller does nothing while the sword is sheathed.
- No hit unless the sword is held for an attack: while sheathed or drawing, and for 0.6 s after a shoulder draw
  reaches the hand (the draw's own slash), the attack's capsules are switched off before the game's collision
  check.

**The shoulder draw.** Reach over the sword hand's shoulder and press its grip:

- **Where.** Within 0.28 m of a point 15 cm to that side, 8 cm below and 12 cm behind the head, in the body's frame
  (`ShoulderReach`; out again beyond 0.34 m).
- **What counts.** The press of the grip, not a held grip, while the sword is sheathed and nothing is in the hand.
  It presses the game's own draw (B for two steps), whose slash is kept from hitting.
- **Elsewhere.** A grip pressed anywhere else only closes the hand, as before.
- **Sheathing** stays the game's: A, "Put Away", as on the GamePad. A shoulder gesture is not used for it, because
  A also talks and rolls.

**Tested:**

- `vr_test`: the state's transitions and gates, a draw that gives up, and the reach's hysteresis and sides.
- In VR, B: the sword drawn (`sheathed -> held`), and in Link's right hand from the first frame of the draw.
- In VR (right-handed): a grip press with the controller over the right shoulder drew the sword. The log shows
  `drawn from over the right shoulder`, then `sheathed -> drawing -> held`, with the sword in Link's right hand.
- Still to check in the headset: swings and B doing nothing while sheathed, a swing attacking after the draw,
  left-handed mode.

## In the right hand from the first frame

The sword appeared in the game's hand (Link's left) for a moment when it came out, then moved to the player's hand
(reported in the headset). The code recognised the sword by its movement: the game's tip and root staying fixed in
the model's frame while the hand moved, learnt anew on every stage. That took a step or two, and until then the
blade was moved without its hilt and grip. Now the sword is recognised by the game's item number (`mEquipItem`
+0x69B0 = 0x103) from its first step in a hand. The grip (the sword's frame in the game's hand) is measured on that
same step, since it needs no blade. In VR captures taken right at a B draw, with the slash still in the air, the
sword was in Link's right fist and his left hand was open. Confirmed in the headset: B draws the sword and it appears in the right hand straight away.

## Swings: full swings only, glitches, flurries (TPVR comparison)

Compared with TPVR's `vr_swing_detector.hpp` (JoeyAW/TPVR) and its physical-sword wiring (`vr_main.cpp`
`g_physicalSwordSwingActive`, `d_a_alink.cpp` `setAtCollision`). Adopted from it:
- **Tracking glitches.** A tip faster than 30 m/s (TPVR: hand faster than 15 m/s, after a captured 31 m/s one-frame
  spike) is not measured and starts nothing (`kMaxPlausibleTipSpeed`).
- **Re-arming on a reversal.** Back the other way at swing speed for `vr.swingMinTime` starts a new swing, even within
  the cooldown or without slowing down. In a fast back-and-forth the speed never drops, so its swings after the first
  were lost.

**The physical sword** (`vr.physicalSword`, on by default; `PhysicalSword` in `vr_swing.h`, `link_executed` /
`link_executing` in `vr_game.cpp`), as TPVR's: while the sword hand swings, Link's three attack capsules are armed
along the tracked blade without the game's attack (no animation or sound), so a swing cuts grass and hits enemies
where the blade goes. Full swings still press B for the game's attack.
- **When:** from a swing's start (the detector's Active phase: `vr.swingStartSpeed` for `vr.swingMinTime`) to its end
  (`vr.swingEndSpeed`) and 0.1 s after (`kPhysicalHold`, TPVR's hold). Only in first person, in gameplay, with the
  sword held for an attack (`SwordMachine::hits_allowed`: not sheathed, not the draw's own slash) and while no attack
  of the game's own is set (those capsules already follow the tracked blade).
- **How:** after daPy_Execute (which registers an attack only if it was armed in its own logic) bit 0 of each capsule
  is set, its geometry is the swept tracked blade (as for the game's attacks), and it is entered into this step's
  collision as Link's attacks are: `cCcS::Set` (0200E240, the game info's + 0x26A4) and the mass collision
  (02516C14, + 0x4EF8, flag 1: grass and bushes). On each new swing first `setNormalCutAtParam` (023E5B0C: a basic
  slash, cut type 3 to the right, 4 to the left) and `ResetAtHit` (02516138), so a new swing can hit a target again.
- **Unarmed before Link's next execute** (`link_executing`): his logic never sees an attack it did not start. Hits of
  the last collision check vibrate the sword hand there first, once per target and swing (`HitLedger`).
- Third person and VR off: unchanged.

**Full swings only** (asked for in the headset: a swing's start, a thrust or a flick played the attack). The game's
attack (B: its animation and sound) is pressed once per swing (`SwingEvent::full`), when the swing has committed. The
tip has to have travelled `vr.swingFullDistance` (0.3 m) across or down, in the body's frame, or the blade has to have
turned `vr.swingFullDegrees` (35) with half that travel. The direction is the tip's path:
- **mostly across:** a swing to the right or left;
- **mostly down:** a chop;
- **mostly forward or back** (a thrust, a wind-up) **or up:** no attack, though the swing is judged again as it goes on.

The swing's last frame counts too. History, all measured in the headset with `[vr] swing: ended ...` logs:
- **70 degrees of blade turn** came too late: the game's hit window (two steps after the press, about five steps long)
  mostly fell after the swing. Arm-driven swings (0.5 to 0.9 m across with under 70 degrees of turn) never counted.
- **The blade's turn axis** did not show side swings with the sword tilted up: 0.7 to 0.9 m across was read as
  neither across nor down.
- **Now** downward chops register (36 of 60 attacks in the run before the path change).

Tests (`vr_test` `test_swing`): a full swing once, after 70 degrees, to the right; a 41-degree flick never; a
downward swing full and an upward one not; flurries and reversals; a 45 m/s glitch.

### The hand, not the tip; the sword from where the controller points (settings format 6)

All three of these were reported in the headset.
- **Wrist turns started side attacks.** The detector measured the blade's tip, and a 35 degree wrist turn carries it
  0.5 m with no swing of the arm. Swings are now measured on the hand (the blade's root, at the grip), as TPVR does.
  The speed is the hand's (`vr.swingStartSpeed` 2.0, `vr.swingEndSpeed` 1.0 m/s; TPVR 2.2). A swing attacks once the
  hand has carried `vr.swingFullDistance` (0.25 m) across or down in the body's frame, with its direction that path. A
  wrist turn alone never starts a swing (`vr_test`). `vr.swingFullDegrees` is no longer used.
- **Downward chops were missed.** A chop's tip motion is mostly the blade pivoting; the hand's path down is the clean
  signal.
- **A forward thrust stood the sword up into the face.** The tilt was measured from the line through the fist, which on
  Touch-style controllers is 44 degrees above where the controller points (logged at start). The player's 60 degrees
  was therefore 104 degrees above it, and a thrust pointing the controller forward put the blade up and back. The
  sword's base is now where the controller points (OpenXR's aim pose, `sword_base`), and `vr.gripPitchDegrees` (20)
  tilts it up from there. With the wrist straight (`vr.swordWristStraight`), the hand no longer turns the sword: the
  sword keeps the controller's rotation for the step (`g_swordRot`), drawn at the hand's grip, and its collision uses
  the same.

Settings format 6 moves a saved tilt (from the fist) to the new default. It also moves the old tip-speed defaults (2.6
/ 1.2) to the hand's (2.0 / 1.0); other chosen speeds stay.

### More sensitive (settings format 7)

Asked for in the headset: most side slices did not register, and down swings should register more easily. In the
previous run, side slices carried the hand 0.19 to 0.43 m and chops 0.24 to 0.49 m; one threshold of 0.25 m for both
missed many of them, and a hand start speed of 2.0 m/s left slower slices unstarted.
- **Separate thresholds.** A side slice attacks once the hand has carried `vr.swingAcrossDistance` (0.12 m) across, a
  chop once it has carried `vr.swingDownDistance` (0.15 m) down.
- **Lower hand speeds.** 1.6 to start, 0.8 to end; format 7 moves the format 6 defaults (2.0 / 1.0).
- **The trade-off.** A vigorous wrist turn moves the hand a little and may count again; raise the thresholds if it
  does.
- **Where to set them.** All swing sensitivity values (start and end speed, how long fast, side and down travel, the
  pause between swings) are in the launcher, under Controllers > Swing sensitivity.

**Confirmed in the headset** (by the player): the side slices, chops, wrist-turn rejection, forward thrust and swing sensitivity settings behave as intended.

## The shield in the left hand (upside down, fixed)

Link is left-handed: the game holds his sword in his left hand and his shield in his right (joint 12). For a
right-handed player VR moves them: the sword to his right hand, the shield to his left (joint 8). The shield's
attachment is measured each step in his right hand's frame (`measure_held`: its root in the hand's frame) and placed
from his left hand (`link_draw_begin`).

**The cause:** the attachment was applied to the left hand unchanged. His two hands' frames are mirror images (the
rig's convention `g_mirrorSign`, measured once from both hands), so the right hand's attachment held the shield
reflected in the left one: upside down.

**The fix:** the shield is mirrored onto the other hand exactly as the sword's grip already was. `mirror_held` (in
`vr_ik.h`, shared with the sword's `mirror_grip`):
1. maps the hand-local vectors through the mirror;
2. turns back the shield's own axis that points across Link's body (`across_axis`, measured with the attachment).

The shield is symmetric about the plane of its top and its face, so its top stays up and its face outward, and only
its side-to-side axis turns. The hand's tracking, the arm IK and the game's shield (its blocking collision, its
animations) are unchanged. If the rig's mirror could not be measured, the shield is placed as before. Left-handed
players hold the shield in Link's own shield hand, which the game places itself, so nothing changes for them. The log
says `[vr] shield: moved to Link's left hand, mirrored ...` once.

**Tests:** `vr_test` uses a synthetic right hand and its mirrored left hand, with a shield upright and facing forward
in the right hand. After the move it is still upright and facing forward, at the mirrored place. A wrist turn turns
hand and shield together. The unmirrored transfer, the old behaviour, fails the same check. Still to check in the
headset: the shield upright in the left hand, its face outward when raised, and wrist turns.

## The stick (and other weapons picked up from enemies)

**The problem:** the VR sword path (the grip, the hand turned to the weapon, the tracked draw, the physical swing and
its collision) recognised a held sword only by Link's item number, the sword `0x103`. A stick picked up from a
Bokoblin, like any weapon taken from an enemy, is `daPyItem_BOKO_e` `0x101`. So it stayed in the game's hand, played
the game's animation, and could not be swung.

**The fix** (`vr_sword.h` `melee_item`, `melee_attack`; `vr_game.cpp`):
- **Held, tracked and swung as the sword:** the same grip measured from the game's own attachment of this step
  (whatever the weapon's model), the sword hand, the draw at the controller, swing detection, and the physical swing's
  collision along the drawn weapon.
- **Its own rules:** `mEquipItem` is never changed, so the game's cuts still branch on it (damage, fire, breaking,
  dropping and putting it away are the game's). The physical swing's power, sound and hit mark are the weapon's own
  (`setEnemyWeaponAtParam(FALSE)`, per its type from its actor), not the sword's slash (`setNormalCutAtParam`, which
  would give a stick the sword's power).
- **No weapon actor** (`mActorKeepEquip`, `+0x6594`, empty): no physical attack, as the game makes none
  (`procJumpCut_init`).
- When it breaks or is dropped, the item number changes and the next step's measurement drops it; the sword's path
  is unchanged.

**Tests:** `vr_test` `test_melee_items`.

**Not checked:** a stick in the game or the headset: its grip and orientation in the hand, swings hitting enemies and
grass, lighting it, breaking and dropping it, and the sword afterwards.

**Why the first fix was not enough:** recognising the item number turned the sword path on, but everything in that path
(the held-model measurement, the game's hand, the grip, the tracked draw) read Link's own weapon model
(`mpEquipItemModel`, `+0x4440`). A picked-up weapon is never that: it is its own actor (`daBoko_c`, carry mode). Link's
`setItemModel` places that actor's model (`+0x3B4`) from his left hand at a fixed offset, about 62 units out, and the
actor follows its model (`procCarry`). So the stick was never found in a hand: the sword stayed "not held" and the
tracked draw never moved it. In first person, the sword's gate then switched off the game's own attack capsules
(no hit unless the weapon is held), so even the game's stick attacks could not hit.

**Now** (`held_model`, `measure_held`, the tracked draw):
- The held model is that actor's model while Link holds a picked-up weapon.
- Its hand is his left, where the game attaches it.
- The blade check allows its root far from the handle.

The log line `[vr] held: item 0x101 (a tracked weapon), model ... (a picked-up weapon's actor)` shows it was
recognised. `[vr] sword: ... -> ...` shows the weapon's state, and `WWHD_VR_TRACE_HELD=1` traces every step.

**The stick, after the headset tests:**
- **It wandered:** the grip was measured from the stick's joint matrices, which still held the tracked draw's last
  placement. It is now measured from its base matrix.
- **It floated beside the hand:** its root joint (what the draw moves) is offset from its base. That offset is learnt
  once, from a root joint the weapon calculated itself, so it is held in the fist.
- **Swinging threw it:** every physical cut with it was followed by Link dropping it (5 of 5), and the game's own
  attacks were not (2 of 2). Picked-up weapons have no physical cut. The real cause was the button: in the HD game the
  do button (A) swings a Bokoblin's weapon and the sword button (B) throws it (`d_a_player_main_03.cpp`), and a full
  swing pressed B. With a picked-up weapon a full swing now presses A (`vr_sword.h` `swing_presses_do_button`): the
  game's own stick attack, with its own collision, power and fire. The primary button (A) swings it too.
- **The overhead hide hid the stick:** the game keeps the held stick as grabbed too, so `hides_held_object` skips
  Link's equipped weapon's actor.

**Lighting it (after the headset test):** a stick touched to a fire did not light. Only its draw had been moved to the
hand (`weapon_drawing`). Its base matrix (`J3DModel +0xC8`), which Link's `setItemModel` sets from his game hand in his
execute, stayed there, and the stick's own execute (`daBoko_c`) calculates its model from that base. Its tip
(`boko_top_root`, `boko_get_top`), what touches fire there, its flame's particles (`boko_flame`, `dPa_set`) and the
flame Link reports to torches (`daPy_lk_c::getBokoFlamePos`: the held stick's actor burning, `+0x43C`, at Link's
`+0x3E4`) were all where the game held it, not where it was seen.

Now `place_weapon_base` (`vr_game.cpp`, from `link_executed`, after `measure_held`) writes the base at the tracked
frame (`weapon_target`, the same one the draw uses: its handle at the posed sword hand, turned as the controller holds
it, moved by `vr.stickOffset*`; the base's own scale kept). From then on the stick's execute, its tip, its fire and
its flame are at the hand; the draw only corrects for the head and hand moving since. `measure_held` keeps the grip
measured before when the base is still where this put it (the game did not set it again that step), so the grip is
never measured from the tracked place. First person with motion controls only (`vr.trackedSword`, `vr.armIK`); third
person, the sword and the other items are unchanged. The log says `[vr] held weapon: its base at the tracked hand`.
Still to check in the headset: lighting it at a torch or a fire, its flame in the hand, lighting a torch with it, and
the grip staying put while held.

**Steady while the view turns:** turning with the right stick, the stick shook back and forth in the hand, as the
Power Bracelets had (docs/VR_ARM_ANIMATION.md). Every drawn frame re-aims Link's arms to the controllers
(`frame_correct`) and moves what he holds with the hand; the stick, its own actor's model, was not among them, so it
stayed at the step's hand while the hand moved on. It is now moved with the sword hand's per-frame move, whole (its
root is far out along it, past the 25 units in which a held model is matched to the nearest hand).

**In the hand as it is drawn (after the headset test: it still jittered).** Every drawn frame blends each model
between two steps' places on its own, joint by joint, along a straight line. The stick's joints, 60 units and more
out along it, and the hand it is in travelled along different lines between two steps (most while Link moved or the
view turned), so the stick drifted against the hand and snapped back at each step, 30 times a second; moving it
with the hand's per-frame correction afterwards kept that drift. Now each drawn frame places it rigidly from the hand
as drawn in that frame (`g_drawnHand`: blended, then moved to its controller), with its grip of the step
(`g_weaponGrip`: its base in the hand's frame, from `link_draw_begin`; its root in its base's frame, from
`weapon_drawing`), whatever its own blend did. Drawn before the body in a frame, it takes the hand of the frame
before. The log says every 10 s while one is held: `[vr] held weapon: in the drawn hand N passes, from the last
pass's hand N, not placed N`.

## Pots and barrels over Link's head

In first person, what Link carries over his head (`mActorKeepGrab`, `+0x65A4`) sat in front of the eyes and covered
the view. While he carries it (the actor's carry flag, `actor_status & 0x2000`) in first-person play, it is not drawn:
the game's own one-shot no-draw bit (`fopAcStts_NODRAW_e`) is set before its draw (`interp.cpp`, the `fopAc_Draw`
hook; `vr_game.cpp` `hides_held_object`). The draw clears the bit again, so as soon as he throws it, puts it down or
it breaks, it is drawn again from the next frame. A pot's contents appear only when it breaks. Third person, events
and non-VR play are unchanged. The log line `[vr] held overhead: actor ... hidden` marks each one.

**Thrown, it broke on Link (2026-10-31, reported in the headset).** A pot thrown in first person did not fly forward: it
broke on him. While he carried it, his arms still followed the controllers, and the carried actor is placed from his
hands as drawn: the pot sat at the controllers, low and against his body, unseen (above), and that is where the throw
started. While he carries something overhead (`carries_overhead`: the grabbed actor with its carry flag, not the stick
he holds as a weapon) his arms now take the game's carry pose (they fade to it over 0.15 s, as when tracking is lost;
`[vr] ... arm stops: Link carries something overhead`), so it is held, and thrown, from over his head. Not yet tried in
the headset.
- **It still floated beside the hand:** a picked-up weapon is drawn by its own actor (`daBoko_Draw`), which may come
  before Link's draw in a frame, so his draw placed it too late. It is now also placed at the tracked hand in a hook on
  its own draw (`tools/recomp/hooks_vr.txt` `020C0E04`, `vr_hooks.cpp`, `vr_game.cpp` `weapon_drawing`), from where
  Link's last draw put it. `[vr] held weapon: its own draw before/after Link's in this frame` tells which came first.
  The hook needs the recompiled code regenerated (`python tools/recomp/recomp.py game/code/cking.rpx build/gen`).
- **The draw order, measured:** the stick's own draw comes after Link's. Its placement now happens only there: its
  joints are moved from its base's frame (its own calculation from the base Link's `setItemModel` set), or from the
  frame a previous pass put them in, to the tracked one. The grip is the base's in the game's hand, mirrored to the
  sword hand as the sword's is. No offset is learnt any more: the learning depended on the order of the actors'
  executes.
- **The same hits as the sword:** the physical swing's collision along the tracked blade is back for picked-up
  weapons, with their own power (`setEnemyWeaponAtParam(FALSE)`). The throws had been the B button, not the cut. A
  full swing presses A and plays the game's own swing as well.
- **Placed by its handle:** its root is far out along it, where the mirrored grip's small errors put the stick beside
  the hand, too far left and out (seen in the headset). Its handle (the game's blade root) is placed at the hand,
  mirrored from the game's hand, and the weapon is turned as the controller holds it.
- **Its place in the hand:** moved by `vr.stickOffsetX/Y/Z` in the controller's grip space (metres: +x right, +z
  towards you; 0.03, 0, 0.10 by default, asked for in the headset: a little right and closer).

## The shield's face (after the headset test)

The shield is mirrored from Link's right hand onto the off hand. A mirror image is not a rotation, so one of the
shield's own axes is flipped to make it one.
- Flipping its up put it upside down.
- Flipping the axis across his body, its face, kept it upright but turned its crest towards his body.
- It is flipped on the third axis, across its own face (`vr_ik.h` `mirror_flip_axis`, with its up found by
  `up_axis` from the game's attachment). That keeps the top up and the crest out, and a symmetric shield shows no
  difference.

## The shield's orientation (flips with the wrist and the sword, fixed)

Two flips were seen in the headset with the shield in Link's left hand (right-handed play):
- **Turning the left wrist** flipped the shield's face towards the body.
- **Raising or lowering the sword** turned it upside down.

They had separate causes:
- **The wrist:** the shield was attached to Link's posed left hand, and that hand's orientation was not the
  controller's.
  - The arm IK points the hand along the controller with the shortest turn from the animation's hand, so it keeps the
    animation's twist.
  - It then adds the controller's roll, measured from level (`vr_arm.h` `controller_roll`). That roll is undefined
    when the controller points straight up or down, and jumps across ±180 degrees (or across its ±100 degree limit
    without Detached Arms).
  - The game animates that hand too: it is the game's sword hand, swung by its attacks.
- **The sword:** the axis the mirrored shield is flipped on (its face, its up) was chosen again at every step. It was
  chosen from where the shield was in the world. The game attaches the shield to Link's right hand, which in
  right-handed play is the tracked sword hand. So as the sword turned, the chosen axis changed, and the mirrored
  shield flipped with it.

**The fix:** the shield is turned by the off-hand controller alone (`vr_game.cpp` `link_draw_begin`, `vr_ik.h`
`shield_rotation`):
- Its face points out of the back of that hand (grip -X on the left hand, +X on the right) and its top up through
  the fist (grip +Y).
- It is built from these two directions as one rotation: no Euler angles, no roll, nothing from either posed hand,
  the sword or the animation. So it is the same for the same controller pose, and continuous through every wrist turn.
- The shield model's own face and top axes (with their signs) are learnt per shield model (`shield_axes`). They are
  summed over the game's own pose of its hand: Link's right hand as the animation posed it, read before the arm IK
  (`link_arms`), with the measured attachment. A moment of an attack does not change them. The log says
  `[vr] shield: its face is its axis ..., its top ...` once there are 30 steps, and again if they change.
- The hand stays where the game's attachment holds it, behind the face. That offset is mirrored across the shield's
  plane of symmetry (`mirror_across_face`) and placed at Link's posed wrist, which follows the controller.
- Until the axes are learnt, or without the controller's pose, the game's attachment is mirrored onto the posed hand
  as before, flipped on the learnt side axis once it is known. The log says which way it is placed whenever that
  changes.

Only where it is drawn changes. The game's shield (its blocking, collision, guard state and animations), the sword's
tracking and the arm IK are unchanged. Left-handed play still holds the shield in Link's own shield hand, where the
game attaches it.

**Tests:** `vr_test` `test_shield_orientation`:
- the axes from the summed poses, with outliers;
- the rotation for every axis choice and sign;
- the mirrored hand offset;
- the face outward and top up in either hand;
- full turns of the wrist about each axis: level, pointed up, pointed down and turned over. Each 2-degree turn of the
  controller turns the shield by no more than that, with no jump.

**Still to check in the headset:**
- the shield's angle in the hand (now the controller's, where before it was the game's attachment);
- the wrist turns;
- raising and lowering the sword;
- blocking;
- whether `vr.shieldOffset*` needs retuning, since it is applied in the same grip space but around a differently
  turned shield.

Its place in the hand: moved by `vr.shieldOffsetX/Y/Z` in the off-hand controller's grip space (0, 0, -0.13 by
default, asked for in the headset: 13 cm away from you, so it sits in the hand).

Both offsets (`vr.stickOffset*`, `vr.shieldOffset*`) apply in right-handed play only (`vr.leftHanded` off), where they
were set in the headset. Left-handed play places the stick and the shield as the game's attachment does.

## The stick steady in the hand (2026-10-28)

Three causes, found with per-frame counters in the headset:
- **The held item read as nothing on two frames of three.** For Link's hand shapes his draw sets mEquipItem (+0x69B0) to
  nothing until his next execute (apply_hands; the game's value kept, `game_item`). The draw-time code read the field
  itself: `held_model`, `link_draw_begin` and `frame_correct` saw no stick on the in-between frames, and placed it
  another way on those (the stick in the drawn hand on 241 of 476 frames; now every frame). They read `game_item`.
- **The grip was taken at every frame.** It set the controller's pose of the frame against the hand of the step, and the
  frame then placed the stick from the hand as drawn: the hand's move since the step counted twice. It is taken once a
  step, at its first draw.
- **Its geometry was measured at every step.** The handle, tip and base in its own frame, and the axis a mirrored grip
  flips, took on the game's walking animation, and where the measured blade crossed an axis the stick turned half round
  in the hand, its base 30 to 40 units away (7 times in a session). They are measured while it settles (15 steps) and
  kept until another weapon is held (`measure_held`).

The off hand, holding nothing (not the shield, not an item aimed with it), has its wrist in line with the forearm, as the
sword hand has (`vr.swordWristStraight`): it was tilted up off the arm.

## Put away after use (2026-10-31)

On the GamePad, Link keeps the telescope, the Wind Waker or a Picto Box in his hand after he looks through it or conducts. It stays there until another action puts it away, and the game still counts it as held. In first person it stayed in the player's hand (reported).

Now `vr_game.cpp` `link_executing` puts the item away with the game's own `daPy_lk_c::deleteEquipItem(link, TRUE)` (0x023DC7AC, with its put-away sound), so `mEquipItem` reads nothing (0x100) and the model is gone. `vr_sword.h` `PutAwayAfterUse` decides when, once a step before Link's logic:

- The item held is one of these: the telescope (0x20), the Wind Waker (0x22), the Picto Box (0x23) or the Deluxe Picto Box (0x26).
- It was in use since it was taken out: his action is `procScope` (0), TactWait (0x9A) or TactPlay (0x9B), the metronome is on, or the status shows the telescope or the Picto Box aimed.
- That use has ended, and he stands or walks in first-person play (actions 4 to 7). While a song's event plays or a menu is open, it waits.

In third person, nothing changes: the game's own behaviour applies.

## Sword in the fist, shield flush with the body (2026-10-31)

Reported in the headset, with clips: the sword's handle sat a little out of the hand, the hand around it looked open, and the shield stood sideways with its round end across the view. Twilight Princess VR was given as the reference.

- **The shield** (`vr_ik.h` `shield_face_level`, used in `link_draw_begin`) is held flush with the body. Its face points where the off-hand controller points (its aim pose's -Z), levelled. Its top is straight up, so its round end points down however high the hand is raised (asked for).
  - Pointing the controller up or down, the controller's top side takes over the level direction, so the face turns without a jump.
  - Before, its face pointed out of the back of the hand and its top went along the fist. How the controller was held turned it sideways.
- **The shield's offset:** `vr.shieldOffsetZ` -0.13 was set for the old orientation. With the new one it would push the shield out in front of the hand, so the default is 0. Settings format 11 moves a saved -0.13 (with x and y 0) to 0 once.
- **The sword** is moved by `vr.swordOffsetX/Y/Z` (new; F1 > VR "Sword in the hand"; metres in the sword-hand controller's grip space, +z towards you along the fist). The default is 3 cm towards you, so the handle sits in the fist. This is right-handed play only, where the game's grip is mirrored onto the hand, as `vr.stickOffset*` is for a picked-up weapon. The swing's collision is unchanged.
- **The sword hand** uses the game's fist around a stick's handle (right-hand shape 9, as `setDrawHandModel` uses for `daPyItem_BOKO_e`). Before, it used its shape around the shield's grip (8), which left the fingers open beside the handle.

**After the headset test:** the sword was confirmed. The shield sat with the hand's fingers going through it, not holding its handle: flush with the body, the hand points into its face. `vr.shieldOffset*` is now in the shield's own level frame (x right, y up, z towards you from its face). The default is 0.072, 0.016, -0.06: out in front of the fist, a little right and up, so the handle is in the hand. That was tuned and confirmed in the headset. Settings format 12 moves a saved 0, 0, 0 (format 11's default) to it once.

**Tilted with the wrist (asked for after that):** the shield also pitches with the off-hand controller (`vr_ik.h` `shield_face_tilted`). Tipping the wrist forward tilts its top forward, and tipping it back tilts it back. The pitch is the controller's own, up to 60 degrees either way, so the round end stays below. There is no roll: its top stays square to the face in the vertical plane. `vr.shieldOffset*` turns with the tilt (its frame is the tilted face), so the handle stays in the hand. Level, it is as before.

## Thrusts (2026-10-31)

Asked for in the headset: drawing the sword in and then pushing it out should be a thrust attack in the game.

- **Detection** (`vr_swing.h`, the full-swing check). A swing whose hand path is mostly forward is a thrust once the hand has carried `vr.swingThrustDistance` forward. The default is 0.10 m (0.15 at first; lowered on request to make it easier, and settings format 13 moves a saved 0.15 once). F1 > VR "Thrust: hand travel" changes it. The blade must point along that path (within about 53 degrees), so reaching forward with the sword upright never counts. Drawing the sword in first is a swing back, which never attacks. The push out then starts a new swing on the reversal.
- **The attack** (`vr_game.cpp` `thrust_press`). B is held for two logic steps with the left stick pushed forward. The game's own choice of cut (`daPy_lk_c::changeCutProc`) then picks its forward thrust (`procCutF_init`), with its animation, power and sound. The stick's direction is measured from Link's facing, or from the target when locked on. The stick is set after the movement, so it is only that for those two steps.
- **Not a chop (2026-11-06, "vertical slice still misses").** An overhead chop starts by moving the hand forward, so it
  was taken for a thrust before it came down (43 of 55 chops in one log; 18 of 22 locked on in a PSVR2 player's log
  after the first fix). A thrust now also needs: the path not heading down (under half its forward) nor up (the wind-up
  for the next chop); the blade under about 37 degrees up; and the blade turned under 30 degrees since the swing began
  (his chops' flat pushes had the blade coming down from overhead, turning 130 to 170 degrees over the swing, his
  thrusts 38 to 41). Otherwise the swing is judged again and becomes the vertical slash once it is more down. The log's
  `[vr] swing: a full ...` line gives the hand's path, the blade's angle and its turn since the start. `vr_test`: an
  overhead chop and the PSVR2 player's chop are each the vertical slash; a level stab and a stab with the blade 25
  degrees up are thrusts, locked on or not.
- **Easier vertical slash (2026-11-06, the PSVR2 player: "make it easier to do the slice").** His next log (build
  `f6a9e2e`) had no chops taken for thrusts, but 7 chops 0.42 to 0.59 m down and 0.42 to 0.64 m forward, the blade
  turning 125 to 147 degrees, attacked not at all: rejected as thrusts (heading down) and not yet the slash (more
  forward than down). A swing more forward than down that heads down (over half its forward) with the blade turning
  (over 30 degrees) is now the vertical slash; a thrust that dips, or a stab aimed down, keeps its blade's direction
  and stays a thrust. A downward swing also starts at 0.75 of the start speed with half the time when not locked on
  (a 1.58 m/s chop just before he locked on started nothing). Once a swing is the slash, its follow-through curving on
  down and back is the same swing, not a second slash. `vr_test`: a chop 0.6 m forward and 0.5 down, locked on and
  not, and a slow 1.3 m/s chop are the slash once each; a stab aimed 30 degrees down is never the slash; lowering the
  sword at 0.6 m/s does nothing.
- The Skull Hammer has no thrust, so a thrust with it does nothing.
- **Tests:** `vr_test` (the carried-blade swings): straight forward along the blade attacks once, as a thrust; straight back, or forward with the blade upright, never does. The whole suite passes.

## Bombs in the hand (2026-11-03)

Asked for with a comparison video ("can't see bomb in hand or throw in real time"; another VR game holds the bomb in
the hand and throws it with the arm). A bomb Link carries (`fpcNm_BOMB_e` 0x126, `fpcNm_Bomb2_e` 0x127,
`carried_bomb`), in first-person play:
- **Seen:** not hidden as other carried things are (`hides_held_object`).
- **In the sword hand:** his arms keep following the controllers (not the carry pose), and after each of his steps the
  bomb is placed at his posed sword hand, `kBombInHand` (12 units) out along the controller (`bomb_step`).
- **Thrown with the hand:** squeezing the sword hand's grip and letting go throws it. The grip no longer presses R
  meanwhile (R throws what he carries). The release presses the bombs' own button (`checkSetItemTrigger(BOMB_BAG)`:
  the game's throw), and when the game throws (`procGrabThrow`: 19 forward, 34 up a step, along his facing) the bomb
  gets the hand's velocity instead (taken over the last 0.1 s of its places), times `kBombThrowGain` (2), along the
  hand's own heading, within 45 forward and -20..55 up a step. `[vr] bomb:` logs the hold, the release and the throw.
- **Thrown with a throwing motion** too (asked for after the first test): the hand holding the bomb moving faster than
  `kThrowSpeed` (2.2 m/s), at least `kThrowAcross` (1 m/s) of it not straight up or down, throws it as letting go
  does, with that velocity. `[vr] bomb: a throwing motion` logs it.
Tried in the headset (2026-11-03): held in the hand, "bombs are good"; the throwing motion not yet tried.

**Other carried things thrown with the same motion (2026-11-06).** Asked for: "when Link is holding pots or nuts or
anything that asks press A to throw, lets allow the same throw motion from the bombs to throw the item". Whatever else
he carries over his head (`carries_overhead`, not a bomb) in first-person play is thrown by the bombs' throwing motion
of the sword hand (faster than `kThrowSpeed`, at least `kThrowAcross` not straight up or down): it presses A for two
logic steps, as the player's press would. Only while A reads "Throw" (do status 0xE, `checkNextActionGrab`), so never
its drop, a barrel's search or a door's open; the game's own throw, arc and animation (the arms keep the carry pose).
At most one every `kCarryThrowGap` (15) steps. A still throws as before. `[vr] carried: thrown by a throwing motion`
logs it. Not yet tried in the headset.

## The Deku Leaf over the head while gliding (2026-11-03)

Gliding (`daPyProc_FAN_GLIDE_e`, 0x93), Link's arms take the game's pose (as for carrying something overhead), so the
leaf is held above his head like a parachute, where the game puts it; with the arms at the controllers it hung at the
left hand and was not seen (asked for: "see it above your head ... but not so much that it covers your view").
`[vr] ... arm stops: Link glides on the Deku Leaf` marks it. The arms ease into and out of that pose over 0.45 s (in
0.15 s the leaf popped from the hand to over the head, reported in the headset).

The leaf's swing (`vr_swing.h` `FlapDetector`) went off too readily and too often at the sword's thresholds (reported):
a flap now needs at least 2.2 m/s and 0.3 m, and 0.6 s since the last.

**The leaf in your hands while floating down (2026-11-03, replaces the pose above).** Over the head, the leaf was out of
the first-person view while floating down (reported in the headset: "when we are floating down we should be able to see
the deku leaf"). Now the arms follow the controllers while gliding too, and each hand takes the turn the game's glide
pose gives it, so the leaf stands up over your hands wherever you hold them (`vr_game.cpp`, the arms: `glideHandRot`).
The arms still ease in and out over 0.45 s. In the headset the hands were right but the leaf was invisible: the glide
pose holds it in both hands, its base more than 3 units from either, so it counted as in no hand (as a sword on his
back) and was hidden in first person. Gliding with the leaf (the game's held item is then 0x102, the open leaf), it is now the nearest hand's (`measure_held`). Still unseen after that
("his hands are holding nothing"): the game does not draw the open leaf from his posed hand, so it stayed above his head
while the hands followed the controllers. Gliding, it is now placed from that hand with the offset the game's pose holds
it at (`place_held`'s glide branch), as an item moved to the other hand is. Its handle is its joint nearest the hand in the game's pose (joint 1 of the
open leaf, item 0x102, 40 units from its root), placed at the tracked hand. It is one piece with the hand as the sword
is (`lock_to_hand`, the blade's slot): placed only at the step, it came loose from the re-aimed hand between steps and
back ("moving in and out of Link's hands"). It is held by both hands: measured on his left hand always (the nearer hand
switched as the hands moved, and the leaf jumped between them), its handle at the midpoint of his two hands, and locked
to both (`lock_to_both_hands`: each pass from the two hands' midpoint, turned with the left).

**Things popping into view at each use (2026-11-03, reported: "like a shield popping in").** The fan swing
(`daPyProc_FAN_SWING_e`, 0x92) turns Link round past 100 degrees from the head, which the arms took for walking
backwards (`facesAway`): at every use both arms went to the game's animation and back, the hands and the shield on
them popping into view (`[vr] ... arm stops: Link faces away from the head` at each use in the log). The fan swing and
the glide no longer count as facing away.

## One piece: the hands, the sword and the shield (2026-11-03)

Reported with a comparison video ("hand and sword are not one piece, hand still disconnects from handle", "hand
disconnects from shield", "shield movement is not fluid but shakey"; another VR game: "hand is permanently connected to
sword handle", "1 to 1 and accurate to hand movement"). Three causes, three changes:
- **The sword hand's wrist was straightened** onto the forearm after being turned to the sword's grip
  (`vr.swordWristStraight`), while the sword itself was placed from the controller: the hand turned off the handle by
  as much. The sword hand is no longer straightened; it stays exactly on the grip (the elbow still lines the forearm
  up as far as the arm allows). `vr.swordWristStraight` now only straightens the empty off hand.
- **The shield was turned by its controller** beside a hand posed apart from it. Its place in the hand is now taken
  once, when the hand follows its controller fully, and kept (changed `vr.shieldOffset*` take it again): it turns with
  the hand, 1 to 1. `[vr] shield: one piece with the hand from here` marks it.
- **Both were placed once a step** and moved between steps by the hand's move since the step, so they drifted against
  the hand and snapped back at each step (the shake). Each drawn pass now places them from the hand as drawn
  (`g_locks`, `lock_to_hand`, `frame_correct_pose`), their root where it was in that hand at the step, as the picked-up
  stick is.

**Open when empty** (2026-11-04, asked for): `kFistsClosed` is off, so a hand with nothing in it is open (his body's
open hand, 0), a pressed grip closes it, and a hand holding the sword, the shield or an item keeps its grip.

**Fists closed (before):** in first person Link's hands were closed by default (`kFistsClosed`), round the sword and the shield
and empty alike; the grips no longer need pressing. A hand holding an item the game shapes itself (the bow while drawn)
keeps the game's shape.

Not yet tried in the headset.

**After the first headset test (2026-11-03):**
- **The shield's level back:** its place in the hand, taken once and kept, rolled with the wrist, so its point was no
  longer down (reported). It is turned by its controller again (levelled, point down) at each step
  (`kShieldPlaceKept` off), and each pass still placed from the hand as drawn (`g_locks`), so it does not shake.
- **The sword popping in with the Deku Leaf:** the sword's hilt and glow were hidden only with nothing in the hand;
  holding the leaf the sheathed sword's hilt was drawn, flickering as the leaf's models swapped at each use. They are
  now hidden unless the sword is in the hand.
- **The leaf less eager:** a flap now needs 2.8 m/s, 0.4 m and 1 s since the last.
- **Bombs further:** the hand's velocity times 3.2 (was 2), plus 14 units a step upwards for an arc, within 60 forward
  and 60 up a step (thrown at 2x they went 17-21 forward and nearly flat).
- **The shield vibrating (two rules fighting):** at each step it was turned by its controller (levelled, point down),
  and between steps placed from the posed hand, which rolls with the wrist: the two disagreed and it snapped between
  them every step (reported: "vibrating", "fighting for direction"). One rule now, `shield_by_controller`, at the step
  and each pass: turned by the live controller, its handle at the hand as drawn. Its other uses of the shield model
  (`measure_held`, the axes learnt from the game's pose) read the game's own matrices, not the drawn ones.
- **The bomb's "A Throw" prompt** is hidden while a bomb is in the hand in first person (`vr_ui.h`: `bombInHand`); the
  hand throws it.
- **The sword flickering while moving:** drawn before his body in a pass, the sword was placed from the last pass's
  hand, a pass behind; while Link moved that put it a few units off the hand, by a different amount as the draw order
  changed (reported: "flickers and moves around"). The last pass's hand is now carried on by its own last move, and the
  Master Sword's glow moves exactly with the blade each pass. `[vr] sword and shield: from the drawn hand N passes,
  from the last pass's carried on M` (every 10 s) shows how often each happens.
- **The glow synced to the blade:** it was moved from the blade's place at the step, but between steps the drawn glow
  is the blend of two steps' places, so while Link ran it landed off the blade by his move since and flickered
  (reported). It is now moved from where it is drawn that pass onto the blade's root as placed that pass.
- (diagnostics) `[vr] hands every frame: N hand-passes not re-aimed` counts passes where a controller was 40 units or
  more from its arm's blend (taken for a cut: the hand drawn as the animation has it), to tell whether that is behind the
  sword's flicker while running and turning.
- **Found: the sword's flicker while running and turning.** The headset showed 6 to 10 hand-passes every 10 s not
  re-aimed, the controller 68 to 70 units from the arm's blend: running while turning the view went past the 40-unit
  "cut" limit, and those passes drew the hand, and the sword in it, as the animation has it. The limit is now 200
  units (`kFrameHandCut`); real cuts are hundreds of units and more (281001 at the test area's load).
- **The sword flipping during full turns:** each pass the hand was turned from the drawn blend of two steps' hands.
  When Link's facing jumped in a step (a full turn of the body: 136 degrees in one step, measured), that blend swung
  the hand's turn through the turn while the controller held still, and the hand and the sword in it flipped about
  (reported). Each pass the hand is now the hand posed at the last step turned by the controller's turn since that
  step (`FrameHand::handNow`), whatever the blend does.
- **The shield hand locked to the handle** (asked for: as the sword hand is): at each step and each pass the hand
  holding the shield is turned to the shield's turn with the game's own grip on its handle (his right hand's
  attachment, mirrored onto that hand as the shield is: `shield_hand`), as the sword hand is turned to the sword's
  grip. The sword was confirmed steady in the headset after the full-turn fix.

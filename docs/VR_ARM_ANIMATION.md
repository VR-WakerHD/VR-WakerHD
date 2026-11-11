# VR arm animation

## Where the arms are posed

Link's body is one skinned model; HD runs `daPy_lk_c::jointCB1` (02403150) right after `mpCLModel->calc()` in his
execute (the game's own foot IK, writing joint matrices with `setAnmMtx`, which marks them changed so the skin is
rebuilt). The VR hook after it (`vr::game::link_arms`) poses both arms; everything later in his execute (hand
positions, the hands model, what he holds) and his draw use the result. The joints, checked at run time from bone
lengths (logged as `[vr] arm joints` / `[vr] clavicle joint`): clavicle 5 / 9, upper arm 6 / 10, forearm 7 / 11, hand
8 / 12 (measured: clavicle to upper arm 14.8, upper arm 17.0, forearm 18.3 game units). If they do not check out, the
arms keep the animation.

## Which arm is which

At each new Link the shoulders of both joint sets are measured against his own position and heading (`[vr] arm
sides` in the log): joints 6 are 11 to 12 units to his left, joints 10 about 15 to his right, so the left controller
drives 6/7/8. A build that took his position from the previous scene's record swapped them (the arms crossed); the
check now reads his current position and only changes the assignment on a clear answer.

## World scale (`vr.matchLinkBody`, on by default)

Link's arm is 35.3 units (shoulder to wrist). At the old fixed 100 units per metre your 0.62 m arm is 62 units, so
his hands were drawn at 57 % of your hands' distance, nearer your face than your real hands, and reaching out never
straightened his arm when yours did. With the world sized to Link the scale is his arm over yours (`vr.realArmLength`,
in the launcher under Controllers and in F1 > VR): 56.9 units per metre for the default, logged as `[vr] world scale`.
Your hands are then drawn where they are and reach as far as yours; the world looks as large as it does to Link
(about 1.75 times larger than at 100). The eye separation, head movement and the sword's position use the same scale.
Off: `vr.unitsPerMetre` as before.

## What the solver does (`vr_arm.h`, `pose_arm_natural`)

- **Target.** The wrist goes to the controller's offset from your eyes, scaled by Link's arm length over yours
  (`vr.realArmLength`), from Link's eyes (the camera, including your head's lean). A uniform scale about the eyes keeps
  the direction you see the controller in, so looking down at your hands shows Link's hands there (`eye_wrist_target`).
  Measured: controller 54 degrees below the eyes, Link's hand 52. (Anchored at the shoulders, as before, the hands
  left the view once the camera moved onto his head.) A target beyond his reach is pulled back along the line of sight
  onto his reach (`keep_in_view`); when that line passes beyond his reach (a controller far away at eye level) his arm
  reaches towards it from the shoulder instead, so his sleeve is not raised across the view. The two-bone solver keeps
  both bone lengths and clamps unreachable targets (no stretching, no straight-arm singularity).
- **Shoulder.** On extended reaches (beyond 60 % of the arm's length, fully at 120 %) the clavicle turns a quarter of
  the way towards the target, at most about 20 degrees: reaching high or far lifts the shoulder; close, relaxed
  movements leave it as animated.
- **Elbow.** The bend direction is the natural one for your body's facing (estimated from your head, so turning
  your body away from Link's heading turns it too): down, out to that side and a little back; it swings out to the
  side as the hand rises, further out when the hand crosses the body, and comes under the arm when reaching forward
  (`natural_pole_body`). The elbow is a hinge: the upper arm also turns about its own axis so the animation's hinge
  (the normal of its bend plane, kept in the upper arm's frame from the last clearly bent animation pose) lies along
  the new bend's normal, and the forearm turns about that hinge only. Before, each bone took the shortest turn from
  the animation's direction and kept its twist, so the forearm bent about another axis and the elbow skin twisted
  (the elbow looked bent sideways). Smoothed over 0.1 s, so the elbow does not flip or pop. The idle animation's elbow is no longer blended in:
  it swayed the arms with his breathing while the controllers were held still.
- **Wrist and hand.** The hand turns with the forearm, then so its pointing axis points where the controller points,
  but never more than about 80 degrees from the forearm; then it twists about that axis with the controller's roll
  (relative to holding it level), at most about 100 degrees either way.
  An empty hand's roll is the controller's roll from level and nothing else: its level "up" axis is learnt from his
  idle pose (`level_hand_up`, `ArmStyle::levelUp`). Added to the animation's roll, as before, it turned with his body
  and his run cycle, and the hands rolled over when he ran left or right (reported with a video). A hand holding an
  item still keeps the game's hold.
- **Tracking.** Each arm blends in and out over 0.15 s when its controller's tracking starts or stops; without a valid
  pose the arm keeps the animation (no jump to the origin). Turning, walking and recentring change only the target, so
  the arm follows smoothly.

Left-handed mode changes the sword hand; Link's left arm always follows the left controller and his right arm the
right one. The sword is drawn and swept at the sword-hand controller (`vr.trackedSword`).

The arms are posed once per logic step (30 Hz) and drawn with the game's frame interpolation; they lag the controllers
by about a step.

## The step's own position (the arms one step behind while moving)

`link_arms` runs inside Link's execute (his model's calc). `link_executed`, which records his position and heading in
`g_link`, runs after it. So `g_link.pos` there was still the step before, and the arms' targets (his eyes plus your
controllers), the body hold and the shoulder hold were all measured against where he had been. While he moved, the hands
trailed his body by one step's travel.

Measured with `WWHD_VR_TRACE_LAG=1`, which compares every frame the controller's target now with the drawn hand's (the
two steps' targets blended at the frame's `pass_t`):
- standing still: 0.1 to 0.4 cm on average, at most about 1 cm;
- running, before: 17 to 23 cm through whole running stretches;
- now: 0.75 / 0.95 cm on average over 52 two-second windows of running, swinging and turning.

`link_arms` now takes the step's own position and heading from the actor (`stepPos`, `stepHeading`). Nine of those 52
windows still show a single frame's spike (up to 70 cm, the same on both hands). It is not yet explained: possibly the
diagnostic's own head pose on a camera cut.

Hands at frame rate (TPVR's approach): TPVR (JoeyAW/TPVR, `src/dusk/vr/vr_link_visibility.hpp`,
`refreshFullBodyIkLive` / `ikSolveArm`) solves the arms once per rendered frame on the interpolated pose. Here they are
solved per logic step and interpolated. The measurement above shows the interpolation costs under 1 cm standing and
about 1 cm while swinging, so it was not changed. The interpolation's `viewCalc` hook (`interp.cpp`) is where a
per-frame solve would go; the separately drawn hands, sword, hilt and shield would have to move with it.

## Hands every frame

The arms are aimed once a step (`link_arms`) and in-between frames blend two steps' arms, so the drawn hands trailed
the controllers by about a step, and by a different amount each frame while frames came unevenly: the hands shook
while moving and turning, and swung back and forth while stick turning (reported in the headset; `WWHD_VR_TRACE_LAG`
measured 1.5 to 2.5 cm on average and up to 19 cm, 0.1 cm standing still).

Now, each pass:
- the camera override measures how far each controller's wrist target has moved (this pass's eyes plus the
  controller's offset, as `link_arms` aims) and how far the controller has turned, since the blend of the two steps the
  drawn arm is at (`g_frameHand`);
- when Link's body is drawn (`interp.cpp`, the viewCalc hook), each tracked arm as drawn is re-solved (`solve_arm`) from
  its own shoulder, bone lengths and elbow plane to its wrist moved by that much, and the hand turned by the controller's
  turn (`frame_correct`);
- his hands model and the models he holds (the sword, hilt, shield, the held item) move rigidly with the hand they are
  on.
- the Power Bracelets (two joints the game copies from his forearms each step) move with the forearms (reported in the
  headset: they shook on the re-aimed arms).

What the step's solve does (the natural elbow, the shoulders, the sword's straight wrist) stays: only the lag is taken
out. The game's own matrices are put back after each draw, so his logic, collision and attachments read the step.
Items that are their own actors (an arrow, the boomerang, a picked-up weapon) are not moved: they still follow the
step's hand.

## The sword hand's wrist (`vr.swordWristStraight`, 1)

With the sword in the hand, the hand is turned to the sword (the controller's blade, `held_sword_rotation`). Before,
the elbow was placed as for an empty hand, so the wrist bent up to hold the sword: 30 to 60 degrees on average,
measured. Now two things keep it straight (asked for in the headset):
- **The elbow.** It goes where the forearm best lines up with the hand. The pole is the straight-wrist elbow (the wrist
  less the forearm along the hand's line), taken square to the shoulder-to-wrist line.
- **The hand.** It is turned the rest of the way onto the forearm's line, keeping its twist, and the sword turns with
  it.

The "straight" line is each hand joint's forearm direction in the idle pose (`g_wristStraight`). Measured after: 0
degrees. Swing detection still reads the controller; the hit capsules follow the drawn blade. At 0 the hand turns to
the controller's sword as before.

## Detached Arms (`vr.detachedArms`, off)

**What holds the hands back** (with it off, as before):
- **The reach from Link's own shoulder.** The hand target is where you see the controller from his eyes, but
  `keep_in_view` pulls it back to 98 % of his arm's length from his animated shoulder, and the solver clamps it to
  that reach. His shoulders sit on his body, which faces his heading (the body follow, the game turning him), so a
  hand reaching across your body, far out, or to the side his body is turned from stops short of the controller.
  The input summary counts these frames as out of reach.
- **The wrist's limits:** the hand bends at most 80 degrees off the forearm and twists at most 100 degrees.
- **The clavicle** follows a reach by at most 20 degrees.

**With it on** (`vr_game.cpp`, the arm loop; `vr_arm.h` `detached_shift`, `detached_limit`):
- **The target is the controller,** with no pull back towards the shoulder.
- **Beyond his reach, the whole arm moves out from his shoulder** (upper arm, forearm and hand together), by what
  the reach lacks along the line from the shoulder, at most one arm's length. Every bone keeps its length, and the
  solver then poses it as always: elbow side, hinge, clavicle, smoothing. The skeleton, skinning and the game's
  attachments are unchanged. The shoulder's skin stretches with the gap.
- **The wrist and twist limits open to a half turn,** so the hand turns as the controller does.

**What stays the same:**
- Within reach, nothing changes: the shift is zero and the target is the same.
- The sword and the shield are on Link's hand joints, so they follow the hand where it now goes, with the same grips,
  sword wrist and swing detection (from the controller). The item wheel reads the controllers, as before.
- The body follow, head tracking, recentring, ladders and ledges are untouched. Third person and events play the
  game's arms, as before.

**Switching it:** the launcher ("Sword and arms": Detached Arms) or the in-game menu, at once, also while playing.
It eases in and out over 0.25 s (`g_detachWeight`), and it fades with the arm when tracking is lost, so nothing
snaps. A settings file from before it has no key, so it stays off.

**Not checked:** all of it in the headset (reaching overhead, out, across and behind; the sword and shield held far
out; the item wheel; climbing, ladders and ledges with it on), and how far the shoulder's skin stretch shows.

## Tests

`ctest -R vr`: `test_arm_pose` (reach, lengths, pointing, blending, invalid input) and `test_arm_natural` (shrug only
on extended reaches and within its limit, the shoulder stays on the clavicle, the natural elbow side per arm, the wrist
limit, twist about the pointing axis only and its clamp, controller roll). In the headset: hands and sleeves are drawn
in first person, both in view when looking down (captures on Outset, controllers at rest); how natural the motion
feels while moving them has to be judged by wearing the headset.

## The body turns with the view every frame (stick turning)

Turning the view with the stick (smooth or snap) turns Link only at the next logic step (his facing is the view's
plus the body's, `link_executing`), and every frame between draws his model between two steps' facings. The arms
were on the controllers every frame (above), so while the stick turned the view his body trailed it and twisted
against the arms (reported in the headset). Now `frame_correct` turns his whole skeleton about where he stands by
the stick's turn since the step it draws (`g_turnTotal`, summed where the stick turns the view, against
`turnPrev` / `turnCur` of the last two steps, blended by the frame's place between them), before the arms are aimed:
the shoulders turn with the view, the hands reach the controllers from there, and what he holds follows the hands
(or the body: `g_bodyTurn`, for what is on no hand). Only the stick's turns count: recentring, ladders and ledges turn
the view to Link instead. With no turn in progress the arms are aimed exactly as before.

## Hanging from a ledge: the game's arms (2026-11-09)

A player's video: hanging from a ledge in first person, Link could not shimmy left or right (third person could). The
game moves him along the ledge by how far the gap between his hands changes in the hang-move animation
(`procHangMove`: `mNormalSpeed` from `mLeftHandPos` and `mRightHandPos`). Those are read from hand joints 8 and 12
right after `jointCB1`, the hook where the arms are posed to the controllers (`link_arms`), so with the hands at the
controllers the gap hardly changed and his speed was about zero. While he hangs (`hang_proc`, actions 0x2B..0x31) the
arms now keep the game's pose, his hands on the edge; they follow the controllers again when he climbs up or lets go.
Logged: `[vr] left arm stops: Link hangs from a ledge`. Not yet tried in the headset.

## Empty hands held rigidly to the controllers (2026-11-10)

A player: "his hands still rotated when he's moving around in a circle". The roll from level (`levelUp`, above) is
measured against the horizontal, and a hand pointing down, as hands hang while walking, has no horizontal to measure
from: a small turn of the body was a large roll, and the hanging hands spun. An empty hand is now held rigidly to its
controller (`ArmStyle::rigid`): its orientation is the controller's (game world) times a fixed offset learnt from his
idle pose (`controller_to_hand`: the idle hand turned level and forward, against the controller held level and
forward), then turned back within the wrist's limit of the forearm. Nothing of Link's body, his animation or "level"
is in it. A held item keeps the game's hold, as before. Not yet tried in the headset.

**The left hand (same day, a player's video).** "The left hand still rotates when turning left or right or moving in a
circle", both hands empty. The off hand had one more step after the pose: turned onto the forearm's line (the straight
wrist, `vr.swordWristStraight`). The forearm's line follows the elbow, and the elbow follows the body's facing
(`natural_pole_body`), so every turn of the body twisted the left hand. A hand held rigidly to its controller skips that
step now; the wrist's limit still keeps it within 80 degrees of the forearm.

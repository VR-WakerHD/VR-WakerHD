# VR first-person camera and Link's head

## The camera (unchanged)

During normal play the drawn camera is the headset's pose in the game world (`vr_game.cpp`, `gameplay_head()` in
`vr_camera.h`, pinned by `test_gameplay_camera_regression`):

- base: Link's position (`fopAc_ac_c::current.pos`, blended between logic steps like his model), moved horizontally
  to his eyes: his head joint (15) from this step's animation plus 4 units forward along his heading; plus his eye
  height above his feet, from his animated eyes (`eyePos` +0x37C). Both are held still (`Settled` in `vr_math.h`)
  while the animation only sways them (dead zone 6 units for the height, 4 for the offset, the offset kept in Link's
  own frame so turning him turns it at once) and follow a real change of posture (crawling, swimming, climbing) over
  0.35 / 0.2 s, until they have caught up. The camera does not move without your head moving or Link moving;
- plus the headset's offset from the calibrated head position (recentre), turned into the game world and scaled
  (`vr.unitsPerMetre`), held within `vr.maxHeadOffset` of the base horizontally;
- the headset's orientation turned by the recentred heading.

The height was already right; the horizontal base was not: it was Link's feet, behind his head, so the camera sat
behind it and his tunic and shoulders filled the lower view (reported in the headset). Measured: the head joint is 2 to
3 units ahead of his feet standing; the eyes are now 5 to 6 units ahead (`[vr] first person: eyes ... ahead` in the
log). (Measured again later: the 4 units were far short of his eyes; see the next section.) The camera was not lowered to get the head out of the way; the head is hidden from the gameplay view instead
(below). The player's world position, the
recentre and the height calibration are not touched.

## The eyes in front of the head (the view was too close)

The view felt zoomed in, and looking down showed too little of Link's body. Checked, and not the cause:

- **Projection.** It is the headset's own field of view: both eyes use the union of their fields (123 x 106 degrees),
  and the compositor gets the same field.
- **Eye separation.** It uses the world scale (59 mm apart), like the head's movement.
- **Near plane.** 3 units (about 5 cm).
- **Character scale.** Untouched.

The cause was the camera's place in Link's head. Its horizontal base was the head joint plus a fixed 4 units forward.
Measured with `WWHD_VR_TRACE_SKELETON=1`, Link's head joint is at the back of his large head, near the neck, at 82
units. His eyes, by the game's own eye point (`eyePos` +0x37C, whose height the camera already used), are about 18
units in front of it. The camera therefore sat in the middle of his head, about 14 units behind his eyes. Looking
down, his shoulders and chest were straight below the view and too close to show the rest of him.

Now the forward distance is measured from the game's eye point while his head faces forward
(`vr_camera.h` `eye_forward_sample`: eye point 8 to 32 units ahead and within 6 sideways; he turns his head towards
things he notices, and then it is not his eyes' place in his body). The measurement is held like the other camera
values (`Settled`: 2-unit dead zone, 0.5 s). The camera now sits about 20 units in front of his feet, at his eyes. The
log shows `[vr] first person: eyes 20.2 units ahead of Link's feet (head joint 1.9 + 19.2 forward)`. Nothing else
changed: the head tracking, the recentre, the height, the projection and the scale are as before. Third person and
events use the game's camera as before.

The head hiding (below) also folded Link's waist and left thigh onto his neck: they are within 45 units of the head
joint and their indices (29 to 31) were inside the range it searched (up to 0x20). His hips were hidden and his left
leg was pulled up. The search now stops at joint 29, where the waist and legs begin; 15 joints are hidden instead of 18.

Confirmed in the headset (by the player): the view, Link's whole body and the sword look right.

## Link's model

Link is one skinned model (`daPy_lk_c::mpCLModel`, joints listed in `vr_game.cpp`) plus separate models attached to it:
the hair and bangs (`mpKatsuraModel` +0x44C, `mpYamuModel` +0x450, both on the head joint), the hands
(`mpHandsModel`), the held item (`mpEquipItemModel`). There is no separate first-person model.

The game already has a "camera very close" presentation: with camera attention status 0x20,
`daPy_lk_c::draw` hides face, hair and hat materials and keeps the sleeves and legs. In the HD model that alone left
the hair in view (the first headset tests showed yellow blades filling the top of the picture).

## Hiding the head (first-person gameplay only)

Around Link's draw (`daPy_Draw`, 023DBBE4), during normal play (`aspect::gameplay_camera()`, the game's own state:
not an event, cutscene, title, file select or ending):

1. the close-camera status 0x20 is set for his draw (restored after it);
2. his head is collapsed onto his neck: the body model's head joint (15) and the hat, hair and face joints after it
   that are near the head (up to the legs at 0x20, within 45 units of the head joint), and every joint of the hair and
   bangs models, get a matrix that maps everything onto the neck joint (14). Vertices skinned only to them disappear
   into the neck opening; vertices shared with the neck stay between the two (no stretching).
3. the neck joint (14) itself is collapsed onto its base too: once the camera sat on top of the body, its skin was a
   skin-coloured patch at the bottom of the view when looking down. The collar, shared with the chest, only shrinks
   towards the base; the arms (joints 5 to 12) come before the neck and are not affected.

The collapse is not undone after the draw: the game's `update_ubo` job thread copies joint matrices for the GPU while
the main thread draws on, and restoring them at the end of Link's draw raced with that copy. Link's next execute
recomputes every matrix from the animation (`J3DModel::calc`), so the skeleton and the animation keep running and
everything that reads them during logic is unaffected.

Outside normal play nothing is collapsed: in conversations, cutscenes, the title, file select, deaths and scene
changes the head is drawn from the very first frame, with its facial animation (the next execute has already
recomputed it). There is no separate visibility flag that could stay set.

Limits: the head's shadow and reflection are hidden too during first-person play (they use the same matrices);
`vr.showBody=1` draws the whole model in first person.

## Tested

In the headset (PS VR2, scripted entry into gameplay with `WWHD_AT_START=last`): the view from Link's eyes on Outset
is clear of hair and head; with the camera on the head joint his body is below the view instead of in front of it;
his hand and sleeve are drawn, his shadow is cast.

Stillness: while Link stands still the log reports every 10 s how far the camera's base moved
(`[vr] first person: camera base moved ... standing still`). Measured on Outset with the idle animation: his animated
eye height breathes between 91.4 and 93.7 units; the camera holds at 91.72 (0.00 in most 10 s windows; up to about
1.5 when the animation changed posture and the camera caught up). `WWHD_VR_TRACE_EYES=1` logs both values each step.

**Held while standing (2026-10-27).** The 6-unit dead zone was not enough: in a headset session the camera's height
moved 1 to 7 units a step in 16 of 42 ten-second windows of Link standing still (without lifting or carrying).
Drawing and sheathing the sword, taking out items, conducting, swings and the end of an event moved his animated eyes
20 units and more for a moment, past the dead zone, and the camera followed them up and down (asked for in the
headset: the camera bobbed on its own). Now, with his feet still (under 0.5 units a step) and his action not a
posture the view goes with, the height is held and his animations are not followed. The postures, followed as
before: crouching and crawling (actions 0x0C to 0x12), hanging, swimming, ladders and climbing (0x2B to 0x3F), also
after he leaves one until the view has caught up (getting up). Moving, everything is as before. A change of more than
15 units that lasts a second while standing (an action not named here) is followed too, with the log line
`[vr] first person: eye height follows a lasting change of ... (action 0x..)`. The 10-second stillness line now also
says how far his animations moved his eyes from the held height and in which action (`not followed`).

Only the VR view's height changes (and what is measured from it: the arms' reach, the swing's mapping without a posed
hand); nothing is written to the game. Checked on the desktop: standing, the height held exactly (0.00) while his
breathing moved his eyes up to 1.4 units. Still to check in the headset: drawing and sheathing, items, conducting
and swings while standing (the view level), crouching and getting up (the view follows), ledges and ladders.

## The body steady against the eyes while moving

The camera's eye height and forward offset are held through the stride (above). The running animation, though, lifts
and drops Link's whole skeleton with every stride: his head rose and fell 8 to 17 units a second while running
(`WWHD_VR_TRACE_HAND=1`, `[vr] body trace`). Under a still camera his body, arms and hands bobbed and shook while
he moved (reported in the headset). Nothing else wrote his bones twice. His position is the game's alone, the head
collapse only maps the head's joints onto the neck, and frame interpolation blends him between steps as one model.

Now, right after his model's calc (`daPy_lk_c::jointCB1`, `vr_game.cpp` `link_arms`), before the arm IK and before
the game attaches the sword, hilt and shield to his hands, his whole skeleton is moved so that his head joint stays at
a settled place in his own frame (position and heading):

- **Holds through the stride.** A 9-unit dead zone covers the bob.
- **Follows real posture changes.** Crouching, climbing, swimming, the lean of a run start: it follows them over
  0.3 s.
- **Capped, not skipped.** One step's correction is at most 12 units. A larger one is scaled down, not dropped, so
  the body never jumps between corrected and not.

Only where his bones are drawn changes, never his position, collision, animation or logic. It applies in first
person in normal play; third person, events and cutscenes show the game's own body. Measured while running: the
corrections were the bob's size (6 to 9 units), and the shoulders' remaining bob fell to 3 to 8 units, which the
shoulder hold takes (docs/VR_HELD_ITEMS.md). How it feels while moving needs the headset.

## Steady between steps (random shaking)

Reported in the headset: the first-person view shook now and then.

**Where the view's base comes from** (`vr_game.cpp` `anchor`, every frame): Link's position, plus his eye point. The
position is blended between the last two logic steps at the frame's fraction (`pass_t`), as his model is. The eye point
is the eye offset, about 18 units ahead of his feet along his facing (`eyeOffset`), and the eye height (`eyeHeight`).
Both were set once a step and drawn unblended. So:
- **In every turn while he moves,** the eye point rotated with his facing in 30 Hz steps while the frames came at 60 to
  90 fps: the view moved in small jumps.
- **When the game turns him at once with his feet still,** the eye point swings round his centre to the other side. That
  happens when he grabs or picks up an object, talks, opens a chest or locks on. A session logged
  `camera base moved 37.08 units at most in 10 s of Link standing still` during `GRAB_UP` (action 0x6F): about 37 cm in
  one step, twice the eye distance, which is a half turn.

**Fixed:**
- The eye offset and height are blended between steps like his position (`prevEyeOffset`, `prevEyeHeight`). It adds
  no latency beyond the position's own blend. A teleport of the eye point is not blended across.
- A facing change of more than 20 degrees in one step with his feet still holds the eye point where it was, then
  glides to his eyes (about half a second). This is the hold and glide already used when his body turns to follow the
  head. `[vr] first person: Link turned N degrees at once (action 0x..)` logs it (the first 20).

**Unchanged:**
- The head pose: the runtime's pose for the display time, applied once; no smoothing or dead zone added.
- Frame pacing. At 60 fps on a 90 Hz headset the frames still alternate 1 and 2 refreshes (about 300 a 10 s window in
  the logs). That is a separate effect, smoothed by the 45 fps mode (docs/VR_FRAME_RATE.md).

Not yet tried in the headset.

## The body under the head (room scale)

**Before:** the view was Link's eyes plus your head's offset from the recentred position, held within `vr.maxHeadOffset`
(0.6 m) horizontally. Link's own position was never moved by your head. When you leaned or walked in your play space,
the view moved away from his body, which stayed behind or to the side, and at 0.6 m the view stopped.

**Now** (`vr.roomScale`, on by default; `room_scale_move` and `g_room` in `vr_game.cpp`), before Link's logic each step:
1. Your head's horizontal movement since the last step is turned into the game world (the recentred heading plus stick
   turns, scaled like the view) and added to Link's position. His own collision takes it like any move of his, so he
   does not pass through walls.
2. The tracking origin moves by the same amount, in tracking space. The view is Link's eyes plus (head − origin): his
   move and the origin's move cancel, so the view stays exactly at your head. Nothing is applied twice.
3. The origin is blended between steps with the same fraction as his position, so the view and his drawn body move
   together, and his body stays under the view.

**Only in sensible conditions:**
- first-person normal play, with the view calibrated and the head tracked;
- Link standing or walking on the ground (actions 4, 5 and 6).

It never applies while he crawls or crouches, climbs, hangs, swims, is on a ladder or the boat, is locked on, attacks,
uses an item, is in the air, or in third person, events, cutscenes and menus. There the view is held within
`vr.maxHeadOffset` as before. Afterwards he catches up at no more than 8 cm a step (2.4 m/s; logged as "Link catches
up"). Against a wall the origin still takes the whole move, so the view stays at his body rather than going into the
wall.

**What is unchanged and does not conflict:**
- Vertical head movement (crouching, standing up) moves only the view.
- Head orientation, stick turning, the left stick's head-relative direction, the body following the head's yaw, the
  hands (placed relative to the head), the recentre (which resets all this) and the lean limit. Of these, only the lean
  limit reads the head's position against the origin, and it now sees only what was not taken.
- The checks for "his feet are still" (the eye height hold, the eye point hold during turns, the stillness log) leave
  out the room-scale move, so leaning does not count as walking.
- The eye point held during a body turn is moved with him.

**Turning with the head (in constant alignment).** Reported in the headset: in a full turn of the head the body lagged and
only lined up once the head stopped. That had two causes:
- **The body's facing:** it only began to follow once the head was 30 degrees (`vr.bodyFollowDegrees`) away from it,
  then eased round.
- **The view's eye point:** it was held through the turn and glided to his eyes afterwards. Turning him about his feet
  swung his eyes, 18 units in front, round with the turn.

Now, while he stands in first-person play (seated or standing, `vr.roomScale` on or off):
- The body faces the head at once, every step, with no slack.
- `room_scale_move` turns him about his eyes: his position moves by what the turn swings his eye point, so his eyes stay
  under the view. The view and the tracking origin are not moved by it, and his eye point is not held.
- A turn of the game's own on top of that (picking something up, talking) is still held and glided as before.

`vr.roomScale` only decides whether walking in the play space moves him; with it off the head's turns still turn his
body this way (seated play). The old slack and easing remain only outside first-person play.

Not yet tried in the headset.

## Camera audit (2026-10-30)

The whole first-person pipeline was audited: its ownership, update order, threads, OpenXR timing and stereo
(docs/VR_CAMERA_AUDIT.md). Five fixes were made:

- the ladder, ledge and lock-on view turns now run per frame (they ran at 30 Hz);
- Link's body turns between steps about his eye point, the view's pivot, instead of his feet;
- the lean-back limit uses his drawn facing;
- the smooth turn's interval comes from the views' own display time;
- the eyes are submitted at the offsets they were drawn with.

The head pose is unchanged. Not yet tried in the headset.

## Hiding the body (2026-10-30)

**Hide Link's body** (`vr.hideBody`, on by default since 2026-11-04) is in the VR launcher (Camera tab) and in F1 > VR, among its first
options. With it on, first-person normal play draws only his arms, his hands and what is in them.

**How** (`vr_game.cpp`: `frame_correct`, `hides_from_view`). Each pass, his arms are posed to the controllers, and the
hand moves that place his hands model and what he holds are taken from them (`frame_correct_pose`). After that, still
in the same pass, every drawn joint matrix of his body except the arms' (5 to 12) is set to map its vertices onto his
neck's base:
- **Joints 5 to 12** are the clavicles, upper arms, forearms and the body's own hands. An empty hand is a mesh of his
  body, so these stay as they are.
- **The neck's base** is the point the head is already collapsed onto (`hide_head`).

So his torso, legs, head and clothes draw nothing, and the skin shared between his shoulders and chest shrinks towards
the neck rather than stretching across. The shield and the sword's blade, hilt and glow are collapsed too while they
are on his back. The Power Bracelets stay, on his forearms.

Only the matrices drawn that pass change, never his animation's: the next pass starts again from the game's. So third
person, events, the telescope and the setting turned off draw him as before from the next frame.

**History.**
- **First try (15:38): switching his body's shapes off after his draw** (the visible byte, `J3DShape` +0x04). It did
  not hide him: his draw sets that visibility itself and enters his model in the same call.
- **Second try (15:44): collapsing every joint, the arms' included.** It hid too much (reported in the headset): the
  arms, and the empty hands, which are his body's own meshes.

**Limits.**
- His shadow and reflection lose the body too (the same matrices).
- The shoulders' skin may show a short stretch towards the neck. To check in the headset.

## Hiding the arms (2026-10-31)

**Hide Link's arms** (`vr.hideArms`, on by default since 2026-11-04; F1 > VR and the in-headset menu under Hide Link's body, and the VR
launcher): in first person his arms are not drawn, his hands and what they hold are. With Hide Link's body as well,
only the hands are left.

How (`vr_game.cpp` `hide_arms`, called from `frame_correct` before the body's hiding): each pass, after the arms were
posed and the hands' moves taken from them, each arm's clavicle, upper arm and forearm (joints 5 to 7 and 9 to 11) are
drawn as a point at that side's hand (joint 8 or 12): the arm's skin shrinks into the wrist, the hand keeps its place.
Only the drawn matrices change, as for the body; third person, events, the telescope and the setting off draw him as
before. Skin the game weights between the shoulder and the chest may show as a thin sliver towards the hand with the
body drawn (a vertex half on the torso, half on the upper arm lands between the two). Not yet tried in the headset.

**With the body hidden too (fixed, 2026-10-31):** a thin green thread hung from each hand (reported in the headset). The arms were drawn onto the hands and the body onto the neck's base, so the tunic's skin weighted between a shoulder and the chest stretched from each hand to the neck. Now, when Hide Link's body applies as well, the arms' clavicle, upper arm and forearm go onto the neck's base with the body. All the shared skin then shrinks into that one point, which is inside his hidden head. His hands are shapes of their own, held by the hand joints alone, so nothing stretches from them. The log says which point the arms go to.

## One owner of the head (2026-10-30)

The first-person head's state and maths are now in one place, `HeadController` in `runtime/src/vr/vr_head.h`. It owns:
- the mapping;
- the room-scale origin;
- the recentre;
- the turns;
- the head pose;
- the game-camera mode.

`vr_game.cpp` reads it through `const` aliases and changes it only through its methods. See
docs/VR_HEAD_CONTROLLER.md. The behaviour is unchanged.

## Above the water while swimming (2026-11-03)

A Steam Frame video showed the first-person view at the water's surface while swimming: half the view was a flat sheet
of water, and the eyes dipped under it into black, because the game draws nothing below the sea. Now, while Link is in
an action swimming is among (0x2B..0x3F) and the water's surface (`mWaterY`, Link + 0x6A28) is above his feet by at most
150 units, the lowest the view may be is 10 units (about 10 cm) over the surface (`above_water`, `vr_camera.h`; used in
`head_in_game` with the floor limit, `hold_above_floor`). Only the VR view moves; nothing of the game's. `vr_test` checks
the rule. Not yet tried in a headset.

## A constant height (2026-11-04)

The view's height is Link's standing eye height, taken the first time he stands still after a load (`[vr] first
person: the view's height held at ...` in the log), for every action but the postures that put his body elsewhere:
crawling (0x0D..0x12) and hanging, swimming, ladders and climbing (0x2B..0x3F), which it follows and eases back from.
Asked for after a play test: with the shield up, locked on, his battle stance lowered his eyes as he moved and the
view went down with them, as if crouching; standing, lasting changes of 52, -21 and 26 units had been followed.

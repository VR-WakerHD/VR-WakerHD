# VR aiming: the bow, hookshot, boomerang and grappling hook

## How the game aims them (decomp: `d_camera_subject.cpp`, `d_a_player_main_*.cpp`)

- **The camera aims, not Link's hand.** Each of these items aims from the first-person subject camera.
  `dCamera_c::CalcSubjectAngle` (02506964) gives that camera's pitch and yaw from the right stick (and the GamePad's
  gyro):
  - the yaw is Link's (work `m3BA`) plus `yawMax` × the yaw input (`m384`);
  - the pitch is `pitchMax` × the pitch input (`m388`);
  - both inputs are held in −1..1, so the item's own limits hold.
- **The game's sight** (`daPy_sightPacket_c` at Link +0x58E8) is where that camera's aim lands:
  - shown (+4);
  - locked (+5): the game takes what it aims at, such as a hookshot target or a grappling point;
  - its point (+8).
  Projectiles and the checks on them follow from the camera's aim and the sight.
- **The procedures while aiming**, measured on the desktop (all in `mods::camera_mouse::aiming_item`):

  | item | procedure while aiming |
  |---|---|
  | hookshot | 0x83 |
  | Hero's Bow | 0x94 |
  | boomerang | 0x80 |
  | grappling hook | 0x76 (0x7E after it is thrown, not aiming) |

- **Measured conventions** (`WWHD_VR_TRACE_AIM=1`):
  - the yaw output is the view's heading (game convention, 0 along +Z), and Link's body turns to follow it;
  - the pitch output is the negative of the view's elevation: the stick's −1 input gave −12743, a view 70° up.

## In the sword hand

All four are already drawn in the sword hand by the held-item code (`docs/VR_HELD_ITEMS.md`):
- what is in `mpEquipItemModel` is measured in the game's hand once per step (`measure_held`);
- it is moved rigidly to the dominant hand's posed joint on every draw pass (`link_draw_begin`), placed at an absolute
  target, so every pass agrees;
- that is the fix for the sword's and the telescope's earlier flicker: one writer, one mapping, one measurement per
  step.

Nothing new writes these models, so the four items use the same attachment, hand choice (`dominant_side`,
`vr.leftHanded`) and lifecycle as the sword.

## Now: aiming by pointing (`mods/vr_hooks.cpp`, `vr_game.cpp` `subject_angle`)

- **The hook.** `hook_02506964` runs the game's `CalcSubjectAngle`, then replaces its pitch and yaw while:
  - VR runs, in first person (third person aims with the sticks, as on the GamePad);
  - Link is in an aiming procedure;
  - the sword hand's controller is tracked.
  Not tracked, the game's own aim stays.
- **The direction** is the sword hand's controller aim pose, forward (−Z), turned into the game world with the
  mapping the arms use (`to_game_dir(g_map, ...)`).
- **The angles:** `vr_camera.h` `pointing_aim` turns it into the game's angles. The pitch is held within the item's
  own limit, read back from the game's own result over its pitch input.
- **The game's aim state follows.** The pitch input is set to match and the yaw input to 0, so leaving the aim or
  losing tracking carries on from where the controller pointed, without a snap.
- **Everything after is the game's:** the sight, its lock, the projectile (arrow, hook, boomerang, grapple), range,
  collision, ammunition and target checks. The camera's view only aims; the headset still draws what the head sees.

## The red laser (`vr_laser.h`, `vr_laser.cpp`, `vulkan/vr_vulkan.cpp`, `openxr_session.cpp`)

- **Two OpenXR quad layers:**
  - a beam 4 mm wide from the item to its end point, turned about its own line to face the head;
  - a dot at the end point, sized by its distance so it stays the same size in view.
- **Their picture** is one small image (96×32, drawn once): the beam's strip, a bright red dot with a white core,
  and a dim red dot. It sits in a swapchain of its own (`kLaser`) and is blended by its alpha, as the wrist HUD's
  panels are. No render pass of the game's draws it.
- **Its start** is the sword hand's aim pose, moved forward by the item's muzzle offset:

  | item | muzzle offset |
  |---|---|
  | hookshot | 14 cm |
  | bow | 4 cm |
  | boomerang | 8 cm |
  | grappling hook | 6 cm |

- **Its end** is the game's sight point, when the game shows its sight:
  - the dot is bright when the sight is locked (the game takes the target) and dim otherwise;
  - with no sight shown, the beam runs 15 m along the controller and the dot is dim.
  - Since the camera aims from Link's eye along the controller's direction, the beam converges a little from the hand
    onto the point the game will actually hit.
- **It shows** only in VR first person, while one of the four aims, with the sword hand tracked and no GamePad menu
  open. The log says `[vr] aiming laser shown` / `hidden`. It is updated after each pad read on the game thread, and
  the render thread draws whatever was set last.
- **`vr.aimLaser`** (on by default) turns it off. It is in the launcher ("Sword and arms": Aiming laser) and in
  F1 > VR.

## Used again while held (2026-10-31)

After one use, Link holds the grappling hook without aiming. Its trigger only became the hook's button while he aimed, so it could not be fired again (reported in the headset). The boomerang had the same problem after its catch.

Now the trigger presses the hook's own item button while Link holds it standing or walking (actions 4 to 7), as it does for the held boomerang and Deku Leaf (`vr_game.cpp`, the aimed items' trigger block). That lasts until he puts it away or takes out another item. While he hangs on or swings from the rope, the trigger stays as before.

## Lock-on with the grappling hook (2026-10-31)

The left trigger fires the grappling hook, and it is also where lock-on (ZL) normally is, so the hook could not be used with lock-on. While the hook is in hand (aimed, or held standing, walking or locked on: actions 4 to 9), the physical right trigger presses ZL. Lock-on works as the game's own, and the left trigger still fires the hook at what is locked on. In left-handed play that trigger is the off hand's binding, so it is still the physical right trigger. Asked for in the headset; not yet tried there.

## Turning while holding the hookshot or grappling hook (2026-10-31)

While either one is out, Link is in an aiming action, and the right stick was passed to the game's aim. So he could not turn (reported in the headset). His tracked hand aims these anyway (`subject_angle` overrides the stick's aim). Now in first person, with that hand tracked, the right stick turns the view and Link with it, snap or smooth, as it does without an item, and the pointing aim follows the turn. The bow and the boomerang are unchanged.

## The arrow along the bow (2026-11-03)

The game keeps the arrow on Link's left hand (`daArrow_c::setKeepMatrix`, 0205244C): that hand holds the bow there, and
the arrow rests at it (the model's origin is the nock, +Z the way it flies: a stuck arrow's model sits 50 units behind
where it hit). In VR the bow is in the right hand and the left
draws, so the arrow stood out of the drawing hand at that hand's angle, neither flush nor parallel with the bow
(reported with a video). Now, each drawn frame (`vr_game.cpp` `frame_correct_pose`, with the hands as drawn that pass):
- the arrow lies along the line from the drawing hand through the bow hand, its nock at the drawing hand, so it slides
  through the bow as the hand draws back (`kArrowLength`, an estimate of 60 units);
- its head stays at least `kHeadPastBow` (5 units) past the bow hand when the hands are further apart than the arrow;
- its roll follows the bow hand;
- the aim, the shot and the laser follow the same line (`g_bowLine`, read by `bow_aim`).
The string is drawn as soon as the arrow is on (vr_bow.h), as before.

## Tested

- `vr_test`: `test_pointing_aim` (the measured conventions, the pitch limit, bad directions) and `test_laser` (the
  beam's pose, length and facing, looking along it, the dot's facing and size, degenerate cases).
- Desktop, not in VR (`WWHD_VR_TEST_KEY` puts each item on X, X aims, X again uses it):
  - all four aim in the procedures above;
  - the hookshot fires and the grappling hook is thrown;
  - nothing changes outside VR (the override runs only in VR).
- The whole suite passes.

## Still to check in the headset

- The items in the sword hand, steady while pointing, turning and walking (the held-item code's, unchanged).
- Aiming where the controller points, for each item:
  - the arrow, the hook, the boomerang and the grappling hook landing at the dot;
  - Link's body turning with a sideways aim.
- **The laser:** steady in both eyes, the right length, the dot bright on a valid target.
  - The beam is placed from the controller's pose at the last pad read; the item is drawn from the posed hand. So a
    fast swing may show the beam a little ahead of the drawn item.
- Left-handed play, tracking loss while aiming, and the grappling hook after its throw (0x7E: the game's aim, no
  laser).

## The hookshot's tip in the hand (2026-11-03)

Link's draw sets the hookshot's tip (the barrel's end, joint 4 of the held model) to the hookshot actor's top matrix
(`daPy_lk_c::draw`: `setAnmMtx(HOOKSHOT_JNT_HSHOTTIP, hookshot->getMtxTop())`), after VR moved the held model to the
tracked hand. That matrix comes from the game's own hand, where the aiming animation holds it in front of the view, so
the barrel stayed low in the view while the hand moved (reported with a video: the hookshot arm stuck in place). Now
(`vr_game.cpp` `hookshot_tip_begin` / `hookshot_tip_end`, around Link's draw): before the draw the tip's place against
the model's root is taken as the game calculated them, and whether the actor's top is in the barrel (within 50 units of
the tip's rest); after the draw a resting tip is put back rigid with the hookshot in the hand. A fired hook keeps the
game's tip (the flying hook). `[vr] hookshot:` logs which, with the distance.

## Taking the string with the grip (2026-11-03)

As another VR game does (asked for with a comparison video): the arrow hand's grip squeezed at the bow (within
`kNockMetres`) takes the string and puts the arrow on; letting go of it once drawn back (the hands `kFireDrawMetres`,
0.32 m, apart) shoots. Let go before that, the arrow is dropped (asked for in the headset): it falls out of the
hand. The game shoots it as any arrow; from the next step, for at most 1.5 s and only while it is still that arrow, its
speed is set (an arrow in flight only moves by its speed): 1.5 units a step along the bow and falling from rest, 2.5
units a step faster each step (`arrow_fall_step`). Shot straight down at the game's speed instead, it went into the
ground like a shot (reported). Either trigger
still shoots as before (`vr_bow.h`, `vr_test`).

## Moving while aiming (2026-11-03, being looked into)

HD's first-person aims move Link with the left stick (`procBowSubject`: `setSpeedAndAngleAtn`, `lk_aimSpeedBoost`),
and VR passes the left stick on while aiming, yet he did not move in the headset (comparison video). `[vr] aiming and
moving:` logs, once a second while aiming with the stick pushed, what the game is given and his speed, to find where it
is lost.
At 20 units the resting hook read 21.9 on alternate draws and was handed back to the game's hand every other draw
(reported: "disconnected from the player's hand"); fired, it is 100 units and more out at once.

## Turning with the bow out (2026-11-03)

The right stick turns the view and Link while the bow is out in first person, as it does with the hookshot and the
grappling hook (`aimByHand`): the bow aims along its arrow, between the hands, not with the stick (asked for).

## One dot, not two (2026-11-03)

With the red laser on (`vr.aimLaser`) the game's own aiming sight is not drawn (`sight_scale` 0): the laser's dot is at
the sight's own point and bright on a valid target, and the two dots showed side by side with the hookshot (reported).
With the laser off the sight is drawn at 1/16 as before.

## An easier draw, and the string following the hand (2026-11-03)

Asked for in the headset ("easier to shoot ... the string follow the player's hand"):
- **Shot after a shorter pull:** letting go shoots once the hand has pulled `kMinPullMetres` (8 cm) back from where it
  took the string, or with the hands `kFireDrawMetres` (0.25 m, was 0.32) apart; less, the arrow is dropped
  (`vr_bow.h`, `vr_test`).
- **The string at the hand:** the bow model's joint the string bends at is found once, in the game's own drawn pose,
  as the joint nearest his drawing hand (`measure_held`, within 25 units; `[vr] bow: its string is joint N of M`
  logs it). Each pass while the arrow is on, that joint is put at the drawing hand as drawn (`frame_correct_pose`),
  so the string comes back with the hand instead of the game's fixed full draw. Not yet tried in the headset: the
  joint chosen is the log's to confirm.
- **After the first test:** the string joint was not found (no log line), so the string still went from relaxed to
  the game's full draw at once. The search now looks within 40 units and logs the bow's joints against both hands of
  the drawn pose once (or why it did not look), to find it. The draw is measured from the controllers themselves:
  Link's shorter, reach-clamped arms made a pull look short and dropped arrows that were pulled.
- **The search never ran:** it waited for the drawn pose's hands of the same step, which are taken after the bow is
  measured, so they were never fresh there (`[vr] bow: its string not looked for yet ... not taken this step`). It now
  uses the hands as last taken, a step old.
- **Both halves of the string:** found in the headset, the Hero's Bow has 12 joints and its string two, its top and
  bottom halves (6 and 11, each about 27 units from the drawing hand in the drawn pose). With only the top moved the
  bottom stayed straight (reported); both are now moved to the hand (a second joint within 3 units of the nearest's
  distance is the other half).

## Aiming and moving with the bow: the arms no longer lock to the animation (2026-11-04)

Reported with a video: moving around with the bow out, after a couple of shots the shots stopped, Link would not move and
the aim no longer followed the hands ("frozen and I can't aim"); the video shows the game's own bow pose, the arms not at
the controllers. The arms take the animation while Link faces more than 100 degrees from the head (the rule for walking
backwards), and while aiming the game turns him to the aim, which for the bow is the line between his hands
(`bow_aim`). Turned away, the arms took the animation, the aim came from the animation's hands and kept him turned: a
lock until the bow was put away.
- While aiming an item (`aiming_item`: the bow standing or moving, the hookshot, boomerang, grappling hook) the arms
  always follow the controllers, as with the Deku Leaf.
- The line between the hands pointing back from the bow (the drawing hand past the bow hand) is not the aim: the bow
  hand's own pointing is used instead, so the aim never turns him round to face behind you.
Not yet tried in the headset.

## Moving while aiming: VR moves Link (2026-11-04)

Reported with videos (the bow and the hookshot): with an item aimed, Link stayed where he was. The game's own aims walk
him slowly or not at all (`[vr] aiming and moving:` measured the grappling hook's aim, action 0x76, at 0.00 a step with
the stick pushed, and another aim at 3.2, against about 17 running). Now, while aiming any item in first person
(`aiming_item`), VR moves him the stick's way from where the head looks, by the same slide as walking backwards
(`back_slide`): at a full push his running speed (`mMaxNormalSpeed`), walls checked from his eyes, the game given no
left stick (its aim keeps him facing where the hand aims). `[vr] moving while aiming:` logs when it starts and ends.
Not yet tried in the headset.

## The bow's turn in the hand (2026-11-04)

Reported with a video (bow test 1): the bow "twisting around into the wrong position at certain times when you move or
aim": upright in some frames, flat or sideways in others. In the hand the game holds it in (his left), the bow was left
on the game's own attachment, which is that of the action playing: upright while aiming (0x94), flat or sideways while
walking with it or standing. With the hand on the controller, the bow turned in it as the actions changed.
Now its place in the hand (turn and position, `measure_held`) is taken from the aiming pose once that has settled
(5 steps in 0x94; logged once, `[vr] bow: its place in the hand taken from the aiming pose`) and used in every action
(`g_bowHold`, `place_held`): the bow is placed from the tracked hand with it. Until the first aim it is the game's as
before. Game thread only. Not yet tried in the headset.

## The bow's string on the boat (2026-11-06)

The string's joints are found by the bow model's shape: its two halves meet at one place (the Hero's Bow: joints 6
and 11 of 12), `vr_bow.h` `bow_string_pair`. The joint nearest the drawing hand in the game's pose was taken before;
on the boat, after turning, the game's pose held that hand 20 to 60 units from every joint, the limbs (1 to 3) were
taken, and drawing pulled the whole bow ("the entire bow model looks like it gets pulled"). The nearest joint is still
the fallback for a model without such a pair. The log says `[vr] bow: its string is joint 11 and 6 of 12 (its two
halves at one place; ...)`.

## The game's aiming sight in the UI layer (2026-11-06)

The game's own sight (`daPy_sightPacket_c`: the bow's, the hookshot's, the grappling hook's and the boomerang's aim
mark) is a 2D mark: its setSight projects the aim point to the screen with the game's camera and puts it on the 2D
draw list. In the eye pictures each eye drew it at the same pixel of a different field, so it showed twice (a double
red dot) in third person, on land and on the boat. It is now drawn once into the UI layer's image, as the boomerang's
lock-on sights already were (`mods/vr_hooks.cpp` hook_02414834, hook_020CDBFC). First person with the laser still
draws it at size 0 (the laser's dot marks the point).

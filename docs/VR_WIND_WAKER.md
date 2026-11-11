# The Wind Waker in VR

How you choose the Wind Waker, and how you conduct with it, with the sticks or with your hands.

## Choosing it: the item wheel

The Wind Waker is on the item wheel's Items page whenever the save has it ([VR_ITEM_WHEEL.md](VR_ITEM_WHEEL.md)).
Choose it and bring your hand forward: it goes on X (`vr.itemWheelButton`), and Link raises it at once. That is the
same press the game's own item menu leaves you to make.

**Its icon** is the game's own picture of the baton. The button icons (`BtnItemIcon_00`) have none: the game's
item → icon table (hdMenuItem `itemForSlot`) has no entry for it, and all 55 of its frames belong to other items.
So the picture comes from the Quest screen's icons instead.
- It is `CollectIcon118_07` in `BtnCollectIcon_00`, 118 x 118, read from your own 2D pack like the others and
  centred on a 128 x 128 icon (`item_icons::wind_waker_icon`).
- It is kept after the game's frames (`item_icons::kWindWakerIcon`), and `vr_game.cpp` `wheel_icon_frames` maps
  item 0x22 to it.
- The wheel and the wrist HUD's item buttons both read that mapping, so the Wind Waker shows on either.
- It is the same item number the save and the game's buttons hold, so the icon, the wheel's choice and what Link
  raises cannot disagree.

**In third person** the left controller's grip takes it out: it goes on the same button and Link raises it
(docs/VR_CONTROLS.md, "Third-person view").

## Conducting

The game says when Link conducts: its metronome flag (play +0x5BD1). `daPy_lk_c::procTactWait_init` sets it, and
the song's end, a cancel or a talk clears it. The GamePad's display reads the same flag. While it is on, the game
reads its two sticks as on the GamePad, by where each is held at each beat, not by presses:

| Game input | Stick | Hand (`vr.batonHands`) |
|---|---|---|
| The note: up, down, left, right | left stick | the wand's hand (the sword hand: right, or left when left-handed) held up, down, left or right |
| The C-stick: left 3/4, right 6/4, neutral 4/4; up and down move Link's left arm | right stick up, down, left, right | the other hand held up, down, left or right |

**The notes come from the hand holding the wand** (2026-11-09). A player's video and log: "most of the time it doesn't
register the wand direction". They conducted with the wand, in the right hand, and the right hand was the beat: 70 beat
directions logged against 20 notes. The notes were the left hand's. Now the sword hand (`dominant_side`, the right
unless `vr.leftHanded`) plays the notes and the other hand the beat (`BatonInput::noteHand`). The notes' zone is also
smaller, `Baton::kNoteReachShare` of `vr.batonHandReach` on every side. The beat's stays the full reach.

**Still too far (same day, the player's own test):** "still too hard to hit the right note you need to stretch your hand
out too far". The notes' hand is now read at the wand's tip, 20 cm ahead of the grip along the controller's aim
(`vr_game.cpp` `kBatonTip`), so a flick of the wrist moves it as well as the arm: about 9 degrees of wrist turns it
3 cm. And the zone is half the reach (`kNoteReachShare` 0.5, 0.65 before). Across the body is now as near as outward
(`kNoteShape` 0.5, 0.5, 0.7: across had been half as far again, and in first person the right note, across the body
for the left hand, was the one too far): at the default 12 cm the notes left and right are 3 cm out, up and down
4.2. Not yet tried in the headset.

Everything else is the game's own: the beat timing, the notes, the songs and whether a song succeeded. The VR side
only decides which way each stick is held, and gives the game a full push that way.

While conducting:
- the left stick does not move Link, and the right stick does not turn the view or crouch;
- the item wheel cannot open, and the sword is not drawn from over the shoulder (both are hand gestures).

Swings never press B while conducting, because a swing presses B only with a sword in the hand. The arm IK, the
shield and the held baton are drawn as before, and the hands' poses are only read, never changed.

### The view

Conducting keeps the view you play in. First person stays first person: the view at Link's eyes, his head hidden, his
arms following the controllers. His body, and his head with it, faces where your hands are held: forward, from
your head to the middle of the two hands' places when conducting began (`Baton::hands_centre`), with no more than
5 degrees of slack. Their settled places, not where they are now, because the beat moves the right hand left and
right. Until the hands settle it is their current places. The game holds his facing while he conducts, and with his
arms on the controllers they had reached across his body when you faced another way (`vr_game.cpp`
`body_follows_head`; checked on the desktop: a facing written while he conducts stays). Third person stays third person: the game's own Wind Waker camera, in front of Link, turned by the head. The head's
forward is taken again as conducting begins and ends, so looking ahead shows the game's view of it.

The game shows conducting with a camera of its own and marks it as an event (play +0x5292 is 3). VR counts it as normal
play only while the metronome is on and Link's action is his own conducting: TactWait 0x9A or TactPlay 0x9B
(`aspect::gameplay_or_conducting`, `vr_game.cpp` `play_view`). When conducting stops (B), the game clears the metronome
one step before that mark, so the mark alone counts for 8 more frames. Without that, the view went to the game's camera
for one frame and first person was anchored again after it. Cutscenes, a song being taught and a song's effect keep
the game's camera, as before. The sword, the item wheel, aiming and movement keep the strict normal-play test.

**The bar of notes and beat** (BatonInput_00: the beat `N_InputPoint_00` at y 321, the notes `N_BatonSign_00` at 245)
is at the top of the TV picture. In the headset that is the edge of what the lenses show, and the notes to play and
the ones played could not be seen. While conducting in VR it is drawn about the middle of the view instead: the notes
just above the centre, the beat just below (`aspect.cpp` `calculate_baton_bar`). The message window's own
`N_BatonSign_00` (a song's notes when it is taught, at y -26) is not moved.

The right side's up and down reach the game (the log shows `beat up` / `beat down`), but in the game they only move
Link's left arm (`getTactPlayLeftArmAnm`), not the song. In first person the arms follow the controllers, so that pose
is not seen.

### The wind compass

After the Wind's Requiem the game shows its compass (ControlWind_00) in the lower right of the TV picture, at
446,-169. In the headset that corner is at the edge of the lenses, and outside normal play the layouts are drawn at a
third of their size too, so the compass was not seen, in first person or third. In VR it is drawn in the middle of
the view instead, at 0.6 of its TV size whatever size the layouts are drawn at (`aspect.cpp`
`calculate_wind_compass`). Its OK button (ControlWindOK_00) moves with it; the darkened backdrop stays full screen.

The right stick points it, as the left stick does: the right stick's direction goes to the game as the left stick's,
and the view does not turn meanwhile (`vr_game.cpp`, `aspect::wind_compass_shown`). A stays the button that takes
the direction, B goes back. The log says `[vr] wind compass: shown ...` and `closed`.

Not yet checked in the headset.

### The GamePad screen

While conducting, the GamePad screen's panel (the game's song list) is hidden. The menu button shows or hides it, and
it is hidden again each time conducting begins. While conducting, the menu button does only this: the game does not
get its Plus. B still stops conducting, as in the game. The GamePad's menus show the panel as before
(`gfx::display_vr_gamepad_panel_open`, `vr_game.cpp` `filter_pad`; with `vr.gamepadScreen = always` it stays on).

### The sticks

A stick gives a direction once it is pushed past half its throw (`vr.stickDeadZone` + 0.2, whichever is larger).
- It holds that direction until it is back within 0.35 of centre (or the dead zone + 0.05). So drift gives nothing,
  and a stick at the edge does not flicker.
- A diagonal goes to the larger axis. An exact diagonal goes to left or right.
- A held direction turns to the other axis only when that axis is a quarter larger. So rolling along a diagonal does
  not alternate between two directions.

### The hands

Each hand gives a direction by where it is held from where it settled when conducting began:
- its centre is taken once the hand has been still (slower than 0.15 m/s) for 0.3 s, and until then it gives
  nothing. Conducting begins as your hand comes forward from the item wheel; a centre taken at that moment left the
  hand reading "down" once it came to rest (seen in the headset's log);
- a circle of `vr.batonHandReach` metres round that centre (0.12 m by default) is a neutral zone: inside it the
  hand gives nothing;
- outside it, the quadrant the hand is in gives the direction: up, down, left or right, with the diagonals as the
  borders between them. So a hand held out any way gives one as soon as it leaves the circle, however its reach splits
  between the two axes. Before, one axis alone had to reach the distance, and down and to the right gave nothing
  (reported in the headset);
- the left hand's zone has its own reach on each side (`Baton::kNoteShape`; asked for in the headset: first its left,
  then its right, too hard to reach). Its left note is 0.06 m out (half the radius), its right 0.09 m (0.75), up
  and down 0.104 m (0.87). The quadrants are taken in that stretched space, so a nearer side also has a wider
  quadrant. The right hand's zone (the beat) stays a circle of the full radius;
- it holds the direction until the hand is back within 80 % of the radius (0.096 m);
- it changes to the next quadrant only once it is clearly past the diagonal, about 51 degrees from the held direction;
- moving it forward or back does nothing.

So a hand held up holds the note up, as a stick held up does, and lowering it again is neutral. Directions are
measured in a frame turned to the head's heading at that moment, so looking round while you conduct changes nothing.
Diagonals resolve as for the sticks.

A hand settles again, giving no direction until it has:
- when it is tracked again after a loss (nothing comes from it meanwhile);
- after a recentre (both stick clicks);
- after a jump of more than 0.25 m between two reads of its pose (a tracking glitch);
- when the next song begins.

**The right side has all four directions**, for the right stick and the right hand. Left and right set the beat; the
game reads up and down too (Link's left arm, `getTactPlayLeftArmAnm`: WAITTAKTLHANDU / D). They had been dropped (an
up or down held had kept a left or right from turning it), and were asked for back in the headset: the right stick's
and the right hand's up and down did not register.

### Both at once

Each side is one held direction, so a stick and a hand cannot play a note twice: the game reads one stick value per
beat. The policy:
- a stick pushed out of its dead zone wins over its own hand;
- a stick at neutral leaves the hand's direction, and a neutral hand leaves the stick's, so neither cancels the
  other;
- the two sides are independent: the left hand never sets the beat, and the right never plays a note.

## Settings

| Key | Default | Range | |
|---|---|---|---|
| `vr.batonHands` | on | | the hands also conduct (off: the sticks only) |
| `vr.batonHandReach` | 0.12 | 0.05 .. 0.4 m | the neutral zone's radius: a hand out of it gives the direction of its quadrant (held until within 80 % of it) |

Both are in the launcher (Controls, Wind Waker) and the in-game menu.

## The code

- **The rules:** `runtime/src/vr/vr_baton.h`. It is pure: `BatonFilter` (one source's direction), `BatonHand` (a
  hand's centre and frame) and `Baton` (both channels and the policy between stick and hand).
- **Where it is used:** `vr_game.cpp`'s pad read (`conducting()`, `g_baton`). The log says `[vr] Wind Waker:
  conducting ...` and `conducting ended`. While conducting, it also logs each change of note or beat, with what the
  stick and the hand each gave.
- **The icon:** `mods/item_icons.cpp` (`centred_icon`, `wind_waker_icon`, `game_icons`) and `vr_game.cpp`
  `wheel_icon_frames`.

Without VR nothing changes: none of this runs without a VR session.

## Tests

- **`vr_test` `test_baton`:**
  - each stick's four directions;
  - drift, release and the hysteresis;
  - diagonals;
  - a hand settling only once it is still, then its four directions, jitter, holding and returning, the same
    direction again;
  - the beat's hand and stick left or right only;
  - a turned head, tracking lost and regained, a pose jump, a recentre;
  - the stick against the hand, either one neutral, the two sides independent, the hands switched off;
  - a new song re-centring;
  - the settings.
- **`item_icons_test`:**
  - centring a 118 x 118 picture, and reading the Wind Waker's from an archive;
  - with `WWHD_TEST_2D_PACK` set to your own pack, the Wind Waker's icon is read (`permanent_2d_UsEnglish.pack`:
    read).

**In the headset (2026-10-25):** the icon loaded; choosing it on the wheel put it on X and Link raised it;
conducting began; each stick and each hand gave its directions, and a pushed stick won over its hand and gave way
back to it. The left hand read "down" at rest and the right hand gave up and down on the beat, hence the two
tunings above.

**In the headset (2026-10-27):** first person stayed first person while conducting, and the bar of notes and beat
in the middle of the view was confirmed.

**Not yet checked in the headset:**
- the two tunings;
- whether 0.12 m is a comfortable reach;
- a song played to the end.

### The hands' roles swapped, their calibrations kept (2026-11-09)

Asked for in the headset: "the left hand and right hand needs their roles reversed, right hand should be the one hitting
the notes ... but keep the calibrations the same". The wand's hand (the sword hand, the right unless `vr.leftHanded`)
now gives the right stick's channel and the other hand the left stick's (`BatonInput::swapHands`), each hand keeping how
it is read: the wand's hand at the wand's tip with the wand's zone (3 cm left and right, 4.2 up and down at the default
12 cm), the other hand with the full reach. The thumbsticks are unchanged. The log's start line says which hand goes with
which stick. Not yet tried in the headset.

### Calibrated in the headset (2026-11-10)

A run with each hand's reach logged (`[vr] Wind Waker reach (cm ...)`, once a second, and each direction with the hand's
offset). The wand's moves were 10 to 50 cm each way; directions registered from 2.5 to 5 cm (jitter counted); up and
right on a diagonal came out right (left and right were weighted more than up and down); a sweep from left to down
stayed left; and "I can't hit the neutral note": the hand rarely came back within 80 % of the 3 to 4 cm zone of where it
had first settled. Now:
- the wand's zone even on every side (`kNoteShape` 1, 1, 1) and three quarters of the reach (`kNoteReachShare` 0.75):
  at the default 12 cm a direction from 9 cm, the middle back within 7.2 cm;
- the middle follows the resting hand: while no direction is held its centre eases towards where the hand is
  (`BatonHand::kFollow`, 1.5 s), so a hand that drifts still comes back to the middle;
- a clear move to the other axis turns sooner (`BatonZone::kTurn` 1.15, about 49 degrees; 1.25 before).
Not yet tried in the headset.

**Again the same day:** "holding your arm in the middle triggers the left". With the arm still (1.3 cm in a second) a
left came at 9 to 10.5 cm: a relaxed turn of the wrist swung the point read 20 cm out along the controller sideways. It
is read 8 cm out now (`kBatonTip` 0.08: 25 degrees of wrist moves it 3.4 cm, inside the middle), and the middle follows
the hand only while it is still (slower than 0.15 m/s). Not yet tried in the headset.

**The middle, third run (2026-11-10):** "still too hard for middle note". The middle registered back within about 7 cm,
at its edge. Now:
- the middle is wider: a direction held until the hand is back within 90 % of its zone (`BatonZone::kExitShare`, 0.8
  before), and the wand's zone is 0.85 of the reach (`kNoteReachShare`): at the default 12 cm a direction from
  10.2 cm, the middle back within 9.2;
- **the wand hand's trigger holds the middle** (`BatonInput::middle`): while it is pressed that hand gives no
  direction, wherever it is; the trigger presses nothing while conducting. Logged: `[vr] Wind Waker: the middle held by
  the trigger` / `let go`.
Not yet tried in the headset.

**The middle stuck far away (2026-11-10, a player's log, "Wind Waker Test 4").** "I got it to work once. Then as I keep
trying to repeat the song, it gets worse and worse until the directions aren't even registering." The middle had been
taken with the wand raised; the hand then rested 18-30 cm below it, so "down" was held for good (the reach lines held
at 18 cm down for seconds), and the middle's ease towards the resting hand runs only with no direction held. Now a
direction held with the hand still for `BatonHand::kRestTime` (1.5 s, past a held note at a slow tempo) takes that place as the middle. Logged: `[vr] Wind
Waker: the right hand rested in a direction: its middle taken there`. Not yet tried in the headset.

**Flicks (2026-11-10).** Asked for: "if player swing out it's right, swing in towards their body it's left, and up and
down swings are up and down". The wand's hand also reads its motion (`BatonHand` flicks): moving faster than 1 m/s
(`kFlickSpeed`, across and up in its frame, smoothed over 30 ms) gives the direction it moves in at once, before the hand has
gone the zone's 10 cm, and holds it while the hand stays out (the game reads the note on the beat), until the stroke
back begins or the hand is back within 4 cm of the middle (after 0.15 s at least, `kFlickHold`); the zone carries on
from it. The stroke back to rest (a
move towards the centre from more than 4 cm out, `kFlickReturn`) gives nothing, so a flick up and back is one up, not
up then down. One flick a stroke: the next once the hand has slowed below 0.5 m/s (`kFlickRearm`). The beat hand reads
no flicks. The log's direction lines now name the game's sticks (`right stick (notes)`, `left stick (beat)`: they had
read "beat" for the notes) and say `a flick` when one gave the direction. Not yet tried in the headset.

**A stroke's intent (2026-11-10).** Asked for: "use flick direction and final position to understand players
intentions ... if the hand is travelling to the right position but doesn't make it there but was almost there,
register it as a right note; if the hand is still around the middle, the middle". A move out from the middle faster
than 0.25 m/s is a stroke (`kStrokeSpeed`); once it has gone half the zone's distance from where it began
(`kIntentShare`: 5 cm at the wand's 10.2), heading clearly one way (one axis more than 1.5 times the other), it gives
that way, held as a flick is, though the hand stops short of the zone. A fast flick still goes by where it is moving,
a slower stroke by where it has gone. A hand still or drifting near the middle, a slow diagonal, and the stroke back
give no note. Not yet tried in the headset.

**Direction first, then where it ends up (2026-11-10).** Asked for: "make sure direction is considered first then
consider final location and decide what the players intentions were". A flick's or stroke's direction is held while
the hand stays out, and where the hand is (the zone) no longer overrides it: a stroke to the right from a hand held
down plays right, though its place is still more down than right. It ends when the stroke back begins or the hand is
back near the middle (within 4 cm), or when a new stroke fires another way. While a stroke is under way but has not
gone far enough to decide, the zone does not switch to a different direction than the stroke's. With no stroke (the
hand held still or moved slowly), where the hand is decides, as before. Not yet tried in the headset.

**Long arm motions (2026-11-10, Wind Waker Test 5).** Reported with a video: small pointer moves of the wand worked, but
whole-arm moves (the arm raised all the way, down, out and across, as Link conducts) "barely register", and the middle
note did not work; then "we need to register longer motions". Causes: the zone was sized for 10 cm moves, so an arm
brought back from 50-70 cm out landed 10-20 cm from the middle and read as a direction; a pose held 1.5 s took the far
end as the middle (as did a note held two beats), so later moves read wrong; and a whole-arm raise was decided by its
first flick, often sideways. Now:
- the wand's zone grows with the moves being made: 0.35 of the farthest the hand has gone lately (fading over 4 s),
  from the set zone (10.2 cm) up to 2.2 times it (22 cm). Small moves keep the small zone;
- a long stroke (1.5 times the zone from where it began, heading clearly one way) gives where it has gone overall,
  over a flick's first direction;
- the middle is taken where a hand rests in a direction only after 4 s (was 1.5);
- a jump of the pose that settles the hand again: 50 cm between reads (was 25).
The log's reach line gives the notes' zone as it is. Not yet tried in the headset.

**Big and small together (2026-11-10).** Asked for: "both big and small should register, big if player is swinging
their arms or small if they are flicking their wrists". The zone grown by big moves is used only for where the hand is
and for long strokes; a wrist flick or short stroke is measured against the set zone, whatever the grown one: a quick
flick (over 1 m/s) of any size, or a clear 5 cm stroke, plays its note straight after whole-arm moves, and a big swing
straight after flicks plays too. The grown zone fades over 8 s (was 4), so a pause between big moves keeps it. Not yet
tried in the headset.

**Big moves hold their note on the way back (2026-11-10, headset run).** The log showed every whole-arm move register
on the way out, then turn to the middle at its far end (25-47 cm out) the moment the arm began to come back: the rule
that drops a flick's note on the stroke back (so a small flick's return does not linger) cut it while the arm was
still far out. Now the stroke back drops the note only once the hand is back inside the zone; out past it the note
stays. The grown zone is 0.4 of the recent reach (was 0.35): 35-50 cm moves had given 12-18 cm zones, while the arm
rested 12-19 cm out. Guarding was tried in the same run: the left grip in third person and the shield raised with
its grip in first person each gave the crouching guard at once (no enemy attacked, so no block was seen).

**8 cm strokes (2026-11-10, asked for in the headset).** "8cm of continuous arm movement in any direction will register
that direction", for a wrist's flick and a whole arm's move alike (the run before: guarding worked, but whole-arm
moves turned to the middle at their far end and a move down read as the middle). The wand's notes now come from its
strokes only (`BatonHand`, `kStrokeLen` 8 cm): a stroke is the hand moving (faster than 0.15 m/s) until it stops
(under 0.08 m/s) or turns more than 60 degrees (back, or round a corner: out, then up); once it has gone 8 cm from
where it began, its way (the larger of across and up) is the note, held until another stroke (0.3 s at least, so the
game's beat finds a quick one). From a held note, a stroke straight back the way it came is the hand coming back: the
middle once it stops or is within the middle's zone, the opposite note if it goes on past that zone (up, through the
middle, to down). A stroke back and to one side gives that side (up, then over to the left: left). The middle's zone
is the set 10.2 cm, grown up to 16 cm by big moves (0.3 of the recent reach). A slow drift gives nothing; the trigger
still holds the middle; a note held still 4 s still takes the middle there. Not yet tried in the headset.

**Slower moves (2026-11-10).** Asked for: "how fast does the player need to move to hit notes? can we make it slower?"
A stroke had to begin faster than 0.15 m/s and keep above 0.08 m/s (one slow read ended it): 8 cm in about half a
second at the slowest. Now it begins above 0.05 m/s (8 cm in 1.6 s at the slowest) and ends only after 0.15 s under
0.03 m/s, so a brief slowing mid-move does not start it again; a turn is judged only while moving above 0.05 m/s.
The middle no longer follows the hand during a stroke (a slow move out had the middle ease after it, so it never got
8 cm away). A creep under 0.05 m/s still gives nothing. Not yet tried in the headset.

**Confirmed in the headset (2026-11-10, build 5a25a25).** The 8 cm strokes with slower moves allowed: "Yes, works
now". The run logged up, down, left and right from 8-15 cm moves and one 35 cm sweep, the middle after each note,
and the trigger's middle. Guarding was confirmed the same day (the left grip in third person, the shield raised with
its grip in first person).

**The wand points forward (2026-11-10).** Reported: "sometimes the windwaker turn around and points at link instead of
pointing forward ... in 1st person view can only point forward". Right-handed, the wand is drawn in the other hand
than the game's, its place in the hand taken from the game's animation each step and mirrored over, and some
conducting animations turn it round in Link's hand. Now its length axis (its own axis most along the controller's aim,
chosen once while held) pointing back against the controller's aim turns it half round about the hand, so it always
points forward. Logged: `[vr] Wind Waker: its length is its axis N`, `turned back to point forward`. Not yet tried in
the headset.

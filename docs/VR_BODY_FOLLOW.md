# VR: Link's body follows your head

## What it does

In first-person play, while Link stands, he turns to face where you look once your head has turned more than
`vr.bodyFollowDegrees` from his body (30 degrees by default).
- He keeps turning until he is within 5 degrees of your view.
- Most of the turn takes about a third of a second, then it eases in.
- He turns the short way round, yaw only: looking up, down or tilting your head never turns him.

**Turning with the right stick** (snap or smooth) turns him with the view, once.

**When the game keeps control:** while it turns him itself, nothing here touches his facing, and following resumes
when that ends. That covers:
- walking and running (towards the stick's direction, which is where you look);
- lock-on;
- attacks and items;
- swimming, climbing and hanging;
- events and cutscenes, menus, the telescope;
- third person.

## Why he did not turn before

Link faces where the game turns him, and the game turns him only while the stick is pushed: its movement turns him
towards `dCam_getControledAngleY`, which VR answers with your view's direction (`controlled_angle_hook`). Standing
still, turning your head or turning with the right stick rotated the view but nothing turned Link. You looked out
over a body facing the old way, and the arms, posed against his real heading, twisted.

Setting his facing from outside needed all four of its copies:
- his `current.angle.y` (+0x322: the way he moves);
- his `shape_angle.y` (+0x32A: the way his model faces);
- the d_a_player statics' kept copies of both, `1046CD12` and `1046CD0A`. The HD game restores those into him at
  every update (0240D130), so a write to the actor's angles alone was undone at his next step (seen in a test run).

`WWHD_LINK_WARP` and the editor's fly camera write the same four.

## How (`vr_ik.h`, `vr_game.cpp`)

There is one body yaw, `g_bodyYaw` (`BodyFollow g_body`, tracking space). It is the existing body follow, which
already placed your shoulders for the arms and judged swings. Its dead zone, which left the body up to 34 degrees
behind the head for good, is now a threshold with hysteresis:
- It starts following once the head is more than `vr.bodyFollowDegrees` from it.
- It then turns towards the head until within 5 degrees (`BodyFollow::kExit`), and stops there.
- The step each update is the angle left times `1 - exp(-6 dt)`, at least 1 rad/s. It is frame-rate independent:
  30, 90 and 144 Hz updates agree within a few degrees.
- It wraps the short way round.

**Where its inputs come from:**
- **Your view's direction:** `yaw_of(head orientation)` in the tracking space, horizontal only.
- **Link's facing in the game:** `link_facing(map heading, body yaw)` = the tracking space's turn into the game
  (`WorldMapping::heading`, which recentring and the right stick's snap and smooth turns change) plus the body's yaw.
  A turn changes only the first, so it is applied once and cannot feed back: nothing derives the mapping from
  Link's heading, except a recentre.

**Who decides:**
- **The game turns him** (first-person play and Link in any action but standing): the body's yaw is his heading
  back in the tracking space (`body_yaw_of`, `BodyFollow::set`). Your shoulders follow his body while he walks or
  is locked on, and following starts from where he actually faces.
- **He stands** (`daPyProc_WAIT_e` 4 and `daPyProc_FREE_WAIT_e` 5, `body_follow_proc`), in first-person play
  (`first_person_play`): the body's yaw follows the head (`filter_pad`). `link_executing` sets all four copies of
  his facing to it right before his logic. His animation, his model, the arm IK (posed against his heading), what he
  holds and the wrist displays all turn with him in the same step. Nothing is corrected afterwards.
- **Third person, menus, events:** the body's yaw follows the head for the shoulders and swings, as before; Link is
  not touched.

`WWHD_VR_TRACE_BODY=1` logs each start and end of a follow.

## Ladders: the view turns to Link

**The problem:** to climb down, the game turns Link round to face the ladder (procLadderDownStart_init:
`shape_angle.y - 0x8000`, his root turned with it), but the view's base, the tracking space's turn into the game,
is changed only by recentring and the right stick. So the view kept facing the drop: you looked out backwards from a
Link who faced the ladder. The body's yaw, synced to his heading while the game turns him, pointed your shoulders
backwards too.

**The priority:** on a ladder (`rig_follows_link_proc`: up start 0x38, up end 0x39, down start 0x3A, down end 0x3B,
move 0x3C), in first-person play, the view follows Link instead of the other way round:
- **The view's base turns** (`g_map.heading`, `turn_towards`: the short way, frame-rate independent, about 0.9 s for
  the half turn) until the body's facing maps onto Link's model facing (`rig_heading_for`, his `shape_angle.y`). It
  keeps it there for as long as he is on the ladder, following any change of his facing.
- **The body keeps the yaw it had when he stepped on.** Your head looks round freely, and its yaw never undoes the
  alignment.
- **The headset is untouched:** its position and rotation are tracked as always. Only the base the tracking space
  is mapped from turns, as a snap or smooth turn does.
- **The right stick does not turn on a ladder** (Link cannot turn there); it turns again once he is off.

**Leaving the ladder**, by its end, falling, an event or a load: the rule is no longer met, and the normal rules apply
from the next update. Standing at the bottom, the body follows the head again; walking, the game turns him. Nothing
is kept, so nothing is stale. The view's base stays where it is, with no snap back.

**Climbing up:** Link already faces the ladder as he walks into it, so the base hardly moves. If he squares up to the
ladder at an angle, the view turns the few degrees with him.

The log says `[vr] ladder: the view turns to face the way Link faces` and `... is free again`.

**Tests:** `vr_test` covers:
- the ladder actions;
- the short-way turn across +-180, with no overshoot, exactly on target at the end and frame-rate independent;
- a descent: walking up, the half turn, the view on his facing within a second, still while he climbs, exactly one
  half turn in all.

**Needs the headset:** the descent itself, how the turn feels, and leaving at the bottom, at the top and by falling.

## Ledges: the view turns to Link, and his frame is his model's

**The problem:** hanging from a ledge, two things went wrong.
- **The view's base did not follow Link.** Stepping off an edge, the game turns him round to face the wall (as on a
  ladder going down), but the view kept facing the drop.
- **His frame turned with the way he slid.** The view's eye offset (his eyes about 18 units ahead of his head joint),
  the first-person skeleton hold and the body's facing were all kept in his `current.angle.y`. Sliding along a
  ledge the game sets that to the way he slides (procHangMove_init: `shape_angle.y +- 0x4000`), while his model keeps
  facing the wall (`shape_angle.y`). At every slide's start and end his frame turned a quarter round: the view jumped
  sideways and eased back, and the body swung to face along the ledge.

**The fix** (`vr_ik.h` `hang_proc`, `rig_follows_link_proc`, `link_frame_angle`; `vr_game.cpp` `link_frame_heading`):
- **The ledge actions join the ladder's rule:** hang start 0x2B, hang fall start 0x2C (stepping off an edge), hang
  up 0x2D, hang wait 0x2E, hang move 0x2F (sliding), hang climb 0x30 and wall catch 0x31. The view's base turns to
  his model's facing as on a ladder, the body keeps its yaw, the right stick waits, and the headset is tracked as
  always (only the base turns).
- **On a ledge his frame is his model's facing**, never the way he slides: the eye offset, the skeleton hold, the
  body and a recentre all use it. Everywhere else his frame is `current.angle.y`, as before.
- **The position** is his own, blended like his model, as always: the view moves along the ledge with him.
- **Only yaw:** nothing of his hanging pose tilts or rolls the view.

**Leaving the ledge** (climbing up, letting go, falling 0x27): as for a ladder. The normal rules apply from the next
update, the base stays where it is, and the body's facing is his, so nothing jumps.

The log says `[vr] ledge: the view turns to face the way Link faces` and `[vr] ladder/ledge: the view is free
again`. `WWHD_VR_TRACE_LEDGE=1` logs his action at each change, and twice a second on a ladder or ledge his position,
facing, travel and frame, and (with a headset) the view's base, the body, the head's yaw and the yaw they make
together.

**Tests:** `vr_test` covers:
- the hang actions, and the actions next to them (not included);
- his frame on a ledge sliding either way;
- stepping off, then sliding left, right, back and stopping: the base turns once, then stays exactly put;
- falling off: the body's facing is still his, so nothing jumps.

**Not checked:** a ledge in the game (the scripted run was stopped before Link reached one), and everything in the
headset: sliding, turning the head while hanging, corners, climbing up, letting go and falling.

## The body follows the arms (with Detached Arms)

With Detached Arms (`vr.detachedArms`), the arms may leave Link's shoulders to reach the controllers. While he stands,
the yaw his body follows is then the head's turned towards where the hands reach (`vr_ik.h` `body_follow_yaw`). It
uses their midpoint from the head, flat: no turn within 0.15 m, a full turn from 0.40 m. His shoulders face the arms,
so a reach to the side turns him, rather than leaving the arm far from his body. The same 30 degree threshold and
hysteresis apply. Without Detached Arms it is the head's, as before.

**Moving:** the game turns Link to the movement's heading, which was the head's alone, so a look round or a reach
while he moved left his body facing away from the arms until he stood again. With Detached Arms that heading is the
same estimate (the head's, turned towards the hands), so his body stays with the arms in motion too.

## Walls (sidling): the view turns to Link's head

**The problem:** sidling along a narrow ledge with his back to the wall (wall-hide: ready 0x13, wait 0x14, move
0x15, peep 0x16), Link's model faces away from the wall while his head turns to look along it, and round its corner
when he peeps. Moving, the game sets `current.angle.y` to the way he slides (`shape_angle.y +- 0x4000`, as on a
ledge). None of these actions turned the view, and his frame (eyes, body) swung with the slide.

**The fix** (`vr_ik.h` `sidle_proc`, `rig_facing`, `link_frame_angle`; `vr_game.cpp`):
- **The four wall actions follow the ladder and ledge rule.** The view's base turns smoothly (`turn_towards`) until
  the body's facing is Link's, the body keeps its yaw, the right stick waits, and the headset is tracked as always
  (only the base turns).
- **His facing there is his animated head's:** the horizontal direction from his head joint to the game's eye point.
  The game places the eye point from his eye joint (`PSMTXMultVec(eye joint 19, l_eye_offset)`). Both are read in
  `link_executed`, after his animation, each step. Only its yaw is used, so the head's animated pitch and roll never
  tilt the view. Where it gives no clear direction (under 2 units flat), his model's facing is used.
- **His frame is his model's facing**, not the slide's, as on a ledge.

**Leaving the wall:** as for ladders and ledges. Normal rules apply from the next update, and the base stays where it
is.

**Tests:** `vr_test`:
- the four actions, and the actions next to them (not included);
- the head's yaw, with pitch ignored, and the fallback to the model's facing;
- the base settling on the head's facing as it looks left, right and left again.

**Checked in the headset (2026-10-30, 16:30, PS VR2):** sidling works: the view turns with Link's head along the
wall (the trace showed the wall actions 0x13, 0x14 and 0x15, and `wall (his head): the view turns ...`; the player: "siddle
is working"). **Not checked:** peeping round a corner,
stepping off.

## Tests

- **`vr_test`:**
  - Head turns of 10, 20 and 29 degrees, both ways: no turn.
  - 31, 45, 90 and 179 degrees, both ways: a smooth follow (not a snap) to within 5 degrees, on the head's side; no
    drift while the head is still; small head movements around the new facing do not start it again.
  - A 90 degree turn is mostly done in half a second and finished within one.
  - Across +-180 degrees: the short way.
  - 30 / 90 / 144 Hz updates agree.
  - A snap turn moves Link's facing by exactly its angle; the game's heading back gives the same body yaw.
  - Wait and free wait follow; moving, lock-on (7-9), swimming and item actions do not.
- **In the game, without a headset** (desktop, a copy of the player's last session on Outset): `WWHD_VR_TEST_BODY=step:degrees[:...]`
  turns a simulated head and runs the same follow and the same facing write. Head turns, with Link's heading:

  | Head | Link |
  |---|---|
  | +20 | stays at 23 |
  | +25 more (45 from him) | 23 to 64.6 in about 0.6 s |
  | +90 | to 153.4 |
  | -180 | to -17.1, the short way |
  | -45 | to -62.7 |

  Each time he stops within 5 degrees of the head and stays there. His action stays wait or free wait (he does not
  start walking), and frame dumps show his whole body turned, standing on the dock.

**Not checked without a headset:**
- the feel of the threshold and the speed;
- the camera at his eyes as he turns;
- the arms and the sword while he turns;
- snap and smooth turns in a real session.

The camera sits 18 units ahead of his head joint along his heading, so as he turns towards your view, the view moves
a few centimetres to stay in front of his face.

## The view while the body turns

Link's eyes are about 18 units in front of his centre, and the body-follow turns him about his centre: every body turn
swung the view's eye point round with it, so the view slid sideways (up to 37 units logged in one 10 s window). Now,
while the body is turning to follow the head with Link standing, the eye point is held where it was when the turn
began; after the turn it glides to his eyes over about half a second (`link_executed`, `g_link.eyeHold`). A jump of
more than 60 units (a teleport) takes his eyes at once.

# VR sword combat: audit and staged plan

Status (2026-11-20): this doc holds the audit (phase 1) and the plan. Stage 1 (tracking), stage 3c (the arc sweep) and a
first part of stages 3, 8 and 9 are built and pass the offline tests (`vr_test`). Nothing in this doc has been tested in the headset yet.

## 1. Audit: what exists today

### Files

| File | What it does for the sword |
|---|---|
| `runtime/src/vr/vr_swing.h` | `blade_from_grip` (the blade from the controller's grip), `SwingDetector` (swing phases from the hand's speed, swing kinds), `sweep` (the 3 capsules), `PhysicalSword` (arms the game's attack capsules while the hand swings), `HitLedger` (each target once per swing) |
| `runtime/src/vr/vr_sword.h` | the sword's state (sheathed, drawn), when a swing may hit (`hits_allowed`, `can_swing`) |
| `runtime/src/vr/vr_game.cpp` | `sync_input_locked`: pose sampling, swing detection, the game's attack presses (B, thrust, parry). `link_executed`: hit haptics, the physical sword, the capsules along the tracked blade, the game's attack parameters. `link_executing`: the physical sword's hits, then the capsules disarmed. `link_draw_begin` / `frame_correct_pose`: the drawn sword in Link's hand (`g_swordRot`, `g_grip`) |
| `runtime/src/vr/vr_ik.h`, `vr_arm.h` | Link's arms posed to the controllers (the drawn sword's hand) |
| `runtime/src/vr/openxr_session.*` | controller poses at the predicted display time, `haptic(hand, amplitude, seconds)` |
| `decomp/wwhd_src/d/actor/d_a_player_main_02.cpp` | `setNormalCutAtParam` 023E5B0C (power 1, normal sword), `setFinishCutAtParam` (power 2) |
| `decomp/wwhd_src/d/d_cc_s.cpp`, `d_cc_d.cpp` | the game's collision: sets the attack's hit position, the shield-hit flag |

### Tracking

- The controller poses are predicted for the frame's display time (`display_time()`), once per headset frame (90 Hz).
  The pad is read every 4 ms or more, and a read with the same pose time measures nothing (fixed 2026-11-04).
- `SwingDetector` measures only the hand's (blade root's) **linear** velocity from 2 poses. It has no angular velocity
  and no history: a wrist flick moves the tip fast but is not measured as such.
- There is one transform for the sword (the tracked pose). There is no separate "physical" sword pose.

### Collision

- Link's attack has exactly 3 capsules (`daPy_lk_c::mAtCps[3]`, at link + 0x7B1C, 0x138 apart). The game's collision
  (`cCcS`) runs **once per logic step (30 Hz)**.
- The VR code writes the tracked blade into those 3 capsules each logic step: blade now, the tip's path, root to the
  tip before (`sweep`). This is a swept test, but between two logic steps only: 3 headset frames of motion are
  joined by one straight chord. A fast curved swing can cut inside its arc.
- With the physical sword (`vr.physicalSword`), the capsules are armed with the game's normal cut
  (`setNormalCutAtParam`) and entered into `cCcS::Set` 0200E240 and the mass check 02516C14. Everything after that
  is the game's own: damage, enemy hit reactions, guard, deflection, hit marks, sounds.
- What the game records on a hit (confirmed in the decomp):
  - the hit object at cps + 0x08;
  - the hit position at cps + 0x70 (`mGObjAt.mHitPos`, `d_cc_s.cpp:394`);
  - the shield-hit flag, bit 1 at cps + 0x54 (`d_cc_s.cpp:403`).

### Damage

- The game's damage is its attack power, `mAtp` at cps + 0x14. It is 1 for a normal cut and 2 for a finishing cut
  (Master Sword: twice those), whatever the swing's speed.

### Enemy reactions

- These are the game's own animations, run by each enemy's own hit handler (dozens of actor files). No procedural
  layer exists.

### Haptics

- One pulse per target per swing, at a fixed `vr.hapticStrength` for 0.06 s.

### Recognition

- `SwingDetector` judges the swing kind once per swing: horizontal, vertical, thrust or rising. It uses distances and
  thresholds, not confidence scores. A "full" swing presses the game's attack. Parries are the game's own, on A with
  the parry prompt up.

### Limitations

1. No angular velocity, so wrist flicks and tip speed are not measured.
2. Impact speed is not known, so haptics and damage ignore how hard the swing was.
3. The collision is 30 Hz with 3 capsules: a chord between steps, no sub-step sweep.
4. There is no sword-on-sword contact (enemy weapons hit Link only through the game's own guard).
5. There is no resistance: the blade passes through enemies and walls.
6. Enemy reactions are animation only.
7. There is no visual debug of the blade or the sweep.

## 2. Architecture: new parts and how they fit

- **`vr_sword_motion.h` (new, stage 1).** `SwordMotion` is a ring of the last 32 sword poses with their times. Each
  pose gets:
  - its linear velocity;
  - its angular velocity, from `q1 * conj(q0)` taken the shortest way round;
  - glitch and gap rejection.

  `point_velocity` gives v + ω × r for any point down the blade. `peak_impact` gives the fastest a point moved
  over a time window. It only measures, and it is header only and tested offline.
- **Target and physical transforms.** The target is the tracked pose (`SwordMotion`). The physical pose is what
  the game sees (the capsules, Link's drawn hand). Today they are the same. Stage 4 makes the physical pose follow the
  target with tunable resistance only while in contact, so free swings keep zero added latency.
- **The game stays the physics.** No second physics engine. Hits, damage, guarding and reactions remain `cCcS` and the
  enemies' own code. The VR layer feeds them better input: where, how fast and how hard.

## 3. Stages

| Stage | What | State |
|---|---|---|
| 1 Tracking | `SwordMotion`: 90 Hz history, linear and angular velocity, v + ω × r, glitch and gap rejection; sampled in `sync_input_locked` at each new pose | **built, offline-tested** |
| 3a Impact | per hit: its place along the blade (the game's hit position), that point's peak speed over 0.1 s, its cut-or-stab share, deflected or not; a log line per hit | **built** |
| 3b Damage | `vr.swordStrongHitSpeed` (m/s, **0 = off**): a physical-sword swing this fast at 3/4 of the blade gets a finishing cut's power (twice the game's) | **built, off by default** |
| 8 Haptics | `vr.swordImpactHaptics` (on): the pulse from 35% to 100% of `vr.hapticStrength` by impact speed (full at 15 m/s; 6 before the first headset test), 0.04 to 0.10 s; a deflection a sharp 0.12 s pulse | **built** |
| 9 Debug | `vr.swordDebug` (off): once a second, samples, their spacing, the tip's peak speed and turn rate, glitches and gaps | **built** (log only; the visual overlay is later) |
| 3c Arc sweep | `path_between` / `sweep_path` (`vr_sword_motion.h`): the blades the swing passed through between two logic steps, from the headset's poses (two a step at 90 Hz), put into the game's frame. Capsule 0 stays the blade now; capsules 1 and 2 are the pair (tip path pieces, in-between blades, the old ones) that leaves the least of the swept area outside the capsules' radius (cps + 0x134). The old pair wins ties | **built, offline-tested** |
| 5 Recognition | continuous scores (horizontal, vertical, overhead, thrust, diagonal, parry) from the history, with no lock; logged beside `SwingDetector` first, then compared | planned |
| 4 Resistance | physical pose behind the target while in contact (a hit or a shield), stiffness and maximum lag as settings, so free swings keep zero lag | planned |
| 3d Sword on sword | enemy weapon capsules versus the blade sweep, deflection on contact | planned (needs the enemies' attack capsules found per actor) |
| 6 Reactions | a small hit offset on the hit enemy's root joint along the impact, blended out over about 0.2 s, on top of the game's animation | planned (per-actor joint access) |
| 7 Pipeline | no render-thread work, no new thread waits; measure the game thread's cost (the stage 1 work is a few vector operations per frame) | each stage |
| 10 Tests | `vr_test` for each stage's maths; a headset test at Capped 90 for the finished stages, reporting the average fps and the % of frames at 90 | each stage |

### Rules kept

- Nothing here runs on the render thread, and no new locks or waits are added (stage 1 runs inside
  `sync_input_locked` under the existing `g_mutex`).
- The game's logic rate stays 30 Hz.
- Third person and the game's own attacks are unchanged. The stage 1 measurement only feeds haptics, the log and the
  opt-in damage.

## 4. Test plan

- **Offline:** `vr_test` `test_sword_motion` covers:
  - the angular velocity of a known turn, including the shortest way round;
  - a wrist flick measured at the tip (8.5 m/s at 0.85 m for 10 rad/s) and not at the root;
  - a stab, all along the blade;
  - gaps, glitches and NaN poses rejected;
  - the peak window, the ring's oldest-out order and the blade fraction;
  - the power and haptic maps;
  - `sword_pose_from_grip` matching `blade_from_grip`.
- **Offline:** `test_sword_arc_sweep` covers:
  - the in-between blades of a 90-degree swing;
  - a small target inside the arc, missed by the old chord and hit along the arc;
  - 300 random swings, with turns, hand moves, Link's own step and 1 to 4 poses a step: the chosen capsules never
    leave more outside than the old ones, and the path meets both ends;
  - no poses between steps: exactly the old sweep;
  - the game's frame turned from the tracking space's.
- **Headset** (Capped 90, after asking), for the stage 1 + 3a + 8 build:
  1. Hit enemies with slow and hard swings and a wrist flick. The log's `[vr] sword hit:` lines should show low
     speeds for soft hits, high for hard ones, the place along the blade, and "deflected" on shields. The haptics
     should feel weaker and stronger to match.
  2. The `[vr] sword sweep (10 s)` lines: how many moving steps had poses in between, and how often part of the
     swept area was outside the capsules with the old chord and along the arc.
  3. With `vr.swordDebug=1`: `[vr] sword motion (1 s)` should show about 90 samples about 11.1 ms apart.
  4. Report the average fps and the % of frames at 90 against the saved base.

## 5. Open issues

- The hit's place along the blade uses the blade as written to the game in the last step. A hit position the game
  leaves stale would read as the root. Shown in the log; check it in the first headset test.
- `kHapticFullSpeed` (6 m/s) and `vr.swordStrongHitSpeed` need values from real swings: the first test's log gives
  them.
- The arc sweep's gain is only for very fast swings. In the offline measure (a 100-unit blade, capsule radius 20,
  3 headset poses a step), none of the swept area was outside the old capsules up to 45 degrees a step
  (1350 degrees/s). At 60 degrees a step, 4.6 units were outside with the old chord and 1.7 along the arc. At 90
  degrees a step, 24.9 and 18.3. A small target inside a 90-degree arc (radius 5, 40 units out) is missed by the old
  chord and hit along the arc. The headset log's `[vr] sword sweep (10 s)` line counts how often real swings get
  there.
- The arc sweep costs 3.4 microseconds a logic step with 2 poses between steps, and 20 at most with 4 (desktop, -O2).
- The in-between blades follow the controller's motion relative to the latest pose. Link's own movement over the
  step is shared out by time.
- Stages 4 to 6 are not built (resistance, sword on sword, recognition, enemy reactions).

## 6. The hit pipeline (2026-11-20, "sword hits no longer register")

### The game's own sword attack, call chain

1. **The attack starts.** B starts Link's cut procedure. For example, `procCutA_init` calls `setNormalCutAtParam`
   (023E5B0C) or `setFinishCutAtParam`, which call `setAtParam`. That sets each of the 3 capsules' attack type
   (`AT_TYPE_SWORD`, cps + 0x10), power (cps + 0x14), hit mark, sound and radius (cps + 0x134). It also turns on the
   capsules' attack bit (cps + 0x00, bit 0).
2. **Registration.** In `daPy_Execute`, Link's armed capsules are entered into this step's collision with
   `cCcS::Set` (0200E240). They also go into the mass collision (02516C14, for grass and bushes).
3. **The check.** The game's collision runs once a logic step, after every actor has run. Each attack (At) and
   target (Tg) pair whose shapes touch, and whose type masks match, is marked on both sides:
   - on the attack: hit bit cps + 0x04, the target's collider at cps + 0x08, the hit position at cps + 0x70, and the
     shield-hit flag at cps + 0x54 (`d_cc_s.cpp:394,403`);
   - on the target: its Tg hit bit (collider + 0x1C) and our capsule as its attacker (collider + 0x20).
4. **The target reacts in its own next execute.**
   - **Pot** (`d_a_tsubo_dmg.cpp`): if `mCyl.ChkTgHit()` and `GetTgHitObj()->GetAtType() & AT_TYPE_SWORD`, it calls
     `damaged(1)`, which breaks it, drops items, and plays its effect and sound.
   - **Enemies:** their own hit handlers read the attack's type and power (`cc_at_check`), then do damage, knockback
     and their reaction animation.
   - **Grass** (`d_a_grass.cpp`): it is cut through the mass collision.

### The VR sword, call chain

1. The pose is sampled each headset frame (`sync_input_locked` feeds `SwingDetector` and `SwordMotion`).
2. In `link_executed`, after `daPy_Execute`:
   - The physical sword arms on a swing: the hand's speed, or (since 2026-11-20) the blade's. On a new swing it calls
     the game's own `setNormalCutAtParam` and `ResetAtHit`, which give the same type and power as the game's slash.
   - The capsules follow the tracked blade (`sweep` / `sweep_path`).
   - They are entered with the game's `cCcS::Set` and the mass collision.
3. The game's collision check (unchanged) marks the hit on both sides.
4. In `link_executing`, before Link's own logic, the hits are read (haptics, the hit log) and the capsules are switched
   off, so Link's logic never sees an attack it did not start.
5. The target reacts in its own execute, through the game's own code. Nothing in VR damages or removes anything
   itself.

### What was found

- **The first test with the new build hit nothing.** The capsules were armed for only about 10 logic steps in the
  whole test, because a swing armed only when the hand passed 1.6 m/s, and elbow and wrist swings never did. Fixed by
  arming on the blade's speed (`vr.physicalSwordSpeed`).
- **The next test** recorded 9 hits on the attack side.
- **The trace** shows, for each hit:
  - what we attacked with;
  - which actor's collider the game matched it with;
  - whether that collider took the hit by our capsule;
  - over the next 30 steps, whether the target lost health or was removed.

  It writes `[vr hit] ...` lines in the log, so a test shows exactly where a hit stops.

## 7. Feeling like a real sword (research and plan, 2026-11-20)

The goal, in the owner's words: in motion controls, all swings and hits are done by the player, like a real sword.
Keep Wind Waker's look and its gameplay foundations.

### What VR melee games do (research)

- **The blade at the hand, 1:1.**
  - Broken Edge's director rejects drag-based weight: it "breaks coherency", because "the faster you move, the further
    your virtual hand is from your real hand".
  - Blade & Sorcery's spring and damper lag reads as weight to some players and as too slow to others. It is a tunable
    setting there.
  - For Link's light sword: no lag in free air.
- **A touch is not a hit.** In Blade & Sorcery, a light tap does nothing, while a hard swing or thrust does. The
  blade's speed at the striking point decides.
- **Contact must show.** Several reviews name the sword passing through enemies as the core failure of VR melee.
  - Skydance calls the answer "visual haptics": physics and animation that make the hand's effect believable.
  - Broken Edge shows the clash itself, with a backlash that pushes the blades apart, instead of simulating it.
- **Anti-waggle.** Broken Edge designs out "hyper-speed movements such as wiggling" and rewards deliberate motion.
- **Zelda precedent.** BotW BetterVR's swing analysis decides whether a controller motion "counts as a swing" (it was
  reworked in 0.9.15). Its guide tells players to commit to full motions. That is the gesture approach, which this
  plan moves away from.

### What in our code goes against the rule today

| Today | What the player feels | Change |
|---|---|---|
| A full swing presses B: Link's canned cut (procCut), its animation and sound | Link stops and plants his feet; the cut is the game's, not the player's | Motion controls: no swing presses anything. The physical sword does every cut |
| Locked on: the physical sword is off; each swing plays the game's move (side cut, vertical slash, thrust) | Canned locked-on combat | The physical sword armed locked on too. Lock-on keeps its camera and strafing only |
| Jump attack from an overhead swing (A), combo resets, queued swings | Canned moves | Off in motion controls |
| A parry is the game's A move (leap or roll) and its finishing cut (`parry_move_proc`: the game's capsules) | The cut is the game's | Keep the game's parry move (the dodge, a foundation of the fights). The finishing cut becomes the player's blade, with the game's own parry-cut attack parameters, so enemies take it as a parry cut |
| One hit per target per swing; a new swing only from rest | A fast back-and-forth on one enemy counts once | A reversal of the blade's direction starts a new swing. The enemies' own hit-stun still limits damage |
| The power is the normal cut's for every hit | A light and a hard hit do the same | Already built and off by default: `vr.swordStrongHitSpeed`. From the first test's hit speeds (2.3 to 22.7 m/s), a value of about 10 m/s gives a hard swing a finishing cut's power |

### Risks to the game's foundations (to check before it ships)

- **Orca's training** asks for specific moves (and the parry). With no swing-pressed moves, a lesson may not count.
  It needs testing; the B button can still do the game's moves.
- **Darknuts** lose their armour to parries and to specific cuts. The parry stays, but the plain cuts need checking.
- **Spin attack and Hurricane Spin** are the game's moves. They stay on their buttons; they are not swings.
- **The Skull Hammer:** this plan covers the sword first.

### Built (2026-11-20): the player-only sword (`vr.swordPlayerOnly`, on)

- **No swing presses anything** in first person with the sword. It was a full swing that pressed B; the owner
  reported that moving the arm into position "will set at least 2 of Link's swing animations off" and that it "can
  feel disconnected". Each full swing now logs `your blade cuts, no game attack`.
- **The tracked blade cuts locked on too.**
- **A fast back-and-forth is a new swing** each time the blade goes back the other way (`blade_reversed`: more than
  about 107 degrees, at arming speed, at least 0.1 s after the swing began). Its hits start afresh; the enemies'
  own hit-stun still applies.
- **Hard hits:** `vr.swordStrongHitSpeed` is now 10 m/s by default.
- **Kept on their buttons**, as asked ("parry and spin and b attacks should stay"): B attacks, the spin attack, and
  A's parry. The parry's finishing cut is the game's own, started by your swing; the blade's speed now counts as
  that swing.
- **Link's voice with each swing** (asked for: "make sure links voice plays with the swings too"): at each physical
  swing's start the game's own `daPy_lk_c::voiceStart` (02443880) plays. It uses 0, a normal cut's voice, or 1, a
  finishing cut's, when the swing starts at `vr.swordStrongHitSpeed`. Two are at least 0.27 s apart, so a flurry does
  not stack them. Logged as `Link's voice`. The swing's swoosh is not played: in the game it comes with the cut's
  animation.
- **Only real swings count** (asked for: "I just move my hand very lightly ... Link will yell ... attacks that
  register should be ones that resemble a swing"). `BladeSwing` (`vr_sword_motion.h`) runs every headset frame:
  - the blade's point three quarters of the way to its tip must move at `vr.physicalSwordSpeed` (now 4.5 m/s);
  - it must keep going, never slower than 2 m/s, for `vr.swordSwingDistance` (0.30 m, about 27 degrees of arc);
  - only then is it a swing: its hits armed and Link's voice played (`[vr] swing: a real swing`);
  - a back-swing must build up again;
  - the hand's swing and the blade's speed alone no longer arm the player-only sword.

  In `vr_test`, a real slash counts in about 0.05 s and a real back-and-forth counts twice. A fast but short wrist
  flick, a light hand move, moving the sword into position, and twitching do not count. A slow 90-degree swing over
  0.25 s (4 m/s) does not count either; that is the tuning to check in the headset.
- **Thrusts still count** (asked for: "make sure thrust attacks still work"). A thrust is its own kind of real swing:
  - the blade pushed along its length, tip first (at least 0.8 of its motion toward the tip, and 0.6 while it goes
    on), at 1.8 m/s or more, for 0.15 m;
  - stabs are shorter and slower than slashes (play tests: 1.8 to 3.4 m/s of the hand, 0.1 to 0.3 m);
  - a real thrust hits with the game's forward thrust's cut (`procCutF`'s `CUT_TYPE_CUT_F`, 2);
  - pulling the sword back toward the hilt, even fast, is neither a thrust nor a slash.

  In `vr_test`, stabs at 2 to 6 m/s count as thrusts, and a stab with its pull back is one thrust. A light poke
  (0.06 m), a slow push (1 m/s), a sideways shove and a pull back do not count.

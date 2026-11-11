# VR testing: progress checkpoint

Read this, run `git status` and check the repository before continuing a session. A confirmation is never taken from
this file or from an earlier session: only the tester's own statement in the current conversation counts.

| | |
|---|---|
| Current stage | all three stages complete (Stage 3 declared passed by the tester 2026-10-26) |
| Current test | none |
| Last confirmed test | Stage 3, stability (the tester, 2026-10-26: "lets commit all changes and stability test is passed") |
| Last commit for a confirmed test | d1e2d150 (Stage 3: P-FLK-01, the hands, bracelets and view; Stage 2: 83748aab; Stage 1: 70102477) |
| Changes awaiting manual validation | none (Stage 1 committed) |
| Outstanding issues | none open. Not run: the frame-rate targets (P-RATE-*, P-ADAPT-01); the frame rate is 62 to 85 frames/s at 90 Hz, the game thread the limit (fix 3, profiling, left for later) |
| Next required action | none in the test process; for later: profiling the game thread at 90 Hz |
| Testing rule | Every test is done by the tester in the headset. The agent runs no automatic game tests, and only builds. |

## Commands

The build:

```bash
/c/PROGRA~2/MICROS~2/2022/BUILDT~1/Common7/IDE/COMMON~1/MICROS~1/CMake/Ninja/ninja.exe -C build/game
```

The tests (608 when this was written):

```bash
cd build/game && ctest -j 8
```

Starting in the test environment: launcher > **VR test (Forsaken Fortress)**, or `build/game/wwhd.exe --vr --vr-test`,
then load your save. A scripted desktop check:

```bash
WWHD_VR_TEST_FORTRESS=1 WWHD_VR_TEST_ENV_AT=2400:reset,2900:exit build/game/wwhd.exe --no-vr
```

## Log

- 2026-10-26: the test environment implemented.
  - Desktop runs (not the headset): it enters at the courtyard; the inventory is full (GamePad Items screen); reset
    returns Link to the spawn; leaving restores the original world (Outset, 3 hearts, 5 rupees, the telescope); the
    save folder was unchanged.
  - `vr_test` and the suite (608) pass.
  - R-ENV-01 is presented for confirmation.
- 2026-10-26, first headset run (R-ENV-01, the grappling hook, the sword, the bow):
  - The environment entered after one retry. The reset state was refused (I-02). Quitting left the test world as
    Last session (I-03, the player's file restored).
  - Feedback: the grappling hook should be on the off hand (I-04); the bow should shoot by drawing with the left grip
    (I-05); the left hand was missing and the body out of place with the bow (I-06); the laser dot was too big (I-07).
  - All fixed in the source and built; the game is not run by the agent any more (testing rule).
- 2026-10-26: Stage 1 confirmed complete by the tester ("right hand test is complete, all tests passed, accept
  changes, commit all changes and lets move to left hand test"); committed as 70102477. Stage 2 begins:
  vr.leftHanded=1 set in the tester's settings.ini (backed up first).
- 2026-10-26: Stage 2 (left hand) and the item wheel confirmed by the tester ("ok we are good commit all changes to
  right hand and left hand modes and all changes to the menu we just made and lets move on to stability tests").
  Committed as 83748aab. Stage 3 begins.
- 2026-10-26, Stage 3: the hands shook and swung while moving and stick turning (I-30); fixed by re-aiming the arms
  every drawn frame, with the hands, held models and Power Bracelets moved with them (I-31), and the view held still
  while the body turns (I-32). SteamVR set back to 90 Hz (it was at 120). Confirmed in the headset ("everything looks
  good now", "lets commit the changes"). Committed as d1e2d150.
- 2026-10-26: Stage 3 declared passed by the tester ("lets commit all changes and stability test is passed"). The
  frame-rate target tests (70 to 90 Hz) and the adaptive rate were not run; the frame-rate work (profiling) is left for
  later.

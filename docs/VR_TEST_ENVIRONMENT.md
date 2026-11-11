# VR test environment: Forsaken Fortress

This is one repeatable place to check every VR interaction. Link has every item, full resources and the Master
Sword, in the real Forsaken Fortress. The tests are listed in [VR_TEST_REGISTRY.md](VR_TEST_REGISTRY.md), and where
testing stands is in [VR_TEST_PROGRESS.md](VR_TEST_PROGRESS.md).

## Entering, resetting, leaving

| | how |
|---|---|
| **Enter** | Launcher > General > **VR Weapons Test**, or start the game with `--vr-test`, then load your save as usual. You can also enter from a running game: F1 > VR > **Enter the test environment**. Scripted runs set `WWHD_VR_TEST_FORTRESS=1`. |
| **Reset** | F1 > VR > **Reset**: back to the moment of arrival (the place, the enemies, the items), with the items given again. |
| **Refill** | F1 > VR > **Refill items**: the items and resources again, where Link stands. |
| **Leave** | F1 > VR > **Leave**: your own world comes back exactly as it was before entering, with the same place, inventory and progress. |
| **Picto Box variant** | F1 > VR > "Deluxe Picto Box": on gives the Deluxe Picto Box, off the plain one. It is given at the next Reset or Refill. |
| **Running out** | F1 > VR > "Unlimited health, magic and ammo while testing". Turn it off to test empty arrows, bombs and magic. |
| **Scripted** | `WWHD_VR_TEST_ENV_AT=frame:what,...`, where `what` is `enter`, `reset`, `refill` or `exit`. These run at those frames of the service, as the buttons do. |

**Showing that it's on:**
- In the headset, the rupees panel on the right wrist shows an amber **TEST** in place of the rupee gem. That panel
  needs "Wrist: rupees" on.
- On the desktop, F1 > VR shows the current phase and the last step, and each step also shows as an on-screen notice.
- The log has `[vr test] ...` lines.

## What it does (`runtime/src/vr/vr_test_env.cpp`, `.h`)

It is built only from the game's and the runtime's own services:

1. **Isolation.**
   - The save folder is copied to `<states>/vr-test/save`, and in-game saves go there until you leave. The editor's
     test mode does the same (`hle_fs::set_save_root`).
   - The runtime's auto save is held off while you test. Its setting is not changed (`ss::set_auto_save_paused`).
   - A portable state of your world, `<states>/vr-test/return.wwstate`, is written first. Leaving loads it back.
   - The infinite health, magic and ammo switches are put back as they were.
   - Nothing writes your own save files.
2. **The place.** The game's own stage change (`ss::request_warp`) to `MajyuE`, room 0, spawn point 0. That spawn is
   in the stage's own PLYR list, at (-230, 37, 305): the courtyard's lower level by the entrance. It has room to swing
   and aim, with walls, crates, barrels and stairs.
3. **The loadout.**
   - The Gameplay cheats (`mods/cheats.cpp`), which give each item the way the game's own `item_func_*` do:
     - **items:** every inventory item (telescope, sail, Wind Waker, grappling hook, spoils, bait and delivery bags,
       boomerang, Deku Leaf, Picto Box, Iron Boots, Magic Armor, the Hero's Bow with fire, ice and light arrows, bombs,
       hookshot, Skull Hammer) and the Power Bracelets;
     - **ammo:** 99 arrows and 99 bombs;
     - **stats:** 20 hearts, double magic, the 5000-rupee wallet, full;
     - **bags:** filled;
     - **sword:** the Master Sword at full power and the Mirror Shield.
   - On top of the cheats:
     - the four bottles hold an empty bottle, a red potion, a fairy and water;
     - the chosen Picto Box variant goes in its slot;
     - the sword and shield are equipped in the play's copy too. The fortress's first visit takes the sword at its
       load (`d_s_play`: stage type FF1 with event 0x0801).
4. **The reset state.** `<states>/vr-test/fortress.wwstate` is written on arrival. Reset loads it, then gives the
   loadout again.

The order is a small stage machine, tested in `vr_test` (`test_test_env`). Before entering, Link must be in control
for 30 frames. A return state the game refuses (a scene still changing) is tried again, up to 5 times. A refused stage
change goes back.

**Measured on the desktop:**
- Entering gives the full inventory. The GamePad's Items screen showed:
  - every item, with the light arrows ×99 and the bombs ×99;
  - the Deluxe Picto Box;
  - the four bottles: empty, red potion, fairy, water.
- Life was 80/80, rupees 5000 and magic 32. The Master Sword and Mirror Shield were on B and on Link's back.
- Reset brought Link back to the spawn.
- Leaving restored Outset: 3 hearts, 5 rupees, only the telescope, the same place.

## Dependencies and gaps at this place

| item / check | at Forsaken Fortress |
|---|---|
| Sail | needs the sea (the boat): not here |
| Wind Waker | conducting works; the songs are not given (they are story progress) |
| Skull Hammer switches and pegs, grapple points, hookshot targets | to be found or added in the fortress (see the registry's notes); otherwise tested where they are |
| Enemies | the fortress's own (Moblins, Bokoblins, depending on your progress) |
| Picto Box pictures | the game's own pictures; the variant is chosen above |

## Your own world

Your saves, progress and inventory are not changed:
- in-game saves go to the copy;
- the world comes back from the return state when you leave;
- after a desktop enter, reset and leave run, the `--save` folder was byte-identical to before. No in-game save was
  made in that run: saving from the game's menu while testing is a manual check (test R-ENV-01).

If the game closes while the test environment is on, the next start is your own world as your last save left it.
In-game saves made during testing exist only in `<states>/vr-test/save`.

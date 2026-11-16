# Steady hands during turns: the hands layer (plan, 2026-11-12)

**Problem.** At 30/45 fps, Turn smoothing re-sends each game frame turned by the stick for the refreshes in between.
Far scenery turns correctly; Link's hands, sword, shield and held items turn with it and snap back on the next game
frame (the shake). A per-pixel depth guess (tried twice) left gaps and stutter. The fix: draw Link's hands and held
items into their own image and send it to the headset as a second layer that Turn smoothing does not turn.

**Status:** plan only, nothing built. Every step below is untested until headset-confirmed.

## What the code already gives us

- **Link draws into his own draw lists.** `daPy_lk_c::draw` switches to `dComIfGd_setListP1()` before entering his
  models (decomp `d_a_player_main_01.cpp`); the lists are `mpOpaListP1` / `mpXluListP1` (`d_drawlist.h`), separate
  from the world's.
- **Those lists are drawn through a function we already hook.** `J3DDrawBuffer::drawHead` (027F10CC) / `drawTail`
  (027F1174) walk a list's packets (`j3d_draw_guard.cpp`). Comparing the list being drawn with Link's two lists tells
  us "Link's pieces start here, end here": one check per list, not per piece.
- **The body is already hidden in first person** (`vr.hideBody`), so what is left in Link's lists is his arms, hands
  and what he holds.
- **The headset already takes several layers** (the UI quad, wheel, wrist panels).

## Steps and time estimates

A "session" is one evening's working block. Headset tests are short runs like tonight's.

| # | Step | My time | Your headset tests | Done when |
|---|---|---|---|---|
| 1 | Tag Link's lists: markers in the drawn command stream around `mpOpaListP1` / `mpXluListP1`; log how many draws each frame fall inside; confirm the sword, shield and held items are in them | 0.5 session | none (a desktop run shows it in the log) | the log shows Link's draws, and only his, tagged |
| 2 | Draw the tagged pieces into a second image per eye (same size, transparent background), tested against the world's depth so walls still hide the sword; the world image then has no hands | 1-2 sessions | 1-2 | captures show the world image without hands, the hands image with only hands, lining up |
| 3 | Send the hands image to the headset as a second layer on top of the world (real frames: both at the same pose, looks exactly as today) | 0.5-1 session | 1 | no visible change in normal play (the check that nothing broke) |
| 4 | Turn smoothing: on the repeated refreshes, the world layer turned (as now), the hands layer not turned | 0.5 session | 1-2 | hands, sword, shield steady during stick turns, far scenery as good as plain Turn smoothing |
| 5 | Setting to switch it off (asked for), in the in-game VR settings and the launcher; only at 30/45 (Capped 45, Steady 30, Auto when it drops), nothing at a real 90 | 0.5 session | none | the toggle works both ways |
| 6 | Quest: the same on the Quest build, measure its cost, keep it PC-only if it costs too much | 1 session (needs the Android build tools) | 1-2 on Quest | measured on Quest 3 |

**Total:** about 3-4.5 sessions and 4-6 short headset tests on PC; about 1 more session for Quest.

## Step 1 result (2026-11-12, desktop-only runs, VR blocked)

Built: `OP_VR_HANDS` markers around Link's two lists (`j3d_draw_guard.cpp` drawHead/drawTail hooks, the buffers at
`g_dComIfG_gameInfo + 0x5D58` and `+ 0x5D60`), counted on the render thread (`[vr hands] (10 s)` in the log);
`WWHD_VR_HANDS_SKIP=1` leaves the bracketed draws out. About 40 draws a frame fall inside; the opaque list is walked
about 5 times a frame, the other once. With the draws left out, Link disappears completely, his shield and sword with
him, and nothing else in the scene goes; his shadow stays (it is drawn elsewhere), the HUD stays. So Link's lists hold
Link and what he carries, as the plan needs. Still to check in step 2: which of the 5 walks of the opaque list draw into
the eye picture (the others must be left alone).

## Item and mode audit (2026-11-12)

**From the code: what goes into the hands layer.** Only what Link's own draw enters (`daPy_Draw`): his body (hidden in
first person), arms and hands, and the item models he holds himself: sword, shield, bow, the hookshot's handle, Deku
Leaf, Skull Hammer, the Wind Waker baton, Picto Box, bottles. Separate actors stay in the world picture (visible, but
they still shake in a stick turn until they get their own draw hooks): the hookshot's chain and tip (`d_a_hookshot`),
the boomerang (`d_a_boomerang`), bombs, arrows (also the nocked one), the grappling hook's rope (`d_a_himo2`), the boat.
The layer is used only in first-person play (not third person, the telescope, the Picto Box view, menus, events).

**Rule now:** every draw into the hands layer counts as fully covered (solid and blended draws alike), so nothing in it
can be invisible; a translucent effect on the hands (a glow) shows over black at its edges at worst.

**Headset checklist** (each: visible, solid, steady in a stick turn at Steady 30):
- Empty hands; sword out; shield up; both held.
- Bow drawn (with an arrow nocked); hookshot held and fired; boomerang held and thrown; a bomb held and thrown.
- Deku Leaf (held, and gliding); Skull Hammer; Wind Waker (conducting); Picto Box held; a bottle.
- Grappling hook (held, swinging); a stick or enemy weapon picked up.
- Standing, crouching, climbing a ladder, hanging on a ledge, swimming, sailing (on the boat).
- Third person: Link drawn normally (no hands layer). Telescope and Picto Box view: normal.

**Headset results so far (2026-11-12, Steady 30, test switch `WWHD_VR_HANDS_LAYER=1`):** hands steady during stick
turns; all held weapons and the hands holding them visible after two fixes (every hands-layer draw counts as fully
covered; the hands write depth again, with each eye's depth saved before Link's draws and put back after them, so
two-pass models draw and the world behind is not hidden). Still to do: step 5 (the setting, off at a real 90), the
separate weapon objects (hookshot chain and tip, thrown boomerang, bombs, arrows) via their own draw hooks.

**Shadows (2026-11-13, headset-confirmed "shadows look right"):** the game's screen-space sun shadow mask is made from
the depth without Link's hands (no dark copies of their outlines in the world behind them, which slid out as black edges
in a turn). On PC VR the sun shadow pass is drawn a second time for each eye from the depth with his hands into his own
mask, which only his draws read: under a tree he is shaded, a distant cliff's shadow does not land on him. On a
standalone headset (Quest) his draws get a mask that shadows nothing instead (no second pass, no extra depth copy); he
still casts his shadow (the sun's shadow map). A VR option for that comes with step 5.

**Open (earlier):** Link's body shows in first person with the layer on (the log's `[vr body] (10 s)` line says whether his
hidden body is drawn again by the game's later update).

## Cost (estimates; measured at steps 3 and 6)

- **CPU:** small. One check per draw list, a few extra commands a frame, one extra layer sent (on the end-frame
  thread). The game thread's work does not change.
- **GPU:** an extra eye-sized image per eye, cleared and stored each frame; Link's pieces drawn once either way (moved,
  not added); the headset combines two layers. PC under 1 ms, Quest about 1-2 ms. Because Link's pieces are drawn as
  whole lists, the drawing switches image only a few times a frame (the costly part on a Quest GPU stays small).
- **At a real 90 fps:** nothing (step 5 keeps it off).

## Risks

- **What else uses Link's lists.** Step 1 checks this; anything that isn't his hands or items would need moving back.
- **See-through effects on the hands** (the sword's glow, the Deku Leaf) and **Link's shadow**: they may draw in other
  lists; checked at step 2.
- **The sword in a wall during a turn:** on real frames walls hide it (depth test); on the repeated refreshes of a turn
  it may show through for a fraction of a second.
- **Our two-eyes-at-once drawing (multiview)** must send Link's pieces to the second image for both eyes; checked at
  step 2.
- **If step 1 or 2 shows it can't work cleanly**, we stop there and nothing else is changed.

## Status (2026-11-13, end of day)

- **Steps 1-4:** built; hands steady during stick turns, all items visible (headset-confirmed).
- **Step 5:** `vr.handsLayer` ("Steady hands while turning", in-game VR menu and launcher), on by default on PC, off on a
  standalone headset. Runs only with Turn smoothing at 30/45 (Steady 30, Capped 45, Auto Mode when it drops); off at a
  real 90 (headset-confirmed: 87-90 fps, the second drawing thread working). `WWHD_VR_HANDS_LAYER=1/0` forces it for a start.
- **Threads:** Link's draws, the depth copies and the game's depth-reading passes are handed to the scene's drawing thread
  (kStateHands, in order with his draws); in light frames (no scene thread) they are still on the render thread.
- **Shadows on Link:** the game's screen passes read the depth with him (stopped the shadow outlines round his hands);
  distant shadows still land on him with `vr.shadowsOnLink` on, so it is off by default in every build.
- **Sword through objects:** his pixels behind what the world drew after him are left out in the dilate pass (built,
  not headset-tested; PC only, it needs the depth with him).
- **Cost at 30 fps (PC):** about +2.6 ms GPU a frame (about 36 full-size depth copies); region-limited copies to come.
- **Step 6 (Quest):** compiles for ARM64 with the Quest paths; not built into an APK or run on a Quest yet.

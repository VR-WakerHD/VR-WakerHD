# VR cutscene and dialogue text

## How the game draws text

Dialogue boxes, subtitles, the legend's story text, prompts and tutorials are HD layouts (nw::lyt) drawn into the TV
picture: screen-space 2D, not world objects. Their projection reaches the renderer as an `OP_SET_PROJ_REGS` command
(`gx2/gx2_core.cpp`), where `aspect.cpp` already narrows the layout space for screen shapes other than 16:9. The stereo
renderer draws layouts identically in both eyes (zero disparity), so in the headset they float at the depth of the
far scene and, unscaled, span the whole field of view: too close-looking and covering the scene.

## The change

While the game is not in normal play (`aspect::gameplay_camera()` false: conversations, cutscenes, events) the TV
layouts are drawn smaller around the centre of the view, as if they were farther away, by `vr.textZoomOut` (default
3: a third of their size), and moved up (+) or down (-) by `vr.textHeight` (default +0.05, a share of half the view height;
`y += dy * w` in their projection, so the whole layout moves rigidly). The title and file select use
`vr.menuZoomOut` instead. The gameplay HUD (hearts, buttons, rupees) keeps its size, and the desktop picture is never
changed (only while a VR session runs).

Both settings are in the launcher (Camera and comfort) and the settings overlay (F1 > VR headset).

## Why not a world-space panel

The text is part of the TV picture the game composes (with the scene behind it); there is no separate textbox object
to place in the world. Drawing it into a quad layer would need the layouts rendered to their own target, which the
renderer does not separate today. Scaling keeps the original layout, fonts and timing, and puts the text into the
central, sharpest part of the lenses.

## Layouts parked off screen

The game keeps some layouts just outside the screen while they are not needed. Drawn smaller, they would come into
view: the first headset test of a conversation (Grandma's house, 1.6) showed a faint "GAME OVER" above the scene and a
minigame timer at the textbox. While layouts are scaled, every layout draw is clipped to the shrunk native screen
(moved with the text), so they stay out of view (`draw.cpp`, after the TV content clip).

## Dialogue cut off at the bottom (fixed)

**The cause:** on a screen taller than 16:9, `aspect.cpp` moves HUD panes out to the screen's edges. The message
windows' `N_Position_00` (MsgWindowMain_00 and MsgWindowEvent_00, at y -200) is one of them (`aspect_panes.h`, a HUD
rule): in a headset's eye picture, which is much taller than 16:9, it went down by `360 * (ky - 1)`, 280 units in a
square eye. VR draws conversation layouts shrunk into the native 16:9 screen and clips them to it (above), so the
box's lower part, with its bottom lines of text, was cut off. The game's own box is not too small: it is 900 x 280,
with a 700 x 180 text area for four lines.

**How many lines:** every message in the English tables (4,284 messages in `message*_msbt`, read from the player's
own game files, not copied anywhere) is wrapped by hand and shown four lines to a box; shorter boxes are padded with
empty lines. 1,188 of 6,133 boxes use all four; none needs more. Only 63 messages break the padding pattern
(special layouts).

**The fix** (`aspect_panes.h` `layout_factors`, `aspect.cpp` `calculate_aspect_pane`, `vr.cpp` `layouts_shrunk`):
while VR draws the layouts shrunk into the native screen (conversations, cutscenes, menu scenes; not the telescope's
eyepiece), their panes are placed as on a 16:9 screen. The box then stays where the game puts it, inside the clip, 20
units above its bottom. Other HUD the game shows during a conversation (a shop's rupee counter, say) also stays at
its 16:9 place, shrunk, instead of being clipped away. Gameplay, the desktop picture and non-VR play are unchanged.

**Tests:** `aspect_panes_test` reproduces the cause for eye shapes 0.9 to 1.5 and checks that the box and its text
area stay inside the native screen with the fix. Without a headset, `WWHD_VR_TEST_LAYOUT=scale:offset` with
`WWHD_ASPECT=1:1` draws every layout shrunk in a square picture. Before the fix the HUD vanished there; with it, it
shows shrunk at its 16:9 places.

**Narrowed after a headset test:** placing every HUD pane as on 16:9 while shrunk brought the gameplay HUD back into
the middle of the view whenever it faded in or out during an event (the old UI flickering back). Only the dialogue
windows' `N_Position_00` stays at its 16:9 place now (`aspect_panes.h` `keeps_native_place_when_shrunk`). Every other
HUD pane keeps its rule and stays outside the clip, as before.

**Not checked:** a real conversation in the headset.

## Tested in the headset

- 1.6, unmoved: readable, but the box was still large and low in the view (reported in the headset); the off-screen
  "GAME OVER" and timer were visible.
- 1.0 (as the game draws it): worse, the box low at the edge of the lenses.
- 2.0, +0.2: the box a third of the view wide, centred a little above the middle, the text readable (conversation
  with Sturgeon on Outset); still too large in the headset.
- 3.0, +0.2 (the defaults now): the box about a fifth of the view wide, near the middle of the view; the letters about
  1 degree tall (conversation with Sue-Belle on Outset); `drawn at 0.33` in the log. 3 is the setting's limit.
- 3.0, +0.05 (the defaults now): moved down from +0.2 (asked for near the middle of the view, slightly lower). The clip was not yet seen in Grandma's house, where the stray layouts showed.

`WWHD_VR_SHOT_TEXT=1` (tests) takes a screenshot 2 s into each conversation or cutscene.

## Heat haze over the text

In hot places (Dragon Roost, lava rooms) the game draws heat haze: particles in its projection group (draw group 4 of
`dPa_control_c::draw`; `o kagerou00`, effect 0x4004, set in group 4 at 0239C464, and the stages' `heatfloor00/01`)
that bend the picture behind them. With the dialogue's box drawn small in the middle of the view, the haze lay over
it and the text wavered (reported in the headset). While the layouts are drawn shrunk (dialogue, cutscenes, events)
VR does not draw that group (`vr_ui.h` `draws_particle_group`, `mods/vr_hooks.cpp`); in play the haze is drawn as
before, and the desktop picture is unchanged.

**Not checked:** a conversation in a hot place in the headset.

## Conversations in first person (2026-10-31)

Asked for in the headset: talking to a person in first person should stay in first person, not switch to the game's camera. The text box should appear where you look and stay there, not move with your head, and your view should stay on the person you talk to.

- **Telling a conversation from a cutscene:** a conversation is the event control's mode 1 (`dEvtMode_TALK`, play +0x5292 = `dEvt_control_c::mMode` at +0xC2 of play +0x51D0). A cutscene is 2, and a compulsory event such as conducting is 3. `aspect::talk_camera` checks mode 1 with Link in the play, and no menu, pause, stage change or scene overlap. The message's own status and state (play +0x5BB2, +0x5BB3) are set all through a conversation, so unlike normal play they do not count against it.
- **The view** (`vr_game.cpp` `talk_first_person`, in `play_view`). In first person with `vr.firstPersonTalk` on (the default; F1 > VR "Conversations in first person"), a conversation counts as normal play for the view. The camera stays at Link's eyes with your head's pose, his head stays hidden, and his arms stay on the controllers. The game's conversation camera is not used, so your view stays on the person you walked up to. In third person, or with the setting off, the game's camera shows it, as before. Cutscenes are unchanged.
- **The text box held in the world** (`talk_text_anchor`, called from `CameraOverride` every frame). The game draws the box as a flat layout at the picture's centre, so it followed the head. When the conversation begins, the direction you look is kept, in game space, so turning with the stick turns it too. Every frame the layouts move across and up (`vr::layout_offset_x` / `_y`, applied in `gx2_core.cpp` as `x += dx * w`, `y += dy * w`, and to the layouts' clip in `draw.cpp`) to where that direction is in the view now. That is how a sign far away would look; the layouts are drawn alike in both eyes. `vr.textHeight` still moves it from there, and its size is `vr.textZoomOut` as before. Looking far away takes it out of view.
- The log says `[vr] conversation in first person: ...` when one begins and ends.

Still to check in the headset: a conversation with a person (Outset), a sign, a shop, and one whose talk turns into a cutscene (the camera should go to the game's when the mode changes to 2).

## No conversation text on Quest 3 through VDXR (2026-11-03)

A player on a Quest 3 through Virtual Desktop (VDXR) saw no text in first-person conversations, and thought A did
nothing (the log showed 13 presses reaching the game and the conversation ending). Their game sent the scene's depth
with each frame (`vr.frameSynthesis` auto: depth only, `XR_KHR_composition_layer_depth`); runs without depth showed the
text. The text is drawn into the game's UI layer, a quad 3.5 m ahead (docs/VR_EYE_PROJECTION.md), and a runtime that
composites the layers by that depth puts the person talked to, 1 to 2 m away, over it. Menus send no depth and showed.
Now the depth is not sent while the UI layer shows text or a menu (its layouts drawn shrunk, `vr::layout_scale` under 1;
`openxr_session.cpp` `end_frame`); normal play keeps it. The log says `[vr frame synthesis] depth not sent while the
game's UI layer is shown` and `sent again`. Not yet confirmed on that headset.

The same log showed the view jumping about 30 cm up and down six times in one conversation: Link's talking (action
`0xAA`) moved his eyes 31 to 35 units for seconds at a time, past the standing hold's limit, and the view followed. In a
conversation in first person the eye height is now held until it ends (`vr_game.cpp`, the eye height).

## Dialogue in the UI layer (2026-11-04)

A player on a Quest 3 through Virtual Desktop (VDXR, wired, build `bb2fd65`) sent two videos and a log: the dialogue
box very faint (a light band behind white text, hard to read over a bright wall), text cut off, and text gone while
the box's "next" arrow stayed, in first-person conversations. Their UI layer's image was 3841x3985 (each eye 108 x 110
degrees), the text at 0.50 of its size. Four changes, all with each eye's own projection on:
- **The UI image has a whole alpha channel** (`draw.cpp` `ui_layer::surface`): it took the game's picture's format,
  A2B10G10R10, whose two bits of alpha gave the coverage the quad is blended by four levels. A translucent box or a
  glyph's soft edge was rounded to a third, two thirds or nothing. It is RGBA8 sRGB now: the game's colours are kept
  (encoded on write, decoded for blending and reading, the blit into the sRGB swapchain unchanged), the alpha has 256
  levels.
- **Layouts placed for the UI image's shape** (`gx2_core.cpp` OP_SET_PROJ_REGS, `stereo::ui_layer_factors`): they were
  placed for an eye's narrower picture and drawn into the symmetric field's, so they were stretched across by the two
  shapes' ratio (1.04 on that Quest 3, 1.32 on a PS VR2), the shrunk screen's clip (`draw.cpp`) with them. Both now
  take the UI image's factors: the layouts' 16:9 space fills its width, undistorted.
- **A first-person conversation's text held by the quad** (`talk_text_anchor`, `vr::talk_text_dir`, `vr_vulkan.cpp`):
  the layouts stay centred in the UI image; the UI layer's quad is placed in the tracking space, 3.5 m along the
  direction held since the conversation began (turned with the stick as before), facing you, upright. The compositor
  holds it there at the display's own head pose. The layouts had been moved in the picture once a game pass by their
  projection while their clip moved by each draw's offset: the text went out of its clip (cut, then gone, the arrow
  kept). Without each eye's own projection the layouts' offsets are used as before.
- **The quad's size** was checked: the UI image's shape and the quad's size both come from `vr::projection` (the
  symmetric field), eye crop or not. No change.

## The text turning white sooner (2026-11-05)

A Quest 3 player through Virtual Desktop: in some conversations the text, turning white as it is spoken, stopped part
way and stayed so until the page closed (a video: the whole page faint, the white stopped mid-line for 1.5 s). Not
found in the code: the game's UI screens (`interp.cpp` `hook_02715310`) run once per logic step on every pass, with no
time limit. While a message is up in VR (play + 0x5BB2 / 0x5BB3, not over a menu), those screens are now updated
`WWHD_VR_TEXT_SPEED` times a logic step in all (default 4, 1..8; 1: as before), so the white reaches the end of the page
before it could stop. Logic passes only (never in-between passes: issues #64, #74); a message waits for the player's
button, so a faster screen only finishes its animations sooner. The log says `[vr] conversation text: ... 4x as fast`
once. Not yet seen in a headset; if pages close or skip on their own, `WWHD_VR_TEXT_SPEED=1` turns it off.

## The white stopping: a page typed twice (2026-11-05)

Two more videos (Quest 3 through Virtual Desktop, build `60ea8f1`) and a log. The log never printed the 4x line: the
speed-up above did not run in that session (`message_up` or the menu flag said no). In both videos a page typed out
whole and white, then the same page typed again from the start, faint, and that second typing is what stopped:
"aux <red>fleurs bombes</red>" (the French Dragon Roost page) stopped at the first red letter for about 3 s with the
page's arrow up; Medli's "Well, to tell the truth..." was cut at "Well, to" by the next page. The French page's text
(message2.msbt) has only colour codes there (`<0.3:0001>` red, `<0.3:ffff>` back), no wait.

Two hooks (`mods/vr_hooks.cpp`, VR only):
- `hdMsgWindow_updateWaitInput` (026FD1DC): at the end of a page (the next stop offset negative, only the button left)
  a window whose letter count is short of the whole page shows them all (0x14F, as a button's skip does), logged as
  `[vr] message window XXXXXXXX: page at its end with N letters shown (stop ...; counts ..., offsets ...): all shown`.
- `hdMsgWindow_updateOpen` (026FCC10, diagnostics): each message window that types is logged once
  (`[vr] message window XXXXXXXX typing (message N)`), and a window's count going back to the start
  (`typing back to ...`): one window retyping, or two windows typing the same text, which VR draws in one place.

**The speed-up moved (same day).** It never ran where it was: the HD UI screens' update (`interp.cpp`
`hook_02715310`) asked the old message flag (play + 0x5BB2 / 0x5BB3), which is off in HD's conversations, and those
three screens are not the message windows. It is now in the message window's own typing (`hook_026FCC10`): in VR the
letters a step reveals are multiplied by `WWHD_VR_TEXT_SPEED` (default 4, 1..8; 1 off), never past the window's next
stop (`mStopCounts[mStopIndex + 1]`, as the game caps it), so the game's pauses and page ends are unchanged. The log
says `[vr] conversation text: typed 4x as fast in VR` once.

## Half the page left dark: the speed-up moved into the game's typing (2026-11-09)

Seen in the headset on the user's save (messages 5268-5270): the text went by too fast and parts of each page never
turned white. Each message moved on at an A press, and none reached the page end that waits for the button
(`page waits for the button` was never logged). The speed-up raised the window's letter count after
`hdMsgWindow_updateOpen` had run, so the window's own update (its virtual 0x434, which turns each letter white as it is
spoken) never ran over the letters jumped: they stayed dark, and the page looked finished, so A was pressed again. Now
the speed-up adds the step's typing speed (`l_windowCore` at 0x101F4B5C, `mTypingSpeed` +0x980) `WWHD_VR_TEXT_SPEED - 1`
more times to the window's accumulator (+0x94) before the game types: `hdMsgWindow_advanceTyping` counts the letters and
the game's own update turns every one of them white, with its own pauses and page ends. The log says `typed 4x as fast in
VR (WWHD_VR_TEXT_SPEED), by the game's own typing`. Not yet tried in the headset.

## Back to the game's own text (2026-11-09)

Asked for: "lets make everything back to regular defaults for text". `WWHD_VR_TEXT_SPEED` now defaults to 1: the
message windows type at the game's own speed, and a page's end is left as the game leaves it (the "all shown" fill runs
only with the speed-up). Setting `WWHD_VR_TEXT_SPEED=2..8` turns the speed-up back on, through the game's own typing as
above. The diagnostics (each message typed, each page waiting for the button) stay. Conversations in first person, the
text held where you look and the dialogue in the UI layer are unchanged.

## Dark letters: three guards (2026-11-09)

Asked for after the speed-up was turned off: the dark letters had been reported before it ran too, so the three other
candidates were taken on (`mods/vr_hooks.cpp`, VR only):
- **A page typed again** (the Quest videos: a page typed whole and white, then the same page from the start, faint):
  in `hdMsgWindow_updateOpen` (026FCC10), the same window, message and page (stop index) going back below the letters
  it had reached, within 3 s, keeps what it had reached. Logged: `the page typed again from N (it had M ...): kept`.
  A page shown again later (talking again) types as the game types it.
- **A at a page's first steps** (`hdMsgWindow_updateStop` 026FCEE0, a new hook, and `hdMsgWindow_updateWaitInput`
  026FD1DC): while a page waits at its stop the game updates its text each step, and that is where its last letters
  turn white; an A in the first steps closed it with letters still dark. The first 6 steps (0.2 s) at a stop take no
  A (the window's confirm and skip flags cleared for that call). Logged: `A at a stop's first steps held`.
- **The UI picture a frame behind** (`vulkan/vr_vulkan.cpp`): frames with no new UI picture right after one that had
  it are counted and logged every 10 s (`[vr] UI layer: N frames with no new UI picture ...`). A diagnostic only: no
  such frames have been seen; it says whether this is a cause.

Not yet tried in the headset.

## Part-grey text and the frame rate (2026-11-10)

Found with the user: the text turned white for only part of each page (and looked too fast) at Auto's 45 fps, on
the user's E:\Games copy and forced to 45 here (messages 13008-13011, 13064), but came out right at Capped 90.
Measured at 45 fps (`[vr] message typing:` lines, `mods/vr_hooks.cpp`): the game's typing runs once a game step,
never on passes without logic, 4 letters a call, as at 90; nothing typed twice. Two fixes tried and taken out:
- the windows' updates held to once a step (nothing was ever skipped);
- a conversation drawn at 30 fps at a fractional rate (first for the typing only, then the whole conversation by
  the message status, play +0x5BB2 / +0x5BB3): still part grey at 30.

So the text is right only with in-between frames every step (90: 2 a step), and part grey at 45 (1 every other step)
and 30 (none). The game's logic is the same in all three: the cause is on the drawing side, most likely how the
game's text picture reaches the headset's UI layer (an in-between pass paints the previous pass's lists again, so the
white catches up there). Next: trace the UI layer's picture for the conversation text. Not fixed yet.

**Found and fixed: the buffer cache (2026-11-10).** In the headset at 45 fps: with three renderer caches off the
text came out fully white; with the buffer cache alone off, fully white too. The buffer cache keeps ranges the game
rewrites often ("dynamic") between the game's hints that memory changed (`hintedDynamic`, issue #1); the text's
per-letter colours change with no hint, so the copy stayed until the range's back-off ran out. That back-off is
counted in drawn frames: at 90 fps (3 a step) short enough to hide it, at 45 and 30 it spans more steps and the
letters stayed grey for part of each page. Now a kept range is reused only within the game step it was copied in
(`Cache::stepScoped`, `set_step` with the game's logic step): each step copies it again, at every frame rate (30, 36,
40, 45, 60, 72, 80, 90, 120); a step's frames still share one copy. `WWHD_VK_BUFFER_CACHE_STEP=0` turns it off. Test:
`buffer_cache_test` 9c (30, 36, 40, 45, 60, 72, 80, 90 and 120 fps, a write each step with no hint; it fails with
the fix off). Confirmed in the headset at 45 fps with the cache on: the text fully white, 45 fps held.

**A crash it made likely, fixed (2026-11-10).** The first headset play with the step-scoped cache crashed on a scene
drawing thread in `buffer_cache_slice` (crash-20261110-215248: draw() from the scene thread's draw_both). The
converted indices' cache entry was looked up under the cache's lock, the lock let go, and the entry read afterwards for
its slice; another drawing thread copying the same entry again in between (far more often now that kept ranges are
copied each step) dropped its region, and the read hit freed memory. The converted indices' slice and size, the
native indices' shadow and both extents are now read under the lock (`draw.cpp`).

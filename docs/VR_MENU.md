# The VR menu: the settings in the headset

**The left stick's click** (the left controller, in left- and right-handed play) opens the settings overlay in the
headset. (For part of 2026-10-31 it took the click held half a second, after a Quest 3 user clicked the stick by
accident while moving; in the PS VR2 headset a click then seemed not to open the menu at all, so the single click came
back.) It opens on the VR tab (every VR setting, in sections: turning and movement, view, sword and items, held
items' positions, menus and HUD, graphics and frame rate, the controller buttons, the test environment), with the
first option focused. (The VR settings were a section of the Graphics tab; the tab is shown with VR built in and the
Vulkan renderer.) The game
pauses. The same click closes it and the game goes on; closing the menu any other way (its close button, B at its top,
Esc on the keyboard) resumes the game too. The game never gets this click while VR runs.

## Where it shows

On the GamePad screen's panel (`vr.menu*`: distance, height, size, curvature, placement), placed where you look when
it opens. While the menu is open the panel shows the menu instead of the GamePad's picture
(`vr_vulkan.cpp` `record_eyes`). The overlay's draw data of the frame is drawn alone into the panel's image, 1600x900,
cleared to black first (`present.cpp` `compose_overlay_into`). The desktop window shows the menu as before.

## The controllers in the menu

By their physical places, in either handedness (`vr_game.cpp` `menu_pad`, fed to the overlay as a gamepad:
`overlay::set_vr_controller`, merged with any real controller):

| Control | In the menu |
|---|---|
| Left stick | move between options |
| Right stick | move between options, one step a push (the D-pad) |
| A (right controller) | confirm, as the game's A |
| B (right controller) | back, as the game's B |
| X, Y (left controller) | as the game's X and Y |
| Grips | previous / next tab (L / R) |
| Triggers | ZL / ZR |
| Left stick click | close the menu |

## The name of a new save

**The problem:** the game asks for the name with the Wii U's system keyboard (`swkbd`, `SwkbdAppearKeyboard`: up to 8
characters, on the file select). The settings overlay's text prompt stands in for it (`overlay/text_entry.h`: the name
field, an on-screen keyboard offering only the characters of the game's name font, OK and Cancel keys). In VR that
prompt showed only on the desktop window and took only the desktop's controllers: the headset showed the GamePad
picture ("Use the GamePad to input"), and the VR controllers reached nothing, because the game gets no input while the
prompt shows.

**Now** (`vr_game.cpp` `name_entry_open`, `name_entry_pass`, `filter_pad`; `vr_vulkan.cpp`; `text_entry.cpp`
`set_vr`): while the game asks for text (the title, the file select, and since 2026-11-07 every prompt in play too: the
settings page's rename, the game's own keyboards, which before showed on the desktop alone):
- the prompt shows on the headset's panel, as the VR menu does, where you look, and uses the whole picture there;
- the controllers drive its keyboard (`menu_pad`, `overlay::set_vr_controller`):

| Control | On the keyboard |
|---|---|
| Left stick | move between keys (one key a push; held, it repeats after 0.4 s) |
| A (right controller) | type the key chosen; on OK, enter the name |
| B (right controller) | delete the last character (never cancels) |
| X / Y (left controller) | space / shift |
| Grips | previous / next page (letters, accents, symbols) |

- a connected gamepad works on it too (the overlay merges both);
- the name goes to the game through `swkbd` (`SwkbdCalc`: the receiver buffer, OK decided), so the game's own new save
  follows;
- the buttons still held when the prompt closes (the A that chose OK) are kept from the game until released, so they
  do not press on into the next screen.

Everything else is the prompt's own: the first frame ignores buttons already held, the field takes at most the game's 8
characters and only the font's characters, and an empty name is the game's to refuse.

**Checked** (desktop, scripted through the same controller inputs: `WWHD_TEST_PAD`): a new game from an empty profile,
the name typed with the keyboard (type, move, delete, type, move to OK, press): `[text] OK: "Qwe"`, `[swkbd] entered
"Qwe"`, then the game's opening. **Not checked:** in the headset.

## The pause

While the menu is open, every pass of the game's main loop is a hold pass at the current step (`interp.cpp`, before the
logic: `vr::game::menu_pause_pass`). It is the same pass frame interpolation draws between steps, with the logic
skipped:
- no actor executes, so nothing moves;
- no timers, input or particles advance;
- it draws the current step exactly and is recorded.

The head stays tracked, the controllers are read for the menu and the hands, and the menu redraws every frame. It works
with frame interpolation off as well (the 30 fps mode): the hold pass's skips do not depend on it. When the menu closes,
the next pass is a logic pass that follows a record pass, as after any step.

The game's music and sounds keep playing, as in its own pause menu.

## Checked

In the headset (2026-10-27): the menu opens on the left stick's click with the game paused, is navigated with the
controllers, and closes on the click again.
- The panel is twice the GamePad screen's width (`kMenuPanelScale`, asked for in the headset).
- **Fixed on the way:** opening the menu froze the game. The click is read while the input's lock is held
  (`input_sdl.cpp` `read`, the pad filter), and opening the overlay takes that lock again (`input::release_keys`). The
  click now only asks for the menu, which opens at the start of the next pass (`menu_pause_pass`).

## Double vision on Linux

On Linux (SteamVR, PS VR2) the menu panel, the GamePad screen's panel and the aiming lasers looked doubled, while the
world looked right. They are quad layers, which the runtime draws itself with the display's own projection.

**The cause:** the game draws both eyes with the head's orientation and one symmetric field of view, the union of the
eyes' (`vr::as_rendered`; one projection for both eyes in the multiview renderer). The PS VR2's eyes are asymmetric:
the left eye -61.5 to +43.4 degrees across, the right -43.4 to +61.5 (the `[vr] left eye:` / `right eye:` lines). The
frame said so, and the runtime had to reproject each symmetric image to its asymmetric display. SteamVR on Linux did
that wrongly (a later beta lists "fixes distortion when scene projection and display projection differ"), so the world
landed a little off in each eye and the quads, placed correctly, no longer matched it.

**The fix:** each eye is submitted as its own field of view: the part of the symmetric image that shows it, with the
exact field of view of those pixels (`fov_crop` in `vr_math.h`, used in `openxr_session.cpp` `end_frame`; the eyes keep
the runtime's field of view in `EyeView::display`). The projection the runtime gets is then the display's, which it
shows as it is. Linux only: Windows submits the symmetric image as before (SteamVR there reprojects it correctly, and
the crop has not been tried there). Nothing is drawn differently: the cropped columns (about a quarter on the inner side of
each eye) were never shown. `[vr] left eye submitted as its own field of view: pixels ...` logs the crop once.

## Bubble vision on Quest and VDXR

**No longer offered (2026-11-04):** each eye is now drawn with its own projection (docs/VR_EYE_PROJECTION.md), so
there is no wider symmetric picture to crop and the crop never applies (the eye is submitted as drawn). The setting
was taken out of F1 > VR and the launcher; `vr.eyeCrop` is still read from settings files. The history below stands.

Players on a Quest 3 through Virtual Desktop's VDXR reported a fish-eye, bubble-like picture. Windows submitted the
whole symmetric image (above), and only SteamVR there had been seen to reproject it right (PS VR2). The crop is now a
setting, `vr.eyeCrop` (VR launcher > Camera and comfort > Eye field of view fix; F1 > VR), applied from the next frame:

| `vr.eyeCrop` | What is submitted |
|---|---|
| `auto` | each eye's own field of view (the crop) on every runtime except SteamVR on Windows |
| `on` | the crop on every runtime, SteamVR on Windows included |
| `off` (default) | the whole image as drawn |

Since each eye is drawn with its own projection (the default, docs/VR_EYE_PROJECTION.md), the whole image is the eye's
own field of view and the crop changes nothing: it is off by default and matters only with
`WWHD_VR_EYE_PROJECTION=shared`, where it is the bubble-vision fix on Quest and VDXR.

The runtime is told apart by its name (`[vr] OpenXR runtime:`); the log says what is submitted: `[vr] eyes submitted
...`. Not yet confirmed in a Quest 3: if the bubble look stays with the crop, try `vr.frameSynthesis=off` (the depth
sent for reprojection, docs/VR_FRAME_SYNTHESIS.md).

# VR Picto Box and Deluxe Picto Box

## The game's Picto Box (decomp: `d_a_player_main_02.cpp` `procScope_init`, `d_camera_subject.cpp` `subjectCamera`)

- **One procedure with the telescope.** `daPy_lk_c::procScope_init` starts both. Its argument picks:
  - `0x20` is the telescope, which sets player status 0 `TELESCOPE_LOOK` (`play +0x5CD8`, bit `0x200000`);
  - anything else is a Picto Box, which sets player status 1 `PICTO_BOX_AIM` (`play +0x5CDC`, bit `0x8`) and takes
    its model (`setPhotoBoxModel` on the boat).
- **The camera** is the same first-person subject camera, with the same zoom:
  - the left stick's Y moves a level from 0 to 1, and the camera's field of view narrows from it (`zoomFovy`, the
    gauge x1 to x9);
  - entering narrows it to 0.444 of the normal one first.
  - The standard and the Deluxe Picto Box share this path. The Deluxe one adds its own modes (the X "Switch" prompt),
    which are the game's logic and unchanged.
- **The picture** differs from the telescope's:
  - while a Picto Box is aimed, the camera draws the scene into a window of the screen (`setView`, the camera's own
    viewport), about 44 % of its height;
  - `PictographBox_00` draws the black frame around it: the zoom gauge, the stick prompts ("Zoom", "Look Around"), the
    buttons (ZR Album, X Switch, A Take, B Back) and the count (0/12).
  - The window is as tall a share of the screen as its field of view is of the normal one, so at x1 the world shows at
    its true scale in it.
- **A picture:**
  - A takes it, and the game asks "Save this pictograph?";
  - A on Yes saves it into the game's twelve-photo store (`d_picture_box.cpp`), and the count goes up;
  - B leaves the question, then the Picto Box.

## In VR before

The VR code knew only the telescope (`TELESCOPE_LOOK`):
- **No zoom, and a miniature view.** The headset's field of view replaced the camera's (`aspect.cpp`
  `vr_projection_args`), so the window showed the headset's whole field shrunk into it, and the zoom never showed.
- **The sticks and hands still did their normal jobs** while aiming: the item wheel, the wrist HUD, the right stick's
  view toggle and crouch, and the sword's swing gates.
- **Its overlay was probably hidden.** Its `N_All_00` has an `N_Mode_00` in it, as the telescope's does, so the VR UI
  took it for the telescope's overlay, which it hides when the telescope is not in use (`vr_ui.h`
  `Area::Telescope`). This is from the trace, not seen in the headset.
- **Its layouts** may have been drawn at the cutscene text's size, if the game's state check does not count aiming as
  normal play, as it does not for the telescope.

## Now (`vr_game.cpp`, `vr_camera.h`)

- **One predicate for both items.** `subject_zoom_view()` is the telescope or a Picto Box (`picto_view()`: the
  `PICTO_BOX_AIM` bit). Every gate that kept the sticks and hands to the telescope now does the same for the Picto
  Boxes:
  - no item wheel, no wrist HUD, no view toggle, no crouch on the right stick;
  - no sword swing presses, so A and B stay the game's;
  - no edge HUD layout, and no first-person "normal play".
- **Magnification** (`zoom_magnification`, the telescope's, now shared and tested):
  - while a Picto Box is aimed, both eyes are drawn with the headset's field of view narrowed by
    `tan(normal / 2) / tan(the game's / 2)`;
  - at x1 that is 2.25x (the game's 0.444) into a window about 44 % of the eye's height, so the window shows the world
    at its true scale, as on the TV;
  - the zoom then magnifies it, with the game's own limits, smoothing and gauge;
  - the head aims it, and each eye keeps its own offset, so the window is in stereo.
- **The viewfinder** (`PictographBox_00`) is drawn at full size, as the gameplay HUD is, with no telescope eyepiece
  fill. Its overlay is shown while a Picto Box is aimed, as the telescope's is while looking through it.
- **Taking a picture is the game's:**
  - A reaches the game as the GamePad's A, and the swing presses are off while aiming;
  - the capture reads the left eye's picture, as every guest read of a render target does in stereo (`stereo.h`
    `stereo_pair`);
  - so the photograph is what the left eye sees, at the head's aim and the zoom.
- **The photo is what is inside the frame** (upstream #303, `aspect.cpp`, `aspect_picto.h`):
  - the eye picture is taller than 16:9, and the viewfinder's frame is a layout fitted to its width, centred, so
    the frame surrounds the picture's centred 16:9 part;
  - upstream's #303 takes the photo, and finds its subjects, in the 16:9 part of a wider or taller picture, placed
    by the projection's off-centre terms (the Wii U's framing of the same scene, for a flat start);
  - in a VR session the head aims the picture, so the part is the centred one instead, the frame's: the copy is not
    moved by those terms, and the subjects' projection is narrowed about the screen's centre
    (`centred_offset`), so the subjects counted are the ones in the photo;
  - the viewfinder (frame, zoom gauge, count, prompts) is drawn at `vr.pictoBoxSize` of its size, centred (default
    0.70, 0.5 to 1; F1 and the launcher: "Picto Box viewfinder size"): at full size its zoom gauge was outside what
    the lenses show (seen in the headset). The photo's part and the subject check shrink by the same factor
    (`picto_frame_scale`), so a photo stays what is inside the frame;
    outside the viewfinder's area the eye pictures are filled black, as around the telescope's eyepiece
    (`set_eyepiece`), so the frame's black reaches the edges of the view;
    the viewfinder is a layout in the UI layer, a quad over the field both eyes share, while each eye's picture
    covers its own field, off-centre across (each eye sees more to its own side): the black's area and the
    photo's part are placed per eye in the eye's picture (`vr_vulkan.cpp` `eye_part`, `aspect_picto.h`
    `to_eye_picture` with the left eye's field, whose picture the capture reads). Centred, the black left the 3D
    window showing at the sides (seen in the headset), and the photo would have sat off to one side of the frame;
  - before, the photo was the whole window squeezed into the 800x450 picture, part of it outside the frame, and
    subjects anywhere in the window counted;
  - outside VR #303 is upstream's, unchanged. `WWHD_PICTO_FULL_PICTURE=1` keeps the earlier behaviour for both;
    `WWHD_PICTO_LOG=1` logs each copy, the projections and the subjects found.
- **Leaving** is the game's (B, a cutscene, a menu, a scene change): when `PICTO_BOX_AIM` clears, the projection, the
  layouts and the gates are normal play's again.
- **The log** says `[vr] Picto Box view begins` / `ends`, and the magnification as it changes (`[vr] Picto Box: 2.3x ...`).

## Tested (desktop, no headset)

Scripted: the item placed on X with `WWHD_VR_TEST_KEY=1650:08:0:26` (the Deluxe Picto Box; `23` the standard one),
then X to aim, the left stick held up, A, A again, B.
- **Zoom:** the gauge went from x1 to x9 and the window magnified, with both items.
- **Pictures:** A took one ("Save this pictograph?"), and A on Yes saved it: the count went from 0/12 to 1/12, with
  both items. B on the question returned to the viewfinder without saving (0/12).
- **Leaving:** B returned to normal play, with the HUD and Link holding the Picto Box.
- **Not in VR:** unchanged, as the game draws it. The log says `Picto Box view begins (not in VR)` and nothing else
  changes outside a VR session.
- `vr_test` (`test_zoom_magnification`) and the whole suite pass.

## Still to check in the headset

- The window's view at x1 and zoomed: true scale, then magnified, and comfortable while turning the head.
- **The frame against the window.** The eye picture is taller than 16:9:
  - the layout is fitted to its width, centred, so the frame lines up with the window across;
  - but the window is a share of the whole height, so the frame's top and bottom bars may overlap it a little.
  - What a photograph keeps is the frame's centred 16:9 part (#303 above): check that a photo matches what the frame
    showed, top and bottom included, and that a subject at the frame's edge counts as it looks (`WWHD_PICTO_LOG=1`).
- That the viewfinder's overlay shows (zoom gauge, count) and the button prompts' place.
- Both items' modes (the Deluxe Picto Box's X Switch), the album (ZR), and leaving through a cutscene or a menu.

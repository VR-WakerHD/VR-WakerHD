# VR telescope

## The game's telescope (decomp: `d_a_player_main_02.cpp` `procScope_init`, `d_camera_subject.cpp` `subjectCamera`)

- **Using it.** Using the telescope puts Link into `procScope` and sets the player status `TELESCOPE_LOOK`
  (`play +0x5CD8`, bit `0x200000`). The camera switches to its first-person "subject" mode at his eye.
- **Zoom.** It is a level from 0 to 1 that the **left stick's Y** moves while it is held: up zooms in, down zooms
  out, and it stops when the stick is released. The level is bounded at both ends. From it the game sets a zoom
  scale of 1 to 9 (`setComZoomScale`) and narrows the camera's field of view (`dCamMath::zoomFovy`). Entering the
  telescope already narrows it to 0.444 of the normal one.
- **The picture.** The game draws its eyepiece as a TV layout over the picture: black around a round view, the
  zoom gauge (×1 to ×9) and "B Back".
- **Leaving.** B, as on the GamePad.

## In VR before

- **No zoom.** The headset's field of view replaces the game camera's in the picture and the culling (`aspect.cpp`
  `vr_projection_args`), so the zoom never showed.
- **The eyepiece was small.** The telescope is not "normal play" for the game's state check, so its layouts were
  drawn at the cutscene text's size (a third). A small eyepiece sat in the middle of an unmasked view.

## Now

- **Magnification** (`vr_game.cpp` `telescope_magnification`). While `TELESCOPE_LOOK` is set, the magnification is
  `tan(normal fov / 2) / tan(the game's fov / 2)`, between 1 and 16. The normal field of view is remembered from
  normal play. The game's own zoom logic, limits and smoothing are unchanged and decide it.
- **Both eyes drawn narrower** (`aspect.cpp`). Both eyes are drawn with the headset's field of view narrowed by
  that magnification: the same projection for both eyes, each with its own eye offset, as always. The frame still
  goes to the compositor with the headset's real field of view and the pose it was drawn with, so the narrower
  picture fills the view. That is the magnified view, in stereo, at the headset's head pose. Turning the head turns
  the view immediately, as through a real telescope.
- **The eyepiece in view, black around it.** The game's mask is drawn at `vr.telescopeSize` (default 0.85, in the
  launcher's Camera and comfort tab and in F1 > VR) of the size that covers the whole eye picture. That picture is
  taller than the 16:9 layout space; the size that covers it is 1.32 on a PS VR2. Covering the picture put the black
  ring outside what the lenses show (reported in the headset), and a smaller mask alone left the scene around it.
  So when each eye is copied into the headset's image (`vr_vulkan.cpp` `record_eyes`), the image is cleared to black
  and only the mask's part of the eye picture is copied into it, centred (`vr::eyepiece`). There is no extra render
  pass.
- **Entering and leaving** are the game's. Head tracking stays on throughout, and the magnification follows the
  game's field of view, so it eases in and out with the game's own transition. When `TELESCOPE_LOOK` clears (B, a
  cutscene, a menu, a scene change), the projection and the layouts are those of normal play again. The log says
  `[vr] telescope view begins` / `ends` and the magnification as it changes (`[vr] telescope: 2.7x ...`).
- **Controls** are the game's: the left stick's Y zooms (the VR left stick goes to the game as the GamePad's left
  stick) and B leaves. Nothing new is bound, so nothing conflicts with the sword (trigger) or the menus.
- **No extra rendering or resources.** Only the projection's field of view and the layout scale change; no render
  target is made or remade.

## Tested (PS VR2, scripted: telescope on Y, the left stick held up, then B)

- `telescope view begins`, then the magnification grew as the stick zoomed in: 1.3x, 1.6x, 2.1x, 2.7x ... 10.9x.
  The game's field of view went from 60 to 6.1 degrees.
- Captures: the eyepiece covers the eye. The gauge read ×1 before zooming and ×9 at the top of the zoom, the pier
  magnified in the round view. B ended the telescope view, and the layouts went back to the gameplay HUD.
- Reported in the headset: zooming works.
- The black fill at 0.85 is built but not yet seen in the headset (the test captures are taken from the eye
  pictures before that copy).
- **Limits.** The
  game's own telescope messages (looking at something that has one) use the game camera's aim, which the head does
  not move. Still to check in the headset: comfort while zoomed and turning the head, left-handed mode.

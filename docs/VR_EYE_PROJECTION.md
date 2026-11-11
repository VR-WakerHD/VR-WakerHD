# Each eye's own projection, and the field-of-view diagnostics (2026-11-03)

## The audit: where the headset's field of view goes

Reported: a narrowed or "bubble" view (Quest 3 standalone and through VDXR). The whole path was traced in the code:

1. `xrLocateViews` gives each eye's `angleLeft/Right/Up/Down`. `vr::as_rendered` (`vr.cpp`) makes one symmetric
   field of both (`symmetric_union`, `vr_math.h`: the widest angle on each axis), widened by `fov_margin` (Android and Steam Frame - Native 5°).
2. The game's camera takes that field: `aspect.cpp` hooks both projection calls of `camera_draw` (`C_MTXPerspective`
   at `024FFD60`, `sead::PerspectiveProjection::set` at `0250007C`) and the clipper; fovy = 2·up, aspect =
   tan(right)/tan(up). The game's flat-screen FOV is not reused; the gameplay FOV scale is not applied in VR; only the
   telescope narrows it (on purpose).
3. The eyes are parallel: infinite convergence (`stereo.h` `latch`), only each eye's sideways translation from the
   runtime's eye positions (world scale 100 units/m).
4. Render targets take the field's shape (`aspect::requested`); the eye picture is copied whole into the whole
   swapchain image (blit or the SGSR1/FSR1/SGSR2 upscalers: no warp, no lens correction anywhere in the app).
5. `vr.eyeCrop` (default off: with each eye's own projection, the default, there is nothing to crop; turn it on with `WWHD_VR_EYE_PROJECTION=shared` on Quest/VDXR) submits each eye as its own part of the image with that part's exact field (`fov_crop`).
6. Depth for reprojection: the game's depth mapped from clip −1..1 to 0..1 is the same curve OpenXR assumes; near and
   far from the scene's projection; a cropped eye's depth and motion use its colour's part.

No step distorts or narrows the picture: the field drawn, copied and submitted agree. The cost of the symmetric field
is pixels: a quarter of each eye's image is the other eye's side (PS VR2: 2302 of 3040 columns submitted; Quest 3:
about 72% of the image), and on PS VR2 the picture is squeezed across (about 30% fewer pixels per degree across than
down).

## Each eye's own projection (on by default; `WWHD_VR_EYE_PROJECTION=shared` turns it off)

Each eye is drawn with its own frustum, inside the symmetric one the camera builds (`stereo_projection.h`
`EyeFrustum`): its picture is the symmetric one's normalized device coordinates mapped by x' = sx·x − ox,
y' = sy·y − oy. For the eye's edges' tangents l, r, u, d in units of the symmetric half-field: sx = 2/(r − l),
ox = (r + l)/(r − l), sy = 2/(u − d), oy = (u + d)/(u − d) (`vr_math.h` `eye_frustum`).
- **Applied where the eye shift is** (`project_eye`, `project_uniform_block`, `apply_eye_rows`): the camera
  projection's x row scaled and offset, and its y row. A remapped layout copies a matrix's rows on their own: the y
  row's place is found by its source position (`EyeRows::Row::yOffset`; `shaders.cpp`, the batched pieces in
  `draw.cpp`), and the rows are patched after all entries are copied. The identity frustum leaves every value
  exactly as before.
- **Relative to the camera's matrix**, so the telescope's magnification still applies.
- **Culling** keeps the symmetric field (it holds both eyes).
- **The eye pictures** take the eyes' own shape (`vr::eye_image_aspect`, the mean of both eyes'; each eye's frustum
  fills it). `as_rendered` gives each eye its own field, so the submitted part is the whole image.
- **Latched per frame** like the eye offsets (`stereo::set_eye_frustums`, `frame.frustum`).

### Not yet done (before it can be on by default)
- **The HUD and on-screen text** (layouts: their own projection, not shifted per eye) stay where they are in each
  eye's image, which is no longer the same direction in both eyes. They need the eye's frustum without the eye's
  translation.
- **The telescope's eyepiece** is centred in the image (now not the eye's forward direction).
- **Effects placed on the CPU** with the game's camera (if any: lens flares, the sun) to be checked in the headset.
- A setting in the launcher and F1 (and a field-of-view reduction slider: 100% down to about 80%, drawn and submitted
  smaller, for sharpness or speed).
- Headset tests (PS VR2, Quest 3).

## Diagnostics (`WWHD_VR_FOV_DIAG=1`)

Every 10 s (`[vr fov]` lines, `vr_vulkan.cpp`): the mode, eye crop, margin, recommended and actual image sizes, the eye
picture and render-target scale; per eye the runtime's field (degrees and radians, extents, eye position), the field
and part submitted (pixels, share discarded, pixels per degree at the centre across and down, the frustum), and the
projection its draws actually uploaded (found in the uniform bytes after the eye shift: `fov_diag.h`), with the field
that projection puts in the submitted part beside the field submitted: **MATCHES** (within 0.05°) or **DOES NOT
MATCH**. A mismatch is a picture magnified or shrunk in the headset.

## Tests (desktop stereo, unattended, fixed seed, frame 4200, Link walking and the camera turning)
- **Default path unchanged:** both eyes' pictures byte-identical to the build before (motion targets vary run to run
  in the build before as well: below).
- **Correctness:** `WWHD_TEST_EYE_FRUSTUM=sx,ox,sy,oy` (test aid, `stereo.h`) draws both desktop eyes with a frustum;
  the picture against the symmetric one mapped the same way: 1.25, 0.25, 1, 0 (left eye) mean difference 2.3 of 255;
  1.2, −0.1, 1.25, 0.2 (both eyes) 2.9 and 3.0. What differs: the HUD (above), resampled edges, and bloom (its kernel
  is in pixels, and the test stretches them; the real mode keeps them square). No shadow, light or geometry moved.
- **Found on the way:** the motion pass's targets differ between two runs of the same build (two runs of the build
  before this work also gave two different results); not caused by these changes.

## The game's UI as its own layer

With each eye's own projection the game's UI (layouts: HUD, menus, text) is not corrected per eye: the game draws it
through a dozen kinds of matrices, and patching the projection reached about two thirds of it (the rest showed
twice). It is drawn once instead, by eye 0, into an image of its own (`draw.cpp` `ui_layer`: the eye picture's height,
the symmetric field's shape, square pixels over that field), and shown as a head-locked OpenXR quad layer
(`openxr_session.cpp` `kUi`), as UEVR shows a game's UI.
- **Drawn premultiplied** over a transparent image: the game's colour blend is kept, the alpha is written as coverage
  ("over"), and the layer is blended by its alpha (`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`).
- **Eye 1's copy is left out**; UI draws stay on the render thread, outside batched runs and the continued-draw
  shortcut, in order with the image's clear.
- **The quad** covers the symmetric field at 3.5 m (`vr_vulkan.cpp` `kUiDistance`; 1.5 and 2.5 m felt too close), 1 m behind the GamePad panel
  while that is shown. Its swapchain is made once, at the image's size once that has held for 30 frames (copied 1:1;
  not destroyed and made again mid-session, where SteamVR hung).
  It is made under the graphics queue's lock, as every quad's swapchain made mid-session now is (`vr_vulkan.cpp`
  `ensure_swapchain`): the runtime may use the queue in xrCreateSwapchain while the end-frame thread and the record
  worker submit to it. A Quest 3 through VDXR on an RTX 5080 crashed in NVIDIA's driver (`nvoglv64.dll`, a read at
  0x130) the frame the UI layer's swapchain was made (`crash-20261103-194249.log`).
  That lock did not stop it: the same crash came again with it (`crash-20261103-201905.log`, build `bb2fd65-dirty`).
  Symbolized, it is `vkCmdEndRendering` replayed by the record worker (`PodCommands::replay`) with no rendering begun on
  that command buffer, soon after the two-eye targets were re-paired after a rescale: the Quest's Adreno crash of
  2026-10-31 (docs/VR_ANDROID.md). Its guard (`loader.cpp`: such an end left out and logged) is now on every platform.
  Where the unpaired end comes from is still not found; `[vulkan] an end of rendering with none begun` in a log shows it.
- **Clipping**: the shrunk-screen clip (`vr::layout_scale`) is not moved by an eye's frustum in the UI image and keeps
  16:9 there; `aspect_panes.h` `clip` leaves an axis narrower than 16:9 unclipped (its inset wrapped to a huge one
  and emptied the scissor: the file-select menu's panels and text were never drawn).
- **Diagnostics**: `WWHD_DUMP_FRAMES` also writes `frame_<n>_ui.png`; `WWHD_VR_UI_TRACE=<frame>` logs each UI draw of
  that frame (target, viewport, scissor); `[vr ui]` lines log each UI pipeline's blend and the targets UI is taken from.

## Point lights (2026-11-04)

A Quest 3 player saw the Forsaken Fortress's searchlight pools in a different place in each eye, apart from their
beams, and torchlight in one eye only. The game lights torches and searchlights in agl's light pre-pass
(`gfx::ProgramKind::PointLight`: VS 44390500 / PS 44390C00 for round lights, VS 44461600 / PS 44461F00 for spotlights),
which rebuilds each pixel's view-space position from the depth copy as the sun shadow does. The light volumes were
already drawn for each eye (their projection is found as a camera projection), but the ray and the light's place were
the centre camera's, so each eye lit the wrong spot.

`stereo::point_light_uniforms` (`stereo_projection.h`) adjusts both stages per eye, as `shadow_uniforms` does for the sun:
- VS `remapped[5]`: ray.x = x_ndc * -[5].x - [5].z, ray.y = -y_ndc * -[5].y - [5].w (y flipped, unlike the sun's ray).
  The eye's frustum: x as the sun's; y with the offset's sign reversed for the flip. The eye's displacement:
  [5].z - eye / convergence.
- PS `remapped[1].xyz`, the light's view-space place: [1].x - eye, into the displaced eye's space.

The two passes are never replayed from eye 0 or drawn as multiview (the sun shadow's rule): a few draws a frame, drawn
once per eye. Layouts read from the shaders' GLSL dumped in the fortress (`WWHD_VK_DUMP_GLSL=1`). Built for Windows;
not yet seen in a headset.

### The light's place no longer moved per eye by default (2026-11-05)

A Quest 3 player, with the change above: both torches lit in both eyes now, but "shining in different directions with
each eye". The PS shift (`[1].x - eye`) moved the light's place by the eye's offset only, 3 game units, a few
centimetres against a light hundreds of units across, so it could not be seen where `[1]` is the place; but the round
lights' layout was taken for the spotlights' program too (PS 44461F00, a program of its own), and were `[1]` a direction
there, taking 3 units off its x turns the light one way in the left eye and the other in the right. It is left out;
each eye's ray (the VS, what puts each eye's light where it falls) is kept. `WWHD_VK_POINT_LIGHT_EYES` chooses:
- `ray` (default): each eye's ray only;
- `full`: the ray and the light's place moved, as made on 2026-11-04;
- `0`: no per-eye correction, as before it: the passes shared by the eyes (replayed, or multiview), which also tells
  whether drawing them per eye is what kept the Quest's scene from multiview in the same log (`[vulkan multiview]`).
The log's `[vulkan stereo] point lights` line says which. `stereo_test` checks both stages. Not yet seen in a headset.

### Not drawn on Android by default (2026-11-06)

The Quest 3 player's torches and searchlights still looked wrong with each eye's ray (`ray`); the test build that left the
point-light passes out (`WWHD_VK_POINT_LIGHT_EYES=none`: their buffer stays as cleared, no torch or searchlight light)
was the one that looked right, so it is Android's default (Quest, Steam Frame - Lepton). PC VR keeps `ray`.
`=0`, `=ray` or `=full` in env.txt draws them again. The log says `point lights (torches, searchlights): not drawn`.

### The light volume moved for each eye too (2026-11-07)

PC VR on a PS VR2 with `ray`: indoor torchlight "going in opposite directions"; with `0`, worse (the light looked cut off
by a shadow). VS 44390500's GLSL (`WWHD_VK_DUMP_GLSL=1`) and the logged uniforms showed why: the light volume is drawn with
a combined matrix in `remapped[1..4]` (w = [1], y = [2], x = [3], z = [4]), not a camera projection, so nothing moved it
for the eye: both eyes drew the lit shape where the centre camera's symmetric picture has it, while each eye's own field
(PS VR2: sx 1.32, ox -0.32 / +0.32) put the ray's light elsewhere, the shape cut off on opposite sides in each eye.
`point_light_uniforms` now also moves those rows as `project_eye` moves a projection (x' = sx * x - ox * w, the eye's
displacement -eye * sx * p0; y' = sy * y - oy * w), taking p0 = 1 / [5].x and p5 = -1 / [5].y from the ray, and only
where the rows' lengths agree with them (within 2%: another layout is left as it was). Seen right in the headset. The
first 12 passes' uniforms per eye are logged (`[vulkan stereo] point light VS ...`).

# Less GPU work on standalone headsets: held clears and foveated rendering (2026-11-11)

A Quest 3 player's log at Headset resolution 0.70x and 90 Hz: 20-26 fps in play, the GPU 94-100 % busy at 35-69 ms
a frame, worst in Outset's grass. Multiview already draws ~700 draws a frame once for both eyes; the cost left is the
GPU's per pixel work and its memory traffic. Two changes cut them. Neither is tried in a headset yet: check the log
lines below.

## Held clears (`WWHD_VK_FOLD_CLEARS`)

On a tiled GPU (Quest, Steam Frame) the GPU draws each pass tile by tile in its own fast memory. A clear recorded on
its own (`vkCmdClearColorImage` / `vkCmdClearDepthStencilImage`) wrote the whole cleared image out to memory, and the
pass that drew into it next loaded it all back into the tiles (`loadOp LOAD`): two full-image memory trips for
nothing. Now a clear of a whole single-layer surface is held (`Surface::pendingClear`, `surfaces.cpp` "held clears"),
and the next pass that draws into all of it starts with `loadOp CLEAR` (`draw.cpp`, the pass begin): no clear of its
own, no load.

- Anything else that uses the surface first (sampling, a copy, a read-back, a pass over part of it) records the clear
  as before: `transition_image` flushes a held clear (`flush_pending_clear`), except the folding pass's own switch to
  an attachment layout (`FoldClearScope`).
- A two-eye pass folds only when both eyes' layers hold the same clear. A single-layer pass into one eye's layer of a
  two-eye image never folds: that clear keeps its wait before it (`wait_before_layer_clear`, the black squares' fix).
- A depth clear folds only of all its aspects (depth and stencil where the format has stencil). Not a volume, an array
  or part of one, not with render-pass objects (`R.renderPasses`), not on the second drawing thread, not a depth
  clear 60 fps (generated) keeps.
- Mobile GPU builds by default; `WWHD_VK_FOLD_CLEARS=0` turns it off, `=1` turns it on elsewhere.
- Log: `[vulkan] held clears on ...` at the start, then every 600 frames `[vulkan] held clears per frame: n held,
  n folded into a pass (no clear, no load), n recorded on their own`.

## Foveated rendering (`vr.foveation`)

The edges of each eye, where the lenses show less, are shaded in blocks of 2x2 pixels (High: 4x4 at the far corners).
Shapes and their edges are still drawn pixel by pixel, each with its own depth; only the colour inside a block is worked
out once. The runtime's foveation (`XR_FB_foveation`) only covers drawing into its swapchain images, and the game draws
offscreen and copies into them, so the game attaches its own shading rate map (`VK_KHR_fragment_shading_rate` by
attachment) to its eye-sized two-eye (multiview) passes (`foveation.cpp`).

**First try, replaced (same day):** a fragment density map (`VK_EXT_fragment_density_map`). A Quest 3 player's log with
it at High: no GPU time saved (100 % busy, 44-58 ms a frame at 0.70x), the clouds in coarse squares and black patches at
the edges of the view. Three causes: the map's texels were the device's largest, 256x256 pixels (a 5x5 map for the eye
picture: quarter-detail squares 256 pixels wide); every two-eye pass was foveated, the game's small buffers too (a
294x308 bloom/sky buffer wholly at a quarter); and on Quest a fragment density map puts the driver in its tiled mode,
which AstroQuest's README (github.com/evertec82/AstroQuest, a shadPS4 Quest 3 port that also draws the game offscreen)
measured as halving its frame rate. AstroQuest uses shading rate maps instead, measured on a Quest 3: 17.0 ms of the
GPU a frame without, 15.1 / 14.3 / 12.8 ms at its levels 1-3. This follows it.

- **Setting:** `vr.foveation` = `off`, `low`, `medium`, `high` (VR launcher Graphics, F1 > VR: "Foveated rendering";
  applies at once). **Low on a Quest** by default (the user's choice, 2026-11-12: a Quest 3S player saw the picture pixelated, Medium shading 69% of each eye in 2x2 blocks with the GPU half idle at 30 fps; settings format 27 moves a saved Medium to Low), **Medium on Steam Frame - Native** (the user's choice, 2026-11-11; High before, settings format 22 moves a saved High to the default), off elsewhere.
  `WWHD_VK_FOVEATION_LEVEL=0..3` overrides it.
- **Levels** (AstroQuest's rings, as a share of the way from the middle of the eye to its edge, `sqrt(x^2 + y^2)`):
  low 2x2 from 80 % (about half the picture, the corners are a lot of it); medium 2x2 from 62 %; high 2x2 from 50 %,
  4x4 from 85 % (where the device offers 4x4, else 2x2).
- **Device support** (`backend.cpp`): `attachmentFragmentShadingRate` and `pipelineFragmentShadingRate`, `R8_UINT`
  usable as a shading rate attachment, multiview, dynamic rendering, Vulkan 1.2. Enabled where supported in mobile GPU
  builds; `WWHD_VK_FOVEATION=0` never enables it, `=1` elsewhere too. Log: `[vulkan foveation] shading rate maps:
  enabled (... texel ..., using 16; rates ...)` or why not. Shaders writing depth or the sample mask are shaded per pixel
  where the device says so (its `with depth writes` / `sample mask` values).
- **Pipelines:** every multiview twin is built with `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`
  (needed with a map, VUID-vkCmdDraw-imageView-06183; allowed without one) and a shading rate state of 1x1 whose
  combiner takes the map's rate (KEEP, REPLACE): without that state the map would be ignored.
- **Maps:** one per render size and level, 16 pixels a texel (or the nearest the device allows), one layer for both
  views. Made and uploaded on the render thread and used by any thread from the next frame on, when the commands that
  filled it reached the GPU's queue first; meanwhile a pass of that size is shaded in full. The recorded command
  streams carry the map (`pod_commands.cpp`, `deferred_commands.cpp`: a pass's only `pNext`). Log:
  `[vulkan foveation] map for WxH (level): ... texels of 16x16 pixels, n % of it shaded in blocks`.
- **Only the eye picture's size** (`vr::picture_size`, within 1/32): the game's smaller buffers (bloom, blur, the sky's)
  are shaded in full, a coarse block in them would be magnified on screen. A pass drawn one eye at a time is not
  foveated either. The map is centred on the eye picture's middle (one map for both eyes).

## To check in a headset

1. The log's `[vulkan foveation] shading rate maps: enabled` and one `map for ...` line, at the eye picture's size, and `held clears per
   frame` with most clears folded.
2. The `GPU busy` and `frame pacing` lines against the earlier log (same place, same Headset resolution).
3. The picture: no change looking straight ahead; a little softness at the edges at High; no blocks on the clouds. Black
   squares or flicker on the sky and water would be the held clears (`WWHD_VK_FOLD_CLEARS=0` to tell).

**Map layers by driver (2026-11-12).** Two identical layers (one per view) are needed on Mesa's Turnip (the Steam Frame,
which reads the map as layered). On Qualcomm's own driver (Quest 3) two layers crashed: a null read inside
`vkCmdDrawIndexed` after some minutes of play, in three crash logs on the builds with two layers, while the build before
(one layer) ran without. `foveation.cpp map_layers`: two on Turnip, one elsewhere; `WWHD_VK_FOVEATION_LAYERS=1/2`
chooses. Log: `[vulkan foveation] shading rate maps of N layer(s)`.

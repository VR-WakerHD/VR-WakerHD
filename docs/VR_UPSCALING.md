# VR upscaling on standalone headsets (Quest 3, Steam Frame)

Each eye is drawn at the internal resolution (on a Quest a fixed 0.75x of it: `dynres.cpp` `fixed_factor`, about
960x1000 an eye) and scaled to the headset's swapchain image (Quest 3: 1680x1760). That scaling was a linear
`vkCmdBlitImage` (`vr_vulkan.cpp` `record_eyes`). This document covers what replaces it, per device, and what was
investigated and not built (2026-11-01).

## What is built

| | Quest 3 | Steam Frame | PC VR, NVIDIA RTX | PC VR, AMD / Intel |
|---|---|---|---|---|
| **Quest Super Resolution** (Meta's compositor) | **auto** | not offered (SteamVR) | not asked for | not asked for |
| **DLSS 4** (DLAA; docs/VR_DLSS.md) | no | no | **auto** (needs the DLSS library) | no |
| **SGSR1** (app-side pass) | by choice | **auto** | by choice; auto without DLSS | **auto** (when upscaled) |
| Plain blit | `vr.upscaler=off` | `off`, or no upscale | `off` | `off`, or no upscale |

Auto picks the best the device offers, by runtime checks (the OpenXR runtime's extensions, the NGX probe of the GPU,
driver and DLSS library), never by GPU model. Never two upscalers at once: each sharpens (`vr/vr_upscale.h`
`choose`).

### DLSS 4 (PC, NVIDIA RTX)
- The renderer's own DLSS Super Resolution / DLAA (docs/VR_DLSS.md), in the scene with motion vectors. Chosen at the
  start, in `gfx/vulkan/ngx_vk.cpp` `probe`: for a VR start whose `vr.upscaler` is `auto` or `dlss` and whose settings
  name no DLSS mode (`dlss=` in settings.ini, or the launcher's Graphics tab), DLAA is turned on when the probe finds
  it offered. A mode set there wins (Quality/Balanced/Performance draw the scene smaller). So a change to or from DLSS
  applies at the next start. With DLSS in use the eye copy is the blit (no second upscaler).
- Needs `nvngx_dlss.dll` (Windows) or `libnvidia-ngx-dlss.so.310.9.16` (Linux) next to the game; without it the
  probe logs `[ngx] VR upscaling: DLSS not offered here ...` and auto takes SGSR1 or the blit. Checked on the RTX 5080
  (Linux, driver 615.78, no library): Super Resolution `0xBAD00004`, the fallback logged, `active: Off`.
- `motion_wanted` (the motion-vector work) follows the probe's decision once it ran, so the auto choice also starts
  the motion pass, and only where DLSS runs.

### Quest Super Resolution
- Meta's compositor (Timewarp) upscales and sharpens the eye images with an SGSR-based filter tuned with Qualcomm.
  Asked for with `XR_FB_composition_layer_settings`: an `XrCompositionLayerSettingsFB` with
  `XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB` chained onto the projection layer
  (`vr/openxr_session.cpp` `end_frame`). Meta's own wording: "Super Resolution is controlled via the quality
  sharpening option in XR_FB_composition_layer_settings" (replaced the old sharpening in OS v55).
- Costs the compositor some GPU, nothing of the app's frame. The settings are hints the compositor may ignore.
- Enabled only on Android (a PC runtime offering the extension keeps its own filtering as before). The log says
  `[vr] Quest Super Resolution (XR_FB_composition_layer_settings): offered` at start.
- Sources: [Meta blog](https://developers.meta.com/horizon/blog/vr-image-quality-meta-quest-super-resolution/),
  [composition layer filtering](https://developers.meta.com/horizon/documentation/native/android/mobile-openxr-composition-layer-filtering),
  [XrCompositionLayerSettingsFlagBitsFB](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSettingsFlagBitsFB.html).

### The picture at its own size (Quest Super Resolution, 2026-11-08)
- Before: the eye picture (924x1000 at the Quest's default 0.75x) was blitted, stretched bilinearly, to the whole
  1680x1760 eye image, and Super Resolution then only sharpened an already softened picture of about the display's
  size: it had nothing left to upscale.
- Now (`gfx/vulkan/vr_vulkan.cpp` `record_eyes`): with Super Resolution chosen, the picture is copied 1:1 into the
  image's top-left corner (its last column and row once more beyond it, for the filter's edge) and only that part is
  submitted (`EyeView::rectWidth/rectHeight`, the projection view's `imageRect`, `vr/openxr_session.cpp`). The
  compositor's SGSR-based filter does the whole upscale to the display in its one resample through the lenses.
  An eye crop (`vr.eyeCrop`) is then a part of that part.
- Not with the telescope's eyepiece, the runtime's frame synthesis (its motion and depth images are the image's size),
  or a picture larger than the image (above about 1.8x of the 0.75x default, 1680x1760): those stretch as before.
- The log: `[vr] left eye: its 924x1000 picture submitted at its own size (of the 1680x1760 image), ...`.
  `WWHD_VR_NATIVE_PICTURE=0` stretches it as before.
- Cost: none for the game; the blit writes about a third of the pixels it did. Not yet seen in a headset.

### SGSR1 (Snapdragon Game Super Resolution 1)
- Qualcomm's single-pass spatial upscaler: a 12-tap Lanczos-like, edge-adaptive filter with built-in sharpening, 4
  `textureGather`s, flat areas left as bilinear. BSD-3-Clause; the shader and its notice are in
  `gfx/vulkan/vr_upscale.cpp` (from [snapdragon-gsr](https://github.com/SnapdragonStudios/snapdragon-gsr),
  `sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag`, edge direction on).
- Drawn per eye straight into the acquired swapchain image in place of the blit: no extra image, no copy. Edge
  threshold 4/255 (Qualcomm's suggestion for VR), sharpness `vr.upscaleSharpness` (1..2, default 2; 1.5 on the Steam Frame, where 2 may halo the game's dark outlines). Into an sRGB
  swapchain the filter runs on approximately perceptual values (square root of the linear samples, squared back).
- Qualcomm's numbers: about 0.3 ms on Snapdragon 8 Gen 2 for 1240x576 to 2400x1600 (bilinear 0.16 ms).
- Not with the telescope's eyepiece (that copies part of the picture into part of the image): blitted then.
- Changes for glslang: GLSL 450, constants as push constants, ES-only `highp vec2(...)` constructors removed.

### SGSR2 (Snapdragon Game Super Resolution 2, temporal; by choice only)
- Qualcomm's temporal upscaler, its 2-pass fragment variant (the one Qualcomm recommends for mobile and VR/XR), BSD-3;
  the shaders and their notice in `gfx/vulkan/vr_sgsr2.cpp`. Per eye: a convert pass (motion and a depth-clip weight
  for disocclusions, at the render size) and an upscale pass (a 5-tap Lanczos upsample blended with the history,
  which is reprojected by the motion and clamped to the neighbourhood's colour range against ghosting), into the eye's
  history image (RGBA16F, the headset image's size; two, alternating), then copied into the swapchain image.
- Inputs: the eye's picture, the scene depth and the **motion pass's** motion vectors (full 6DoF, per object, every
  pixel including the sky's background draws; docs/VR_DLSS.md), turned into SGSR2's current-minus-previous NDC (the
  non-y-up Vulkan path). So the motion pass runs when `vr.upscaler=sgsr2` (set at VR start with frame synthesis's
  flag): a change to SGSR2 applies at the next start, and it shares the motion pass with frame synthesis.
- Its own history per eye; a new history on a view cut or a new scene (the motion pass's resets), on new sizes, and
  after a frame without SGSR2. An eye without motion that frame (or sizes that differ from the scene's) takes SGSR1.
- Parameters as Qualcomm's Vulkan sample: `scaleRatio = (output w / render w, min(20, (area ratio)^3))`,
  `minLerpContribution` 0 (a head is never still), `cameraFovAngleHor` 1.2.
- **No jitter yet:** the scene is not shifted by sub-pixel offsets for it. The sign of SGSR2's `jitterOffset` against
  the scene's viewport jitter (motion_pass's, used by DLSS) needs a headset check; without jitter SGSR2 still
  upscales with its history and steadies edges, but recovers less real detail. Next step once checked.
- Costs: the motion pass (a second draw of the scene's geometry: CPU) and about 1 ms per eye on Snapdragon 8 Gen 3
  (Qualcomm's 2-pass fragment numbers: 0.9 to 1.1 ms for 1260x2800 output).
- Limits: draws without motion (batched draws, transparent effects, particles) can trail or smear; the HUD is in the
  eye picture and goes through it too.

### FSR 1 (AMD FidelityFX Super Resolution 1; by choice only)
- AMD's spatial upscaler, MIT: EASU (edge-adaptive spatial upsampling: a 12-tap Lanczos-like filter along the local
  edge direction, deringed against the 4 nearest) into an intermediate RGBA16F image the headset image's size, then
  RCAS (robust contrast-adaptive sharpening: a 5-tap cross whose negative lobe is limited so it never clips) into the
  swapchain image. Ported from `ffx_fsr1.h` (FidelityFX SDK, as in GPUOpen-Effects/FidelityFX-FSR2) to standalone GLSL
  450, the 32-bit float paths, notice kept (`gfx/vulkan/vr_fsr1.cpp`).
- EASU's and RCAS's constants are computed on the CPU as `ffxFsrPopulateEasuConstants` and `FsrRcasCon` do; checked
  against AMD's own functions compiled from the header (equal for the Quest's 960x1000 to 1680x1760, the Frame's 1280x1333
  to 1728x1728, and a small case).
- Into an sRGB swapchain the filters run on approximately perceptual values (FSR 1 expects perceptual input), as SGSR1.
- `vr.fsrSharpness`: RCAS's reduction in stops, 0 (sharpest) to 2; 0.2 by default (AMD's samples).
- One upscaler a frame: FSR 1 alone when chosen (an eye it cannot draw is blitted).
- **No FSR frame generation:** none of AMD's frame interpolation or swapchain replacement is in the game; FSR here is
  upscaling only. VR's in-between frames come from the headset's runtime (docs/VR_FRAME_SYNTHESIS.md).

### FSR 3.1 (investigated, not built)
AMD FSR SDK 2.4.0 (2026-10-28) carries FSR 3.1.6 with Vulkan 1.2. Its upscaler source (C++ context and HLSL shader
headers) is MIT, but the Vulkan backend is not among the MIT files and the rest of SDK 2.x is binary-only (signed
Windows DLLs, no reverse engineering): no official FSR 3.1 runtime for Linux, Android or arm64, and its shaders are
HLSL for DXC (not the renderer's glslang). Using it means a Vulkan backend of our own and offline HLSL to SPIR-V for
every permutation. AMD lists no Android/Qualcomm support. FSR 2.2 (MIT, FidelityFX-FSR2) has GLSL Vulkan passes and
is the portable temporal alternative. Not started: FSR 1 was asked for first.

### Settings
- `vr.upscaler`: `auto` (the default on standalone headsets; on PC VR, Windows and Linux, the default is `off`, the user's choice of 2026-11-03), `off`, `sgsr1`, `superResolution`, `dlss`, and by choice only `sgsr2` and
  `fsr1`; `vr.upscaleSharpness` 1..2 (SGSR1); `vr.fsrSharpness` 0..2 stops (FSR 1).
- In the game: F1 > VR > "VR upscaling" (with an "In use:" line), and the VR launcher's camera section.
- The log: `[vr] upscaling: <choice> (eye picture WxH, headset image WxH, sharpness S)` whenever it changes.

### Tests
- `vr_test` `test_upscaling`: the choice per device (Quest, Frame, RTX PC, AMD/Intel PC), fallbacks where a choice
  is not offered, never two, SGSR2 by choice only (SGSR1 without motion vectors), the settings saved and read back,
  the sharpness clamped. The SGSR2 shaders compiled with glslang and validated with spirv-val (not run on a GPU here).
- `wwhd --renderer-smoke` `vr_upscale::self_test`: the pass drawn on the GPU into small UNORM and sRGB images and read
  back; a flat picture must come out unchanged (within 2/255). On the RTX 5080: within 1/255 in both formats.
- Not done here: any headset run, image quality comparison or GPU timing on a Quest or a Frame.

## Investigated, not built

| Technology | Why not (now) |
|---|---|
| **FSR1** (MIT, EASU + RCAS) | Two passes where SGSR1 is one and Adreno-tuned; a desktop fallback only. |
| **FSR2 / FSR3** | Six-pass compute with fp16/wave ops (FSR2), HLSL-only for DXC (FSR3): too heavy for mobile and not in the runtime GLSL pipeline. |
| **FSR4** | Not open (signed AMD binaries, ML on SM 6.6); never on Adreno. DLSS: PC NVIDIA only (above). |
| **Fixed foveation (Quest), eye-tracked foveation (Frame)** | `XR_FB_foveation_vulkan` gives a fragment density map only for passes that render into the swapchain image. The game draws offscreen and copies, so foveation means attaching density maps (`VK_EXT_fragment_density_map`) to the scene's own passes: every scene pipeline needs a density-map variant for dynamic rendering, multiview needs per-layer maps (`VK_VALVE_fragment_density_map_layered` on Turnip), and the gaze would come from `XR_EXT_eye_gaze_interaction` / `XR_META_foveation_eye_tracked` (both on Valve's list). It saves GPU fragment work only, and the headsets are CPU-bound. A project of its own. |
| **Runtime dynamic resolution** (`XR_META_recommended_layer_resolution`) | The game's own dynamic resolution (`dynres`) stays off on a Quest: changes of scale preceded the Adreno crashes; the Quest draws at a fixed 0.75x. |

## Device facts used (2026-11-01)
- **Quest 3:** Snapdragon XR2 Gen 2, Adreno 740, Oculus OpenXR runtime; default eye image 1680x1760.
- **Steam Frame:** Snapdragon 8 Gen 3, Adreno 750, SteamOS with Mesa Turnip (Vulkan 1.4 on a7xx; multiview, fragment
  density map and its offset, fragment shading rate; not FDM2), SteamVR's OpenXR runtime (eye gaze, FB/META foveation
  extensions on Valve's list); default eye resolution 1728x1728. Valve's performance overlay fails titles that drop
  below a resolution threshold (dynamic resolution counted at its lowest). Sources:
  [Steamworks custom engines](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom),
  [vrpreferences](https://partner.steamgames.com/doc/steamhardware/steamframe/vrpreferences),
  [Mesa features](https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/features.txt).

## Headset tests still needed
1. Quest 3: the log's `Quest Super Resolution ... offered` and `upscaling: Quest Super Resolution (auto)`; the picture
   sharper than before, no shimmer on fine detail (the sea, grass, distant rails); then `vr.upscaler=sgsr1` against it.
2. Steam Frame: `upscaling: SGSR1 (auto)`; edges sharper than the blit; no halos at high contrast (HUD text is drawn
   before, so it is sharpened too); sharpness 1 vs 2.
3. Both: frame times with each choice against `vr.upscaler=off` (logs).

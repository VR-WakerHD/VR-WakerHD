# Quest 3 graphics optimization (2026-11-03)

Graphics pipeline, textures, foveation, dynamic resolution and quality settings for the native Quest 3 build. Nothing
here was measured on a Quest 3 for this report; marks as in [QUEST3_PERFORMANCE_AUDIT.md](QUEST3_PERFORMANCE_AUDIT.md).

## 1. The pipeline as it is on Quest

GX2 on Vulkan: the game's own passes (shadow map, the sun's light atlas, the scene, water, effects, bloom and other
post passes, the HUD) are drawn by the emulated Wii U GPU into offscreen render targets at the internal resolution
times 0.75; the eye picture is then upscaled into the swapchain image (blit, or SGSR1 by choice; Quest Super
Resolution sharpens in the compositor by default) [code: `vr_vulkan.cpp` `record_eyes`, docs/VR_UPSCALING.md]. In the
per-eye projection mode (default) the game's UI goes to a head-locked quad layer, and the wrist HUD, item wheel and
laser are quad layers too [code].

The GPU was 50-60 % busy in the CPU-bound logs [log]: GPU work is not the first limit on Quest.

## 2. Wind Waker's look: what is cheap and what is not

| Element | How it is drawn [code/game] | Quest guidance |
|---|---|---|
| Cel shading, flat colour, gradients | the game's own shaders, translated | exact: never change shaders for speed (project rule: graphics exact) |
| Dark outlines | the game's own geometry and passes | keep; their edges shimmer at low resolution: resolution and sharpening matter more than AA here |
| Water | the game's sea grid and its shaders, reflections in the game's passes | keep; a per-preset sea draw distance is the game's own LOD, not ours to change |
| Shadows | the game's shadow map: 0.75x (768x768) on standalone builds | keep 0.75x; 0.5x only in a Performance preset after a picture check |
| Bloom, AO, post | full-screen per eye | candidates for half resolution on Quest, **after** a GPU timestamp shows they matter |
| Text, HUD | per-eye mode: one UI quad layer (sampled by the compositor at its own resolution) | already outside the eye resolution and any foveation |

Outline-specific options (screen-space edges, lower-resolution edge buffers) would replace the game's own drawing and
change the look; not recommended. The game's outlines are the game's geometry: they cost what the game drew on a Wii U.

## 3. Textures and compression

- **The game's textures are BC** (BC1-5, the Wii U's formats). Where the driver reports `textureCompressionBC`, they
  are sampled natively; otherwise they are detiled and decoded to RGBA8 by a compute shader (or the CPU, on Qualcomm
  drivers older than 700) [code, docs/android-gpu.md]. RGBA8 is 8x a BC1 texture's size and 4x BC3's. **Whether the
  Quest 3's driver (512.x) reports BC is not in any recorded log**: the start-up line (`device has no BC texture
  support; ... decoder enabled` or its absence) answers it.
- **If BC is not native on Quest 3**, transcoding BC to ASTC on upload keeps the memory and bandwidth close to the
  original: BC1 (4 bpp) -> ASTC 4x4 (8 bpp) or 6x6 (3.56 bpp); BC3 (8 bpp) -> ASTC 4x4. That is a decoder in the same
  place as the compute BC decoder, with a quality check per format. ASTC 4x4 for UI-like and small textures (icons,
  text atlases), 6x6 for large environment textures only if a picture check passes; 8x8 visibly softens Wind Waker's
  flat colour edges [est]. ETC2 is lower quality than ASTC at equal size on Adreno [est].
- **Texture packs** (HD packs): OoT VR's ASTC pack compression cut pack memory from 2,133 to 534 MB [claim]. Our packs
  should ship (or be converted on the device) as ASTC for standalone, BC7/BC1-3 for PC; the same source PNGs, two
  outputs; the packs already cost no CPU at render time (project note).
- Anisotropic filtering: 4x-8x on Quest (texture bandwidth), 16x on PC; test on the sea and paths.

## 4. Foveated rendering on Quest 3

- **Quest 3 has no eye tracking** (Quest Pro does): fixed foveation only.
- **Meta's foveation (`XR_FB_foveation`, `_configuration`, `_vulkan`)** gives a fragment density map for the swapchain
  images: it foveates passes that render into them. Our scene renders offscreen and only the upscale writes the
  swapchain, so the runtime's foveation saves almost nothing as the renderer is today [code], and would only blur the
  edge of the upscale.
- **App-side fixed foveation**: `VK_EXT_fragment_density_map` on the scene's eye passes (Adreno supports it), per layer
  for multiview; never on shadow maps, the atlas or the UI; mild levels (low/medium), no extreme tier for the outlines.
  Saves GPU fragment work only; on Quest the CPU is the limit: P3, after multiview.
- Interaction: the UI quad and wrist quads are compositor-sampled, untouched by any foveation or dynamic resolution.

## 5. Dynamic resolution on Quest

- Built (GPU busy share from timestamps, half-second windows, quarter-second at half rate, hysteresis, levels
  1/0.875/0.75/0.625/0.5, no step down when CPU-bound) [code, docs/VR_DYNAMIC_RESOLUTION.md].
- On Quest: off by default, and off with multiview on Qualcomm's driver (rescales of two-eye targets preceded two
  Adreno crashes) [code]. With multiview off it may be turned on; standalone profiles top out at the fixed 0.75x
  (2026-11-03). Because the Quest is CPU-bound, it mostly holds its level [log: it went down at 90 % GPU busy and back
  up at 55 %, the game at 24-30 fps throughout].
- Recommendation: keep it off until the CPU limit is lifted; then Performance (0.625-0.75x) with multiview only once
  the Adreno rescale crash is understood.

## 6. Anti-aliasing and sharpness

| Option | Cost on Quest | Verdict |
|---|---|---|
| MSAA | would need multisampled versions of every GX2 target and resolves | not with this renderer (DrBeef's ports use MSAA because they draw straight into the swapchain tiles) |
| FXAA (the game's own) | one full-screen pass per eye | test on/off: softens the outlines; text is in the UI quad (unaffected) |
| SGSR1 | ~0.3 ms (Qualcomm, SD 8 Gen 2) | by choice; Quest Super Resolution is the default and costs the app nothing |
| SGSR2 / temporal | motion pass (CPU and GPU) | no: costs CPU the Quest lacks |
| Higher resolution | GPU | only once the CPU allows more than 36 fps |

## 7. Quest presets (proposed; each to be checked in the headset)

| Setting | Performance | Balanced | Quality |
|---|---|---|---|
| Refresh rate / frame rate | 72 Hz, Auto (36/72) | 72 Hz, Auto | 90 Hz, Auto (45/90) |
| Internal resolution | 1x at 0.625 | 1x at 0.75 (default) | 1x at 0.875 or 1 |
| Upscaler | Quest Super Resolution | Quest Super Resolution | Quest Super Resolution |
| FXAA | off | off | on (if the A/B prefers it) |
| Anisotropic | 2x | 4x | 8x |
| Shadows | 0.5x | 0.75x | 1x |
| Dynamic resolution | off | off | off |
| Frame synthesis | off | A/B | A/B |
| Multiview | off (until fixed) | off | off |

Tradeoffs: Performance gives the CPU-bound game the most GPU headroom it cannot use today; Balanced is the current
default; Quality raises sharpness at 90 Hz, where the half rate is 45 fps, above today's measured rate. Until the CPU
limit falls, Balanced at 72 Hz is the practical default.

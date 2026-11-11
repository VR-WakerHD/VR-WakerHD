# VR: the game's screen blur and bloom (2026-10-30)

Two of the game's screen effects behave differently in a headset than on a TV. Each now has a VR setting. Both are
off in the headset by default (Disable screen blur and Disable bloom checked), as asked after the 2026-10-30 headset
test; unchecking them brings back the original picture, with the fixes below.

| Setting | Launcher | In the game | Key (`settings.ini`) |
|---|---|---|---|
| Disable screen blur | Graphics > Screen effects in the headset | F1 > VR > Graphics and frame rate | `vr.screenBlur=0` |
| Disable bloom | same | same | `vr.bloom=0` |
| Disable depth of field (2026-11-06, below) | same | same | `vr.depthOfField=0` |
| Disable ambient occlusion (2026-11-06, below; unchecked by default, checked on a Quest) | same | same | `vr.ambientOcclusion=0` |

Both settings apply only while a VR session runs.

## Screen blur

**What the game does** (`interp_fx.cpp` `hook_02732D3C`). This blur covers the Swift Sail, some attacks and bosses.
Each frame, the 3D layer's overlay pass lays the last frame's picture over the new one:
- at the alpha the game sets (`101F4826`);
- through a 3×4 matrix on the picture's texture coordinates (`l_blureMtx`, `1048CF84`). That is a slight zoom about
  the centre (`dCamera_c::ResetBlure`: centre 0.5, 0.5, scale 0.99).

Frame interpolation already scales it to a per-frame share.

**In a headset**, the last frame was drawn where your head was looking then. Laid over as it is, the trail stayed
fixed to your head, so the world doubled while your head turned. The zoom also rushes out from the middle of your
view, which some people find uncomfortable.

**Now:**
- **The trail follows the world.** Before the game's matrix, the last frame's picture is moved by how far the view
  turned between the two frames (`vr_camera.h` `blur_reprojection` and `blur_compose`, from the views kept per
  display time in `CameraOverride`).
  - Turns of every kind count: your head, smooth and snap turns, and the game camera's own turns in events.
  - The move is exact at the middle of the view.
  - Towards the edges of a 120° view, the projection isn't affine, so the trail is moved less than it should be.
    It is never moved past where it should be (`test_screen_blur`).
  - An exact per-pixel move would need the overlay pass's shader (see "Next").
  - Moving your head sideways isn't corrected; within one frame that movement is tiny.
  - A turn of more than 20° between two frames is a cut: the game's matrix is used as it is.
- **Disable screen blur** runs the pass as if the game's blur flag were off (`101F4825` = 0 for that call).

The log shows `[vr] screen blur: the last frame moved with the view's turn ...` (the first 5 times), or
`[vr] screen blur: not drawn in the headset` when the setting is off.

## Bloom

**What it does.** The game extracts the bright parts of the scene into a 1280×720 chain. It blurs them with a
separable 7-tap blur: PS 44FD6E00 horizontal, 44FD7600 vertical, with the taps measured in console texels. Then it adds
the result back. The chain keeps the console's size at every internal resolution, on purpose (issue #307).

**In a headset:**
- **Stereo and time are fine.** Each eye computes its own bloom from its own picture, from the same frame only. So
  bloom can't ghost and can't break stereo.
- **Shape.** Each eye's picture is nearer square than 16:9. The log shows `[gfx] aspect 1.3859: screen targets x1.0000
  wide, x1.2828 tall`, so the chain buffers are 1.28× taller. A blur sized in console texels then reaches about 28%
  further vertically than horizontally. Glows should look taller than wide; this isn't confirmed in the headset yet.
  The same thing is noted for other picture shapes in `docs/decomp-notes.md` ("bloom/blur kernels sized in guest
  texels").
- **Softness.** The 1280×720 chain covers the whole wide view, so the glow is coarser than on a TV. It stays at the
  console's size: drawing the chain at full resolution showed blocks (issue #307).

**Disable bloom** (`vr.bloom=0`) sets the bloom extract's intensity to 0 in the headset
(`renderer.cpp` `drawn_bloom_strength`). The desktop's Graphics > Bloom strength is kept and still scales bloom when
it is on.

**Now, the shape is corrected** (`area_sample.h` `rewrite_bloom_aspect`, `Fix::BloomAspect`). The 15:18 run's
`WWHD_VK_DUMP_GLSL=1` dump shows how the two blur passes are built:
- Both take their seven taps from three varyings computed by the vertex shader. `.x` is the coordinate across the
  blur; `.y`, `.z` and `.w` are the positions along it, with the middle tap at varying 0's `.w`.
- The vertical pass (PS 44FD7600) samples `vec2(R0f.x, R0f.w)` for its middle tap. The horizontal one (44FD6E00)
  samples `vec2(R0f.w, R0f.x)`.

In the vertical pass, the six outer taps are moved towards the middle by `uf_tex0Scale.x / uf_tex0Scale.y`. That is the
chain buffer's own sx/sy, filled per draw by the renderers.

| Picture | sx/sy | Effect |
|---|---|---|
| 16:9 | 1 | the console's picture |
| headset eye | 1/1.28 | vertical taps reach as far as the horizontal ones: round glows |
| ultrawide desktop | about 1.31 | glows were wider than tall before; now round too |

The ultrawide row is a change to the desktop picture at non-16:9 shapes; it fixes the case noted in
`docs/decomp-notes.md`. The horizontal pass and the extract are unchanged. `area_sample_test` `test_bloom_aspect` pins
the rewrite.

**The diagnostic, and a crash it triggers.** `WWHD_VK_DUMP_GLSL=1` writes every translated program to
`build/game/captures/glsl/`; the bloom chain's are named `ps-<address>-bloom.glsl`. The writes make shader translation
slower. While a draw waits for a translation, the renderer shows the last frame again
(`present_completed_frame_while_pipeline_pending`).
- The 15:18 run crashed in the NVIDIA driver on that path: `nvoglv64.dll+0xE3B650`, called from `end_encoder` in
  `stereo_flush`.
- That driver crash is not new: the same offset is in eight crash logs from 2026-10-26.
- Use the dump only for diagnosis, never in a test run. The crash itself is fixed (below).

## Next

- Blur, optional: move the last frame exactly per pixel in the overlay pass's shader, if the affine move leaves the
  edges visibly doubled.
- (The repeat-frame crash during shader translation is fixed: see "The NVIDIA driver crash" below.)

## Tests

`runtime/tools/vr_test.cpp` `test_screen_blur`:
- no turn gives the identity;
- a turn, a look up and a roll each move the middle exactly as projected;
- a cut leaves the matrix alone;
- the game's zoom is composed after the move;
- the fit is never worse than no move, and a quarter of the error near the middle;
- both settings default to off (disabled) and save and read back.

`runtime/tools/area_sample_test.cpp` `test_bloom_aspect`:
- the vertical pass is squeezed after its varyings are read, and its taps are untouched;
- the same in MSL;
- the horizontal pass and other shaders are left alone;
- the arithmetic at 16:9 and in the headset.

Not yet seen in the headset.

## Heat haze (2026-10-30)

**Disable heat haze** (`vr.heatHaze=0`, the default) is in the launcher's Graphics tab and in F1 > VR, right under
Disable bloom.

**What it does.** The heat haze is the game's projection particles (draw group 4 of `dPa_control_c::draw`): `o
kagerou00`, effect 0x4004, and the stages' `heatfloor00/01`, over lava and hot ground in Dragon Roost and the lava
rooms. They bend the picture behind them.
- **In a headset**, the shimmer wobbles fixed to your head.
- **The cost:** the player reported frame drops where it is drawn.

**The skip.** With the setting on, that group isn't drawn in play either (`vr_ui.h` `draws_particle_group`, the same
skip that already kept it off dialogue text). Only the draw is skipped: the particles keep running, so unchecking it
brings the haze back at once. Other particle groups and the desktop are unchanged. `vr_test` checks this.

Not yet measured: the frame rate in Dragon Roost with it on and off.

## Depth of field (2026-11-06)

Asked for: off by default in every headset, like bloom and the screen blur, with a setting in F1 > VR and the
launcher (Graphics > Screen effects in the headset: **Disable depth of field**, `vr.depthOfField=0`).

**What the game does.** Wind Waker HD draws Nintendo's agl `DepthOfField` (`.bagldof` parameter sets: `wwHD_basic`,
`wwHD_night`, `wwHD_figure`, `scope`, `Monotone`, `MonotoneOut`, `MonotoneOff`). Its draw (`027824A8`) blurs the far
(`far_enable`) and near (`near_enable`) scenery from a mip chain of the picture and its depth. The same effect also does
colour control (`enable_color_control`, `saturate_min`: likely the sealed Hyrule's grey), vignetting (likely the
telescope's edge) and distortion (`indirect_*`).

**In a headset** the blur adds nothing: stereo already shows how far things are. It stays where the game focuses, not
where you look, so a distant island you look at stays blurred, which reads as blurry vision; it also undoes some of the
headset's sharpness.

**Disable depth of field** (`interp_fx.cpp` `hook_027824A8`) clears the layer's parameter set's `near_enable` (`+0x0C`)
and `far_enable` (`+0x1C`) for the draw and puts them back after. The draw then takes the game's own path without blur
(`0277E714` false: no mip chain or blur passes) and still runs the colour, vignetting and distortion pass (`0277C944`).
Less GPU time per eye; the CPU's draws are nearly the same. The log shows `[vr] depth of field: its blur not drawn in the
headset` (the first 3 times). The desktop is unchanged.

**Not yet seen in a headset.** To check: distant scenery sharp (Outset from the sea, Windfall); the sealed Hyrule still
grey; the telescope's edge as before; nothing black or missing.

## Ambient occlusion (2026-11-06)

Asked for: a toggle, "not on by default for now". **Disable ambient occlusion** (F1 > VR; launcher Graphics > Screen
effects in the headset; `vr.ambientOcclusion`, default `1`: AO drawn) is unchecked by default. In the headset only.
On a Quest (standalone) it is checked by default (2026-11-10, the user's choice: the GPU is the limit there;
`vr.ambientOcclusion` default `0`, settings format 19 moves a Quest's saved AO-on to off; still choosable).

**What the game does.** The AO pass (PS `44BDFD00`, `ProgramKind::OcclusionPixel`) reads a point-sampled centre depth,
a noise texture and eight depth taps, and writes `(ao, 0, 0, 1)` to the light buffer's red; the sun's shadow mask is its
own pass (PS by hash `0x62ae9ab4...`), so it stays. Its last step is `ao = 1 - clamp((x - 0.5) * strength)`, the
strength `uf_remappedPS[0].y`.

**Off:** the draw sets that strength to 0 (`draw.cpp`, beside the bloom's uniform scale), which by the shader's own
maths writes `(1, 0, 0, 1)`: no occlusion. The translated shader also returns that at once when the strength is 0
(`shaders.cpp`, an early return before its taps), so the pass costs almost nothing; with AO on the strength is never 0
and the shader runs as before (its GLSL changes, so it is compiled again once; the disk cache is keyed by the GLSL).
If the GLSL is not the expected code the early return is left out and stderr says so; the uniform alone still turns AO
off. Applies at once. **Not yet seen in a headset:** the soft shading under Link and in corners gone, the sun's shadows
kept, nothing black.

## Shadow maps on the Quest 3 (2026-11-06)

Asked for: "For quest 3 standalone lets Lower-resolution shadows". On a Quest the shadow maps are 0.5 of the console's
1024x1024 (512x512, a quarter of its pixels; 0.75 since 2026-11-03), `surfaces.cpp` `target_scale`. GPU time only (the
same draws); shadow edges are softer. The Steam Frame stays at 0.75, PCs and phones as before; `WWHD_SHADOW_SCALE`
still sets it. The log line `[gfx] shadow maps at 0.5x the console's 1024x1024` shows it. Not built here (no NDK) and
not yet seen in the headset.

## The NVIDIA driver crash (fixed 2026-10-30)

**Symptom.** `nvoglv64.dll+0xE3B650`, a read of address 0x130, called from `end_encoder` in `stereo_flush`, inside
`present()`. It happened in 8 crash logs from 2026-10-26 (desktop stereo) and in the 15:18 headset run.

**Path.** A draw waits for a shader or pipeline. Meanwhile the last finished frame is shown again
(`present_completed_frame_while_pipeline_pending`).

**Cause.**
- `present()` records on the render thread with isolated dispatch (`ProducerPresentationRecording`), and isolated
  dispatch bypasses every held stream.
- The normal frame end flushes the stereo streams before presenting. The repeat did not.
- With eye 1's commands held mid-frame (its pass begin among them), the `stereo_flush` inside the present's submission
  sent eye 1's pass end straight into the renderer's command buffer.
- The driver was asked to end a pass that was never begun there, and crashed.

**Evidence.**
- `WWHD_TEST_SLOW_TRANSLATION_MS=30` makes each translation 30 ms slower, which forces the repeat. With it, the game
  crashed at the 10th repeat.
- The new log line shows the state at that repeat: both eyes' passes open, 34 of eye 1's commands held. Repeats 1 to 9
  had nothing held and passed.

**Fix** (`backend.cpp`). The repeat calls `stereo_flush()` after ending its pass and before presenting, as `swap()`
does. Eye 1's pass ends in its own stream, and the held commands go into the renderer's stream in order, as at every
mid-frame flush (about 12 a frame).

**Verified.** The same repro run (headset, Capped 90, 16:00 to 16:02) passed 20 repeats, 3 of them with eye 1's
commands held, and closed normally. The player confirmed the crash is gone.

**Kept for later checks:**
- the log line `[vulkan] frame N repeated while a shader or pipeline is made (n): <stereo state>`, for the first 20
  repeats and then every 1000th (`stereo_debug_state`);
- the test switch `WWHD_TEST_SLOW_TRANSLATION_MS`.

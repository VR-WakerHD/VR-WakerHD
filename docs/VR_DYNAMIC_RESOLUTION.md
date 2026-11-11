# VR dynamic resolution

While the GPU holds the headset's frame rate back, each eye is drawn at a lower resolution, and higher again once there
is room. Settings: F1 > VR > *VR dynamic resolution*, or the VR launcher's Graphics tab (`vr.dynamicResolution`).
It applies at once and is **off by default** everywhere (PC VR too, where VR upscaling and frame synthesis are also off by default).

## How it works

It is the game's own dynamic resolution (`runtime/src/dynres.cpp`, `dynres_core.h`, the flat game's
Graphics > Dynamic resolution), with a VR profile that replaces that setting while a session runs
(`runtime/src/vr/vr_dynres.h`; the VR renderer hands the profile over each headset frame, `vr_vulkan.cpp`
`update_dynamic_resolution`).

- **GPU time, not CPU time.** Every command buffer carries GPU timestamps. Their intervals are joined, so overlapping
  work counts once (`BusyUnion`), and read back without waiting for the GPU. Each half-second window gives the GPU's busy
  share. **At half the headset's rate** (36 fps at 72 Hz, 45 at 90; 2026-11-03) the windows are a quarter of a second
  (`dynres_core.h` `window_seconds`): a late frame there costs two refreshes, so an overload is found and stepped down
  from twice as fast (9 frames at 36 fps). The way up keeps its pace: a second of room (four quarter windows, as two
  half-second ones did) and the same hold in seconds.
- **The headset's frame rate.** The rate it aims for is the rate the session is paced at: 90 fps at 90 Hz, 45 when
  capped at half (`vr.frameRate`), and the refresh rate when unlocked.
- **Overloaded** means the window missed that rate (under 95 % of it) while the GPU was busier than the profile's GPU
  target. Then the resolution goes **one level** down. A frame rate held back by the CPU leaves the GPU idle, so the
  resolution stays.
- **Room** means the busy share predicted at the next level up (pixels grow with the square of the scale) is 10 points
  under the target for two windows in a row, and the settle time has passed since the last step down. Then it goes one
  level up. When a step up has to be undone within 4 s, the wait doubles (up to 32 s). After 30 s without a step down,
  it goes back to the settle time.
- **Fixed levels:** 1, 0.875, 0.75, 0.625 and 0.5 of the internal resolution (linear; 0.75 is 56 % of the pixels). The
  renderer keeps the render target images of the levels next to the current one, and makes them ahead a few at a time,
  so a change copies and does not allocate. The window after a change is not judged.
- **Nothing else changes size.** The upscalers (SGSR1, FSR 1, SGSR2, the blit) and the projection layer already take
  each frame's eye picture region. SGSR2 starts its history again at a change of size. Frame synthesis' depth and motion
  are sampled into the runtime's fixed-size images.

## Profiles

| Profile | Scale range | GPU target | Settle time |
|---|---|---|---|
| Quality | 0.875x .. 1x | 90 % busy | 4 s |
| Balanced | 0.75x .. 1x | 85 % | 2 s |
| Performance | 0.625x .. 1x | 80 % | 2 s |
| Custom | `vr.dynresMin` .. `vr.dynresMax` (the levels inside that range; a range with none inside takes the level just under the maximum) | `vr.dynresGpuTarget` (50..98 %) | `vr.dynresSettle` (0.5..32 s) |

The maximum is the internal resolution (Graphics). A scale above 1 is a higher internal resolution, chosen there, not a
dynamic one. With the profile at its highest level from the start, a headset with room draws at the full internal
resolution.

**Standalone headsets (Quest, Steam Frame; 2026-11-03, the user's choice):** Quality, Balanced and Performance top out
at the headset's fixed factor (0.75x by default, `dynres::fixed_factor`), and the minimum is never above it: Quality
and Balanced then hold 0.75x, Performance goes 0.625x .. 0.75x. Before, they reached 1x, so turning dynamic resolution
on first *raised* the Frame's load above its fixed 0.75x until the controller stepped down; a player's Frame run with
Performance and frame synthesis smeared meanwhile. Custom keeps its own range (`vr.dynresMax` up to 1x). A fixed
factor of 1 (Graphics, or `WWHD_RES_FIXED=1`) leaves the profiles as before (`vr_dynres.h` `Device::fixedMax`).

## Where it runs

- **PC VR (Windows, Linux):** with every profile.
- **Quest (Qualcomm's own Adreno driver):** only with multiview off. Two Quest 3 crashes in that driver came right
  after a change of scale that rebuilt multiview's two-eye images (`docs/VR_ANDROID.md`). Multiview is off on Android
  by default. With it on, F1 > VR says why dynamic resolution is off.
- **Steam Frame - Native and Lepton (Mesa's Turnip driver):** with multiview on too, because it's a different driver.
  The rule is decided by the driver at run time (`VK_DRIVER_ID_MESA_TURNIP`), not by the build: Lepton's Vulkan is
  Turnip as well (docs/VR_STEAM_FRAME.md). This is untested in the headset.
- Earlier Quest logs showed the CPU, not the GPU, holding the frame rate. Then the resolution stays, and the profile
  mostly saves nothing. Steam Frame - Native's logs (2026-11-03) looked GPU-bound.
- **The flat game's Graphics > Dynamic resolution** stays as it was. A VR session uses the VR setting instead (Off
  included), and the Graphics setting comes back when the session ends.

## Diagnostics

- **F1 > VR:** the choice and why, then the scale in use, its range, the GPU's busy share and the number of changes.
- **The log:**
  - `[dynres] VR: ...` when the profile changes.
  - `[dynres] a -> b ...` at each change, with the busy share and frame rate that caused it.
  - `[vr] Dynamic resolution: ...` with the frame times every 10 s.
- `WWHD_DYNRES_LOG_FRAMES=1` logs the frame times around each change, and `WWHD_DYNRES_TEST_BUDGET` /
  `WWHD_DYNRES_TEST_AT` force changes (`dynres.cpp`).

## Tests

- `dynres_test` checks the controller: the levels and the highest level, the GPU target, settling, holds, and the range
  moving under the level in use.
- `vr_test` checks the profiles, the standalone multiview rule, Custom's clamping, and saving and reading back.

None of it has been run in a headset yet.

## Not in this stage

Foveated rendering (fixed, or eye-tracked on the Steam Frame) and an automatic mode that coordinates it with dynamic
resolution are planned separately. The runtime's own foveation (`XR_FB_foveation_vulkan`) only covers drawing into
its swapchain images, and the game draws offscreen and then upscales. So foveation here needs the game's own fragment
density maps on the eye passes (`docs/VR_UPSCALING.md`).

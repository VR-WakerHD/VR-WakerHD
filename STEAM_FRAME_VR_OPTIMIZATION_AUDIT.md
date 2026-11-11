# Steam Frame VR optimization audit: ZeldaWWHD-VR (2026-11-03)

This report covers the research, the codebase architecture, the bottlenecks, graphics quality, Steam Frame
compatibility, foveated rendering, dynamic resolution, frame pacing and the player experience. The prioritized work
is in [VR_PERFORMANCE_ROADMAP.md](VR_PERFORMANCE_ROADMAP.md).

**How each claim is marked**

| Mark | Meaning |
|---|---|
| **[code]** | read in this repository's code (file and function named) |
| **[log]** | from a headset log or measurement already recorded in `docs/` (headset and date named) |
| **[build]** | built here on this PC (Windows x64, `build/game`) |
| **[src]** | from an external primary source (linked) |
| **[claim]** | stated by a third party, not measured by us or confirmed by Valve |
| **[est]** | an estimate or a hypothesis, not measured |

No test was run for this audit: the project rule is headset-only tests, each asked for first. This PC builds only the
Windows x64 target; nothing here was built or run for arm64 (Steam Frame Native, Lepton, Quest).

---

## 0. Summary

1. **The renderer's structure is sound for VR** [code]: true stereo of the real game world, multiview on by default,
   each eye with its own projection (on by default since 2026-11-03), the game's UI as a head-locked quad layer, the
   wrist HUD, item wheel and laser as quad layers, xrEndFrame on its own thread, the 30 Hz logic interpolated to
   90/60/45/40/36 fps schedules locked to the headset's clock. PS VR2 on an RTX 5080 holds 89.5 fps average with 92 %
   of the time at 89.5 or above [log].
2. **Steam Frame's biggest risk today is frame synthesis quality, not raw speed.** A player run with frame synthesis,
   dynamic resolution Performance and SGSR1 smeared and tore; an earlier run at a fixed 0.75x with frame synthesis
   looked clean [log, player video]. QuakeVR's Steam Frame port also saw artifacts and ships frame synthesis off
   [src]. Two code-level contributors were found:
   - **Fixed:** with an eye submitted as a cropped part of its image (`vr.eyeCrop` on with the shared projection), the
     motion vectors were in the whole image's NDC but labelled as the crop's, so too small by crop/full. They are now
     scaled by `motionVectorScale` (section 7.4). The default per-eye projection submits whole images, so the default
     path is unchanged (scale 1).
   - **Not changed, needs a decision:** on standalone headsets every dynamic resolution profile tops out at 1.0x of the
     internal resolution, above the fixed 0.75x those headsets use when it is off [code: `vr_dynres.h` `profile`,
     `dynres.cpp` `fixed_factor`]. Turning dynamic resolution on therefore *raises* the GPU load until the controller
     finds overload and steps down; with frame synthesis, every late frame makes the runtime synthesize from stale
     data. This matches "fixed 0.75x clean, dynamic Performance smeared" [est].
3. **Valve's foveation layers cannot help this game as it renders today** [src + code]. On the Frame, SteamOS's launch
   wrapper adds `VK_LAYER_VALVE_rpo` and `VK_LAYER_VALVE_fdm_injection` to every game through `VK_INSTANCE_LAYERS`
   ([SteamOS#2973](https://github.com/ValveSoftware/SteamOS/issues/2973)); the injection layer only foveates images it
   creates, the XR swapchain ([framefov](https://github.com/wakira/framefov)). The game draws every scene pass into its
   own offscreen targets and only copies or upscales into the swapchain (`vr_vulkan.cpp` `record_eyes`), so at best
   the cheap upscale pass is foveated (lower peripheral quality, no scene savings). Real foveation here means the game's
   own fragment density maps on its eye passes.
4. **The CPU still bounds the standalone headsets** [log: Quest 3 2026-11-02, about 30 fps at every refresh rate; the
   render thread waited 13 to 17 ms in xrEndFrame]. The end-frame thread now removes that wait on PC [log: PS VR2]; on
   Quest and the Frame it is unmeasured. The Frame Native logs of 2026-11-03 looked GPU-bound at 36 fps [log].
5. **Implemented in this audit (built, not run):** the motion-vector crop scale; the game UI layer kept on frames shown
   again while the game is busy (it blinked off); a log line naming Valve's injected Vulkan layers and `FDM_DEBUG`.

---

## 1. Current architecture (traced in code)

### 1.1 Rendering API and devices

- **GX2 on Vulkan** (`runtime/src/gfx/vulkan/`): the Wii U's GX2 calls are implemented natively; the guest's Latte
  shaders are translated to SPIR-V at run time (`shader_translation.cpp`, `shader_compiler.cpp`, disk cache
  `shader_disk_cache.h`, pipeline cache `pipeline_cache_path.h`, pipelines built on worker threads
  `pipeline_worker.cpp`; draws whose pipeline is not ready are left out rather than stalling, docs/VR_PERFORMANCE.md
  "Draws left out while their pipeline is made") [code].
- **OpenXR binding** (`vr/openxr_session.cpp`, `gfx/vulkan/vr_vulkan.cpp`): `XR_KHR_vulkan_enable2`. The Vulkan
  instance and device are created through the runtime (`create_instance` -> `xrCreateVulkanInstanceKHR`,
  `preferred_device` -> the runtime's physical device, `create_device` -> `xrCreateVulkanDeviceKHR`); the session
  begins in `device_ready`. Without a runtime the game falls back to the desktop [code].
- **Swapchains**: one per eye at the runtime's recommended size times `vr.renderScale`, plus quad swapchains (GamePad
  panel, item wheel, wrist HUD, laser, game UI layer) and, with frame synthesis, a motion (RGBA16F) and depth
  (D24S8, else D32F/X8D24/D16) swapchain per eye at the runtime's recommended synthesis size [code]. Quad swapchains
  made mid-session are created under the graphics queue lock (c178e1ac) [code].

### 1.2 Stereo

- **Mode `stereo::XR`** (`gfx/vulkan/stereo.cpp`): both eyes of the real scene. Eye 1 replays eye 0's command stream
  with its own targets and eye-shifted projection rows (`stereo::project_eye`, `apply_eye_rows`) [code].
- **Multiview** (`multiview_pipeline.cpp`, `multiview_glsl.cpp`, docs/VR_MULTIVIEW.md): one draw for both eyes into
  two-layer targets where the draw allows it; on by default (headset-confirmed). Draws it cannot take go through eye
  1's replay [code].
- **Per-eye projection** (`vr.cpp` `own_eye_projection`, default on; `WWHD_VR_EYE_PROJECTION=shared` for the old
  symmetric field): each eye's picture is its own asymmetric frustum within the camera's symmetric one
  (`stereo::EyeFrustum` sx/ox/sy/oy), so no pixels are spent on the other eye's side [code]. The culling and the
  game's camera keep the symmetric union (`drawn_fov`), plus a 5° margin on standalone builds for reprojection
  (`fov_margin`) [code].
- **UI**: in per-eye mode the game's layouts draw once into a UI image (premultiplied), submitted as a head-locked
  quad layer at `kUiDistance` (`record_eyes`, `frame_end`, `end_frame`) [code]; the wrist HUD, item wheel and laser are
  CPU-drawn pictures on quad layers [code].

### 1.3 Threads and frame flow

| Thread | Work | Where |
|---|---|---|
| Game thread | the recompiled game: logic steps at 30 Hz, hold passes (draw only) for interpolated frames, `xrSyncActions`, controller and head poses, the VR camera override, GX2 command generation | `interp.cpp`, `vr/vr_game.cpp` |
| Render thread | GX2 command translation to Vulkan, `frame_begin` (xrWaitFrame/BeginFrame, locate views), eye copies and upscale (`record_eyes`), submission | `backend.cpp`, `vr_vulkan.cpp` |
| Record worker | records command buffers off the render thread (on by default) | `record_worker.cpp` |
| Eye thread | eye 1's held commands | `eye_thread.cpp` |
| Atlas thread | the sun shadow atlas's draws in their own stream | `atlas_thread.cpp`, docs/VR_ATLAS_THREAD.md |
| Scene threads A/B | parts of the scene's draw preparation | docs/VR_SCENE_THREADS.md |
| End-frame thread | release swapchain images + `xrEndFrame` under the graphics queue lock; the render thread waits for it only before the next `xrWaitFrame` | `vr_vulkan.cpp` `end_thread`, on by default (`WWHD_VR_END_FRAME_THREAD=0` off) |
| Pipeline workers | shader and pipeline compilation | `pipeline_worker.cpp` |

Order per frame [code, `vr_vulkan.cpp`]: `frame_begin` (wait for last end -> poll events -> `xrWaitFrame` +
`xrBeginFrame` -> refresh/pacing updates -> `locate_views(predicted)` -> eye offsets and frustums) -> present and
`record_eyes` (acquire, upscale or blit, frame synthesis export, UI/panel/wheel/wrist/laser uploads) -> submit ->
`frame_end` (views by id, job to the end-frame thread). `headset_frame` re-shows the last frame after
max(3 periods, 2x the game's frame interval) without a new one, so the runtime never times the game out [code].

### 1.4 Frame pacing

- Logic is always 30 steps/s (29.97 on console; locked to the headset clock in VR: 30.00) [code: `lock_to_headset`].
- Frame interpolation (`interp.cpp`, `interp_pacing.h`) draws blended hold passes: schedules for 120, 90, 60, 45, 40,
  36 and 30 fps; 72 and 80 Hz "full" draw at 60 (no 72/80 schedule) [code, docs/VR_FRAME_RATE.md].
- `vr.frameRate`: Unlocked, Full, Half, Auto (default). Auto drops to half under 95 % in-between frames over 1.5 s
  and retries full after 10 s (doubling to 40 s) [code: `auto_rate_step`].
- With frame synthesis carrying motion, the game draws at half (`frame_begin`: `set_vr_frame_mode(Half)`) [code].
- Slip guard: three frames off by ~a refresh within 2 s move the vsync grid half a refresh later (`note_display`)
  [code].

### 1.5 Resolution, upscaling, frame synthesis

- **Internal resolution** `resScale` (1, 1.5, 2, 3 of 1280x720, shaped to the eye's field). Standalone builds draw a
  fixed 0.75x of it by default (`dynres.cpp` `fixed_factor`; `WWHD_RES_FIXED`) [code].
- **Dynamic resolution** (`dynres.cpp`, `dynres_core.h`, `vr_dynres.h`): GPU busy share from timestamps on every
  command buffer (intervals joined), half-second windows, levels 1 / 0.875 / 0.75 / 0.625 / 0.5, one level per step,
  hysteresis (up only when the predicted share at the next level is 10 points under target for two windows),
  settle time doubling on swings to 32 s, neighbour level images pre-created so a change copies and does not allocate
  (`surfaces.cpp` `precreate_levels`). CPU-bound windows (GPU idle) do not step down. Off by default everywhere
  [code].
- **Upscalers** (`vr/vr_upscale.h` `choose`, one at a time): DLSS/DLAA (PC RTX, `ngx_vk.cpp`), Quest Super
  Resolution (compositor, `XR_FB_composition_layer_settings`), SGSR1 (single pass into the swapchain image, sharpness
  `vr.upscaleSharpness` 1..2, default 2), SGSR2 (temporal, by choice), FSR1 (by choice), else a linear blit [code].
- **Frame synthesis** (`vr/vr_framesynth.h` `choose`, `gfx/vulkan/vr_framesynth.cpp`): `XR_EXT_frame_synthesis`, else
  `XR_FB_space_warp`, else depth only (`XR_KHR_composition_layer_depth`). Auto on standalone; off on PC VR (user,
  2026-11-03). Motion vectors come from the **motion pass** (`motion_pass.cpp`): every depth-writing draw drawn again
  with the previous and current frame's vertex data, depth EQUAL against the scene depth, plus the background; per
  eye, or both eyes in one pass on mobile GPUs (`WWHD_MOBILE_GPU_PATHS`). Exported per eye as `(prev-cur)*0.5` -> NDC
  `cur-prev` (`motion_scale`), nearest sampling, z = 0, `appSpaceDeltaPose` identity [code].
- Depth is not sent while text or a menu shows (a runtime compositing by it hid the UI quad), and not for a Quest on a
  PC with Auto [code: `end_frame`, `uiShown`].

### 1.6 Graphics settings available

`resScale`, FXAA (`fxaa`), anisotropic filtering (`aniso`, up to 16x), bloom, ambient occlusion modes, clouds and
cloud shadows, shadow map scale (0.75x = 768x768 on standalone; `WWHD_SHADOW_SCALE` 0.5..4), artistic-mip LOD bias
(`WWHD_ARTISTIC_MIPS`), runtime texture packs (CPU-free at render time: docs and memory note) [code, docs]. No MSAA
path exists for the game's own targets [code: no sample-count option in the surface or pipeline creation].

### 1.7 Camera, tracking, input

- One camera writer: `camera_draw` (024FFC40) gets the head's eye/center/up after the interpolation blend, then the
  game's state is restored (docs/VR_CAMERA_AUDIT.md, one writer and one head pose per frame, four timing fixes) [code].
- Head pose located at the predicted display time plus one frame on the game thread; each frame carries its views id
  to `xrEndFrame`, so the runtime only corrects prediction error [code: `frame_end` `vr::views(g_frameViews...)`].
- Controllers: OpenXR actions -> a virtual GamePad (`vr_bindings.h`), Touch profile by default on SteamVR and
  `XR_VALVE_frame_controller_interaction` for the Frame's own profile; swing detection presses B for two logic steps
  and moves the game's own sword capsules (`vr_swing.h`); bow, hookshot, boomerang, grappling hook aim by pointing
  with a laser quad (docs/VR_AIMING.md); telescope eyepiece (docs/VR_TELESCOPE.md); Picto Box (docs/VR_PICTO_BOX.md)
  [code, docs].
- Comfort: smooth or snap turning, recentre, head offset limit (`vr.maxHeadOffset`), cutscene follow, camera held
  above water in first person (ad9902a3), no camera bob in conversations (11588950) [code].

### 1.8 Platform builds

| Target | Toolchain | Defines | Status |
|---|---|---|---|
| Windows x64 | llvm-mingw (this PC) | `WWHD_VR` | built here [build] |
| Linux x64 | zig/clang | `WWHD_VR` | documented, release job |
| Steam Frame Native (Linux arm64, SteamOS) | zig, glibc 2.35 | `WWHD_STEAM_FRAME_NATIVE` | built and run by the user on the Frame (player logs, video) |
| Steam Frame Lepton (Android 11 container) | NDK 30, API 30 | `WWHD_STEAM_FRAME_LEPTON` | APK tried once (launcher only) |
| Quest / Android XR | NDK, API 33 | `WWHD_ANDROID_VR` | APK runs; ~30 fps [log] |

Platform-specific behaviour is chosen in several places: `#if defined(__ANDROID__) || defined(__aarch64__)` in
`vr_vulkan.cpp` (`update_dynamic_resolution`, upscale caps), `vr.cpp` `fov_margin`, `vr_config.h` `kPcVrBuild`,
`WWHD_MOBILE_GPU_PATHS` in the motion pass, `xs().quest()` / `steamvr_` in the session, `VK_DRIVER_ID_MESA_TURNIP` in
dynres. Each decision function already takes a capabilities struct (`vr::upscale::Caps`, `dynres_vr::Device`,
`framesynth::choose`), which is the right shape for the abstraction in section 10 [code].

### 1.9 Implemented / partial / unused / broken / missing

| State | Items |
|---|---|
| Implemented, headset-verified (PC) | OpenXR session, multiview, per-eye projection, UI quad layer, end-frame thread, interpolation schedules, wrist HUD, sword, aiming, DLSS path (NVIDIA) |
| Implemented, not verified on the target | frame synthesis export on Turnip (first Frame runs only), motion pass on Adreno, dynres with multiview on Turnip, 36 fps schedule, SGSR1/2 on the Frame, Quest Super Resolution |
| Partial | foveation (none), eye tracking (not used), 72/80 Hz full-rate schedules (drawn at 60), motion z (0), FB space warp with cropped eyes (no scale field) |
| Unused on the Frame | `XR_META_recommended_layer_resolution` (offered on the Frame [claim, FramePort]); `XR_EXT_eye_gaze_interaction`; the FB/META foveation extensions Valve lists |
| Broken (fixed here) | cropped-eye motion vectors (scale); UI quad missing on re-shown frames |
| Missing | app-side foveation; `vrpreferences.json`; a per-platform preset system; a Frame performance runbook (section 11) |
| Documentation mismatch | docs/VR_DYNAMIC_RESOLUTION.md says Lepton uses Qualcomm's own driver, but docs/VR_STEAM_FRAME.md says Lepton's Vulkan is Mesa Turnip; the code decides by `VkDriverId` at run time, so Lepton on Turnip already gets the Turnip rule |

---

## 2. External implementations

### 2.1 Comparison table

| Project | Platform | Rendering | Performance techniques | Foveation | Reprojection | Lessons | Applies to us |
|---|---|---|---|---|---|---|---|
| [bs-arm64](https://github.com/DaVarga/bs-arm64) (Beat Saber, Unity 6) | Frame, Windows ARM64 build under ARM64 Proton | Unity, OpenXR on SteamVR | native ARM64 instead of FEX; lower settings (AA 2x, bloom, mirror, smoke, Screen Distortion off); Adaptive SFX off (audio thread) | SteamVR's own (Steam per-game switch; `FDM_DEBUG=enable,hi` stronger) | SteamVR | native code halves CPU time; GPU-bound at default settings (~39 fps both builds) | native arm64: we already do it; Valve's layer works for Unity because Unity renders into the swapchain |
| [QuakeVR PR #75](https://github.com/vittorioromeo/quakevr/pull/75) | Frame, Linux arm64 | OpenGL via Zink on Turnip | render scale 0.45 for ~72 fps calibration; `TU_DEBUG=sysmem` ~3 % faster | none | `XR_EXT_frame_synthesis` built but **off by default**: artifacts seen, no measured benefit, 1.5-2 ms cost, risk of 72 -> 36 throttle | mask the lens hidden area (15.65 % of each eye) in depth; write every motion texel; GPU hang workaround `TU_DEBUG=sysmem` | frame synthesis default for the Frame; hidden-area mask; Turnip GMEM vs sysmem test |
| Half-Life: Alyx (Valve) | Frame standalone (ARM64 port, 2026-09-14); PC | Source 2 (Vulkan on PC for foveation) | ARM64 port with assets unchanged; dynamic fidelity (`vr_fidelity_level_auto`) | eye-tracked on the Frame; PC: lo/med/hi/ultra, rings at full, 1/4 and 1/16 resolution [claim] | depth reprojection "smooth gameplay at lower framerates"; stencil tagging for reprojection [claim] | depth reprojection plus foveation plus dynamic fidelity is Valve's own recipe | depth we already send; foveation needs app-side FDM; dynamic fidelity = our dynres |
| Half-Life 2: VR Mod (Source VR Mod Team; community fork [HL2VRU](https://github.com/vittorioromeo/HL2VRU)) | PC VR | Source 1, OpenVR | — | none | runtime | VR UI on the hands, weapons on the controllers, several locomotion options; physical melee in the fork | ours already follow these patterns (wrist HUD, tracked sword); source not found, so no code-level lessons |
| [framefov](https://github.com/wakira/framefov) | Frame, native OpenVR titles | Vulkan layer | — | injects a fragment-shading-rate attachment in every render pass, fixed or gaze | — | Valve's FDM layer only reaches XR swapchain images; Turnip snaps the rate to GMEM bins | confirms we need our own density/shading-rate attachments on the scene passes |
| [FramePort runtime notes](https://frameport.app/docs/frame-runtime/) | Frame, Lepton/native | — | — | FDM injection probe: 54x54 two-layer density map on 1728x1728 eye buffers; Batman wearer test failed (rainbow glitches, jitter) | — | panel rates 72/80/90/96/108/120/144 Hz; 16 GB RAM; GPU 231-903 MHz; 3 MiB GMEM [claim] | rates to offer; forced injection is not safe as a default |
| SUPERHOT VR (reported) | Frame | Unity | — | eye-tracked foveation on by default on the Frame [claim, mixed-news] | — | — | — |

### 2.2 Beat Saber ARM64: details

- **Measured** [src, the project's own benchmark: STARLIGHT Expert+ replay, five runs per build, 2160x2160 per eye,
  120 Hz, reduced settings]: native ARM64 ~120 fps, 1.0 % reprojected, 4.2 ms app CPU, 14.4 W; x64 via FEX ~104 fps,
  30.9 % reprojected, 8.4 ms, 19.2 W. With default graphics both builds were GPU-bound at about 39 fps.
- **Foveation**: native builds follow Steam's per-game Foveated Rendering switch (gaze-following with the Frame's eye
  tracking); x64 builds need `FDM_DEBUG=enable VK_INSTANCE_LAYERS=VK_LAYER_VALVE_rpo:VK_LAYER_VALVE_fdm_injection`
  [src: [bs-manager steam-frame.md](https://github.com/DaVarga/bs-manager/blob/bs-arm64/docs/steam-frame.md)]. Not
  documented by Valve. No measurements of its benefit are published.
- **Audio**: Adaptive SFX's loudness measurement took most of the audio thread on ARM64; off gave "30 % fewer slow
  frames" [src, claim with no method given].
- **Unity-specific**: the foveation path works because Unity draws the eyes straight into the XR swapchain images.
  Not transferable to our offscreen renderer.

### 2.3 Valve's layers

- `VK_LAYER_VALVE_fdm_injection`: adds a fragment density map to render passes that draw into images Valve's stack
  created (the XR swapchain); tuned by `FDM_DEBUG` (`enable`, `hi`) and, per FramePort's binary analysis, `FOVE_LEVEL`
  lo/med/hi [claim]. Enabled by Steam's per-game "Foveated Rendering" switch on native titles [src: bs-manager].
- `VK_LAYER_VALVE_rpo`: undocumented; added to every game by `steam-launch-wrapper`; crashed No Man's Sky VR with
  Adreno UCHE faults; `VK_LOADER_LAYERS_DISABLE=*rpo*` removes it [src:
  [SteamOS#2973](https://github.com/ValveSoftware/SteamOS/issues/2973), open, no Valve reply].
- **For us**: both are present in a Steam-launched Native process [est: same wrapper]. Our renderer intercepts no
  layer calls, but RPO intercepts pipelines and descriptors and this renderer creates many; if a Frame run shows GPU
  faults or crashes, an A/B with `VK_LOADER_LAYERS_DISABLE=*rpo*` is the first test. The new log line (section 12)
  records what was in the process. **Neither is a shipping dependency.**

### 2.4 Other relevant work

- Mesa Turnip foveation (Connor Abbott, XDC 2026): `VK_EXT_fragment_density_map` and related extensions on Adreno
  [src, news report of the talk]; Mesa's features list gives FDM and FDM offset on a7xx, not FDM2 (docs/VR_UPSCALING.md,
  2026-11-01).
- Meta's AppSW / `XR_EXT_frame_synthesis`: positional timewarp from depth; synthesized frames extrapolate, so
  controller input reaches the picture at the app rate (docs/VR_FRAME_SYNTHESIS.md) [src].

---

## 3. Steam Frame platform

### 3.1 Execution paths

| Path | What it is | Our status | Recommendation |
|---|---|---|---|
| **Native Linux arm64** (SteamOS) | the game as an arm64 ELF, SteamVR's Linux OpenXR runtime (1.0 only), Turnip | built (release job), runs | **primary target** |
| Lepton (Android 11 container) | the APK, SteamVR's Android runtime, Turnip | APK launches, session untested | secondary; same renderer; no cylinder layers; Android 12+ APIs looked up at run time |
| Windows ARM64 under ARM64 Proton | bs-arm64's route | not built (no Windows arm64 toolchain set up) | not needed: Native already is native code |
| x64 Windows or Linux under FEX (+Proton) | emulation | would run the PC build | avoid: bs-arm64 measured 2x the CPU time and 31 % reprojection [src]; our renderer is CPU-heavy |

The Windows, Linux x64 and Native builds are not interchangeable: Native differs in defaults (0.75x fixed resolution,
5° FOV margin, standalone upscaler and synthesis defaults, mobile GPU motion path, smaller shadow maps) [code].

### 3.2 Capabilities (verified vs claimed)

| Item | Value | Source |
|---|---|---|
| SoC / GPU | Snapdragon 8 Gen 3, Adreno 750, Mesa Turnip | docs/VR_STEAM_FRAME.md [log: Lepton log] |
| Memory | 16 GB RAM (+8 GB swap) | FramePort [claim] |
| Default eye resolution | 1728x1728 | Valve vrpreferences page [src] |
| Refresh rates | 72 (default), 80, 90, 96, 108, 120, 144 Hz | FramePort DRM read [claim]; Valve default 72 [src] |
| Refresh rate control | `XR_FB_display_refresh_rate` offered (Lepton log); so the game's request works there | [log], FramePort [claim] |
| OpenXR | 1.0 only on Linux; `XR_EXT_frame_synthesis`, `XR_FB_space_warp`, `XR_KHR_composition_layer_depth`, `XR_META_recommended_layer_resolution`, `XR_EXT_eye_gaze_interaction`, FB/META foveation set listed by Valve | [src: Valve custom engines], FramePort [claim] |
| Swapchain formats | `R8G8B8A8_SRGB`, `B8G8R8A8_SRGB` only | [log] |
| GPU clock | 231-903 MHz in 12 steps; 48 thermal zones | FramePort [claim] |
| Thermal behaviour | foveation "does not guarantee lower temperature at uncapped FPS" | FramePort [claim] |

### 3.3 vrpreferences.json

Valve's spec [src: [vrpreferences](https://partner.steamgames.com/doc/steamhardware/steamframe/vrpreferences)]:
`vrpreferences.json` at the package root, a `steam_frame` dictionary with `preferResolution` (one integer for both
axes; default 1728), `preferMinRefreshRate` (Hz; default 72), `preferHalfFramerate` (bool; default no throttling),
`preferMotionSmoothingMode` (`off`, `on`, `alwaysOn`; default the global setting). Read at install or SteamVR start;
**a user's per-app override stays until they press Reset to Default**, so the file never overwrites a user's choice;
validated by "Loading app-provided preferences from" in `vrserver.txt`.

**Recommendation: do not ship one until the matrix in section 11.3 is run.** Reasons:
- `preferHalfFramerate` and our own half-rate modes overlap: Auto / Half already draw 36 fps at 72 Hz, and with
  `XR_EXT_frame_synthesis` the runtime paces half itself. Setting SteamVR's throttle too could stack two decisions
  [est].
- `preferMotionSmoothingMode` interacts with our app motion vectors in an undocumented way; QuakeVR's result and our
  smeared run argue for `off` until A/B'd [est].
- `preferResolution` sets the swapchain size; the game draws at 0.75x of its internal resolution and upscales into
  it, so a larger swapchain costs only the upscale and the compositor [code], while the performance assessment's
  "effective resolution" sees the swapchain [est: whether it reads the app's internal resolution is not documented].

Draft to test (not packaged):

```json
{
  "steam_frame": {
    "preferResolution": 1728,
    "preferMinRefreshRate": 72,
    "preferHalfFramerate": false,
    "preferMotionSmoothingMode": "off"
  }
}
```

### 3.4 Performance assessment and perfcriteria.txt

[src: [perf criteria](https://partner.steamgames.com/doc/steamhardware/steamframe/compat/perf_criteria)] Enable
SteamVR > Advanced Settings Show > Developer > "Draw Performance Assessment in Headset". Status VERIFIED / PLAYABLE /
UNSUPPORTED / PENDING; average frame time over the last 32 frames against a target frame time from the minimum
required refresh rate including headroom (numbers not published on the page); dynamic resolution judged at its lowest
observed resolution for the minimum and its highest for the "above recommended" warning; repeated excursions only
(low-pass). Depth and motion warnings do not affect the badge. `perfcriteria.txt` in
`/home/steamos/.local/share/Steam/logs` (alias `cdl`): two header lines (app, app id, target frame time, effective
resolution) then the frame times of each violation. "Clear Performance Assessment Status" after loading screens.

Consequence for us: dynamic resolution's **minimum** level decides the resolution check, so a Frame profile should not
reach 0.5x; and holding 72 Hz (13.9 ms) with headroom is what is graded, not 90.

---

## 4. Foveated rendering comparison

| Option | Works with our renderer | Swapchain change | Vulkan ext | Runtime | Multiview | Per platform | Saves GPU | Artifacts / latency | Eye-tracked | With synthesis |
|---|---|---|---|---|---|---|---|---|---|---|
| Steam's per-game switch (`VK_LAYER_VALVE_fdm_injection`) | **no useful effect**: only the swapchain passes (our upscale) | — | FDM (inside the layer) | SteamOS on the Frame | n/a | Frame only | ~none for us [est] | lowers peripheral quality of the upscale | yes | unknown |
| `XR_FB_foveation` + `_vulkan` | same limitation: the runtime's density map is for swapchain images | yes (foveation create bit) | FDM | Quest, SteamVR (Valve lists it) | yes (layered map) | yes | ~none until the scene draws into swapchain images | — | with `XR_META_foveation_eye_tracked` | — |
| **App-side FDM on the scene's eye passes** | yes, with work: a density map attachment on each eye render pass, `VK_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT`-style info for dynamic rendering, subsampled images or not, pipelines unchanged | no | `VK_EXT_fragment_density_map` (+ offset for gaze without rebuilding; `VK_VALVE_fragment_density_map_layered` per-layer for multiview on Turnip [claim]) | any with the ext (Turnip, Qualcomm) | per-layer maps needed | yes | yes, tiler-friendly (bins at lower res) | samplers of subsampled images need care; post passes reading the scene must use the same density | gaze from `XR_EXT_eye_gaze_interaction` (Frame) | depth/motion stay full-size targets: compatible [est] |
| App-side VRS (`VK_KHR_fragment_shading_rate` attachment) | yes, similar plumbing | no | KHR FSR | PC GPUs (NVIDIA/AMD/Intel); Turnip has it [claim] | yes | yes | fragment shading only (not raster/bandwidth) | coarse pixels in the periphery | yes with gaze | compatible |
| Fixed foveation (no gaze) | either app-side path with a static map | — | — | — | — | — | less than gaze (smaller low-res area allowed) | visible when looking sideways | no | — |
| Lower `resScale` / dynres | already built | — | — | — | — | — | yes, uniformly | blurs the centre too | — | — |

**Wind Waker specifics.** The look is flat colours with strong dark edges and high-contrast silhouettes; FDM halves
or quarters the shading density per tile, so edges in coarse tiles widen and stair-step, and on a tiler the coarse
area snaps to bins (framefov's note) [src + est]. Rules for our backend:
- **UI never foveated**: in per-eye mode the game's UI, the wrist HUD, item wheel and laser are quad layers sampled by
  the compositor; they are outside the eye passes and outside any density map [code]. The shared-projection mode
  still draws the UI into the eye picture: foveation must be per-eye-mode only, or the UI target excluded.
- **Mild maps**: full density over a generous centre (the Frame's sweet spot plus eye-tracking error), 1/2 in a ring,
  1/4 only at the far edge; no 1/16 tier (Alyx's PC rings go to 1/16 [claim]; too coarse for outlines).
- **Post passes** (bloom, FXAA, AO, the telescope eyepiece) read the scene picture: with subsampled images they must
  sample with the subsampled flags, or use non-subsampled targets (simpler, saves less).
- **Shadow maps, reflections, the sun atlas** are not eye passes: never foveated.
- **One system at a time**: if the app's own FDM is on, Steam's switch must be off (both would apply to different
  passes, but the upscale pass would then be foveated twice in effect) [est]; log both (the new layer log line).

**Recommendation**: P3 work. Platform backends behind one setting `vr.foveation` = off | fixed | eye (auto: eye where
`XR_EXT_eye_gaze_interaction` and FDM-offset exist): FDM on Adreno (Frame, Quest), KHR VRS on PC. Expected gain is
GPU fragment time only; on the Frame logs that looked GPU-bound it is worth measuring, on the Quest (CPU-bound) it
gives little [log + est].

---

## 5. Dynamic resolution recommendation

The existing controller already meets most of the brief: GPU time from timestamps (not CPU), the session's paced
rate as the budget, half-second windows (no single-spike reaction), level steps with hysteresis and a doubling settle
time, gradual recovery, no step down when CPU-bound, configurable min (`vr.dynresMin`), pre-created level images
[code]. Gaps:

1. **Standalone ceiling** (section 0): profiles reach 1.0x where the fixed default is 0.75x. Proposal: on standalone
   builds the profile's maximum is the fixed factor unless the user raises it (Custom). Needs the user's decision
   because docs/VR_DYNAMIC_RESOLUTION.md describes the current ceiling as intended.
2. **Frame synthesis budget**: with synthesis the paced rate is half (36 at 72 Hz); dynres's `target_fps` follows
   interp's output rate, so it judges against 36 [code]. Correct, but an overloaded window there is two refreshes
   late: the minimum level should be reached faster (shorter first window) [est].
3. **Valve's check**: the minimum level counts; Frame profile minimum 0.625x of internal [src + est].
4. **Runtime recommendation**: `XR_META_recommended_layer_resolution` (offered on the Frame [claim]) could cap the
   maximum; not used [code].
5. **Per-eye**: both eyes share one scale; keep it (stereo-consistent sharpness).

Proposed ranges (to be confirmed by the matrix in section 11; scale of the internal resolution):

| Platform | Default | Range | Notes |
|---|---|---|---|
| High-end PC VR (RTX 4080+) | off; resScale 2-3x | Quality 0.875-1 | DLAA where offered |
| Mid-range PC VR | Balanced | 0.75-1 | SGSR1 or FSR1 when below 1 |
| Quest 3 | off (multiview + Qualcomm driver rule) | — | CPU-bound: dynres saves nothing [log] |
| Steam Frame Native | fixed 0.75x | 0.625-0.75 (proposed) | GPU-bound [log]; frame synthesis A/B |

---

## 6. Anti-aliasing and graphics quality

| Method | Cost | Fit for Wind Waker VR |
|---|---|---|
| Supersampling (`resScale` > 1, PC) | high, scales with pixels | best edges and text; already the PC path (PS VR2 at 3x) [log] |
| DLAA / DLSS (PC RTX) | moderate; needs motion vectors | stable edges; ghosting risk on particles; already built [code] |
| FXAA (game's own) | low | softens outlines and fine texture; shimmer reduction is small in VR; UI no longer affected in per-eye mode [code] |
| SGSR1 (spatial, sharpening) | ~0.3 ms on SD 8 Gen 2 [src: Qualcomm] | default on the Frame; sharpness 2 can halo dark outlines: test 1.0-1.5 |
| SGSR2 / TAA-like | moderate; temporal | ghosting on flat-colour edges in motion, history reset at dynres changes [code]; keep by choice |
| MSAA | needs new multisampled targets and resolves for every GX2 colour/depth target and shader reads of depth | high renderer risk; not recommended now |

Other: anisotropic 16x is cheap on desktop and worth testing at 4x-8x on Adreno (texture bandwidth); shadow maps at
0.75x on standalone (done); bloom and AO are per eye full-screen passes, candidates for half resolution on standalone
[est, measure first]; artistic mips keep painted mip chains [code].

**Presets** (proposed; each line to be validated):

| | High-end PC VR | Mid PC VR | Quest 3 | Steam Frame Native |
|---|---|---|---|---|
| Internal resolution | 2-3x | 1.5x | 1x at 0.75 | 1x at 0.75 |
| Dynamic resolution | off / Quality | Balanced | off | off until A/B (then 0.625-0.75) |
| Upscaler | DLAA (RTX) / blit | SGSR1 or FSR1 | Quest Super Resolution | SGSR1, sharpness 1.5 |
| FXAA | off (supersampled) | on | off | test |
| Anisotropic | 16x | 16x | 4x (test) | 8x (test) |
| Shadows | 1x+ | 1x | 0.75x | 0.75x |
| Frame rate | Auto (90) | Auto | Auto (72/90) | Auto at 72 Hz (36/72) |
| Frame synthesis | off (PC) | off | test vs off | **off until the A/B in 11.3** (QuakeVR, our smeared run) |
| Foveation | none (later VRS) | none | none (later FDM) | Steam's switch off; later app FDM |

---

## 7. CPU and GPU bottlenecks (ranked)

| # | Bottleneck | Evidence | CPU/GPU | Visual impact | Effort | Regression risk |
|---|---|---|---|---|---|---|
| 1 | Frame synthesis artifacts on the Frame (smearing, tearing) | [log: player video]; QuakeVR [src] | both | severe | low (defaults) to high (quality) | low for defaults |
| 2 | Standalone dynres ceiling above the fixed 0.75x | [code] | GPU | late frames -> synthesis from stale data | low | low |
| 3 | Standalone CPU: render thread translating GX2 per draw, both eyes, lock waits 1.5-3 ms (atlas thread) | Quest 3 [log]; notes fix 6 | CPU | 30 fps on Quest | high | medium |
| 4 | Motion pass draws the depth-writing geometry again (CPU + GPU), only while synthesis/SGSR2/DLSS use it | [code]; cost on Turnip unmeasured | both | — | medium | medium |
| 5 | Loading freezes of several seconds on the Frame | [log: player video]; shader/pipeline compiles (`pipeline_worker.cpp`), stage loads on the game thread | CPU | freeze (headset_frame re-shows) | medium | low |
| 6 | 72/80 Hz "full" drawn at 60 (no schedule): uneven | [code] | — | judder at full rate | medium | medium |
| 7 | Per-eye full-screen post (bloom, AO, FXAA) at full eye size | [code] | GPU | — | medium | low |
| 8 | Symmetric field waste | fixed by per-eye projection (default) | GPU | — | done | — |

Queue idles in the frame path: `backend.cpp` present waits with `vkQueueWaitIdle` only when presentation is not
asynchronous (`WWHD_VK_ASYNC_PRESENT=0`, or a present capture is requested); asynchronous presentation is the default
[code: `async_present`, the present function's `if (!async)`]. Android also idles once when a desktop swapchain is
new. Other idles are at resize, surface recreation, the session's stop and the texture-pack uploader's stop [code].
No per-frame allocations in the frame-synthesis export (descriptor sets from the frame's pool; image views cached)
[code].

### 7.4 The cropped-eye motion fix (implemented)

`openxr_session.cpp` `end_frame`: the EXT's `motionVectorScale` is now `(image width / submitted width, image height /
submitted height)` per eye. The motion image samples the colour image's crop (`part`), the view's FOV is the crop's,
and NDC over the crop spans `full/part` times as much as over the whole image, so the old scale of 1 under-reported
motion. FB space warp has no scale field; it remains off-by-a-factor there only with the shared projection and
`vr.eyeCrop` on, and only on runtimes without the EXT (comment added).

---

## 8. Frame pacing and reprojection

**Why ~90 fps can still judder** (known causes in this codebase, each with its guard):

| Cause | Guard | Status |
|---|---|---|
| 30 Hz logic vs 90 fps frames | interpolation; even schedules; clock locked to the headset | [code], PS VR2 [log] |
| 60 fps on 90 Hz (1-2 refresh alternation) | Half (45) or Auto's half instead of Unlocked | [code] |
| Frames near the display deadline | `note_display` moves the grid half a refresh | [code] |
| A normal-priority end-frame thread put off | boosted priority | [log: PS VR2 2026-11-02] |
| SteamVR in a bad state (xrWaitFrame p50) | restart SteamVR before A/Bs | project memory |
| Re-shown frames during stalls showing an older turn angle | wait 3 periods / 2x interval | [code], headset [log] |
| Synthesized frames extrapolating sword and hand motion | synthesis off on PC; on the Frame A/B | section 3.3 |

- `xrWaitFrame` / `xrBeginFrame` on the render thread at frame start, `xrEndFrame` on its own thread; each frame
  submitted with the poses it was drawn with; the predicted display time comes from xrWaitFrame and the game thread
  locates at the prediction plus one frame (`take_camera_ahead` counts how often that changes) [code].
- Rendering and simulation already run independently in the only safe way here: logic stays at 30 Hz (project rule;
  the game's physics, timers and collisions count in steps), frames are interpolated draws. Input latency: controller
  poses are read every frame for the camera, arms and laser; buttons act at the next logic step (≤ 33 ms) [code].
- **Native vs reprojected**: native frames show the sword and hands where they were drawn; runtime reprojection only
  corrects the head; synthesis extrapolates every pixel from motion vectors, so a fast swing (non-linear motion) and
  transparent effects (no per-pixel motion) smear. For a sword game, full rate is preferred; synthesis only as a
  fallback below the full rate [est, consistent with Meta's guidance].
- **Refresh rates**: the menu offers 60/72/80/90/120 (`vr_config.h` `kRefreshRates`), asks the runtime only where it
  lets the game choose, never substitutes, and logs a rate not offered; Detect Headset reads what is offered [code].
  The Frame offers 96/108/144 too [claim], which the menu cannot select. Recommendation: build the choices from
  `xrEnumerateDisplayRefreshRatesFB` when offered (P2).

---

## 9. VR player experience

| Area | State | Opportunities |
|---|---|---|
| Head tracking | one writer, views id per frame, pose at prediction + 1 frame [code]; PS VR2 "everything felt good" [log] | measure motion-to-photon via the runtime's timing on the Frame |
| Camera stability | bob filtered, no bob in conversations, held above water [code] | horizon is the head's own (no roll added) [code] |
| Cutscenes | the game's camera turned by the head (`vr.cutsceneFollow`) [code] | offer a fixed theatre screen for long cutscenes (comfort) |
| Lock-on | game's; grappling-hook lock-on added [docs] | — |
| Sword | tracked blade drives the game's capsules; game's state machine decides attacks [code] | haptic strength per hit type |
| Shield | on its handle in the left hand [git log] | — |
| Bow / hookshot / boomerang / grapple | pointing with a laser quad; arrow along the bow; draw by grip [docs] | headset checks listed in docs/VR_AIMING.md |
| Telescope / Picto Box | eyepiece crop; Picto Box mode [docs] | the eyepiece disables SGSR1 (blit) [code] |
| Comfort | smooth/snap turn, recentre, head offset limit, cutscene follow [code] | vignette during smooth turns (not built) |
| Wrist UI | matches the intended layout: hearts on top of the left wrist, items under it (palm side), rupees on top of the right; each a quad layer of one CPU picture, shown when facing the eyes [code, docs/VR_WRIST_HUD.md] | readability on the Frame (about 30 px per cm on the hearts panel) [est]; quads are compositor-sampled, so foveation and dynres never touch them [code] |
| Game UI | per-eye mode: one head-locked quad (no per-eye duplicates, no disparity errors) [code]; it no longer blinks on re-shown frames (fix 2) | depth-aware placement of dialogue (currently fixed 3.5 m) |

---

## 10. Platform capability abstraction (recommended)

One struct filled once per session, consumed by the existing `choose` functions, replacing scattered `#if`s and
runtime checks:

```text
vr::Platform {
  build: windows_x64 | linux_x64 | steamframe_native | steamframe_lepton | android_xr     (compile time)
  runtime: steamvr | meta | vdxr | other; headset: frame | quest | psvr2 | pc_other          (xrGetInstanceProperties, system name)
  gpu: vendor, VkDriverId (turnip, qualcomm, nvidia, ...), multiview, fdm, fdm_offset, fdm_layered, khr_fsr
  xr: frame_synthesis_ext, fb_space_warp, depth_layer, display_refresh_rate (+ the list), recommended_layer_resolution,
      eye_gaze, fb_foveation(_vulkan), meta_foveation_eye_tracked, composition_layer_settings, cylinder
  preset: defaults derived from the above (section 6), applied only where settings.ini has no value
}
```

Rules: detection at run time (extensions, VkDriverId, system name), never by GPU model; the build target only for
what cannot be detected (toolchain, packaging); every derived default logged with its reason (as `[vr] upscaling:`
does today); the user's saved value always wins (project rule: settings.ini overrides code defaults).

---

## 11. Benchmarking and validation

### 11.1 What the game already logs every 10 s [code: `log_stats`]

Frames/s to the headset, headset Hz, xrWaitFrame p50/p99, render thread after xrWaitFrame p50/p99, interval
p50/p95/p99/max, missed display periods, uneven frames (game time per frame), submission (acquire, queue, release +
xrEndFrame), end-frame thread waits, VR game-thread work, camera prediction counts, dynres status. Plus
`[dynres]`, `[vr frame synthesis]`, `[vr] upscaling:` change lines, `[motion]` stats.

### 11.2 Tools per platform

| Platform | Tools |
|---|---|
| Steam Frame | Performance Assessment overlay; `perfcriteria.txt`; `vrserver.txt` (vrpreferences, "Using motion encoding AppMVs"); "Draw Performance Assessment Game Textures in Headset" (our depth and motion); Mesa `fdperf` (Adreno counters); `TU_DEBUG` options (e.g. `sysmem` A/B, as QuakeVR) |
| Quest 3 | OVR Metrics Tool; `debug.oculus.spaceWarpDebug`, `MVOverlay` (docs/VR_FRAME_SYNTHESIS.md); logcat |
| PC | SteamVR frame timing; RenderDoc (desktop stereo); NVIDIA Nsight Graphics; the game's GPU timestamps |

### 11.3 Steam Frame test matrix (each a headset run; ask before each launch)

Same save, same route, 3 minutes, SteamVR restarted first, `WWHD_LOG_FILE=1`, logs backed up:

| Run | Resolution | Dynres | Upscaler | Frame synthesis | Steam foveation | Expect / check |
|---|---|---|---|---|---|---|
| F0 | fixed 0.75x | off | SGSR1 1.5 | off | off | baseline: fps, % at 72, overlay status |
| F1 | fixed 0.75x | off | SGSR1 1.5 | auto | off | smearing? `AppMVs` line; overlay motion warning gone |
| F2 | fixed 0.75x | Performance | SGSR1 1.5 | auto | off | reproduces the video? `[dynres]` changes vs smearing times |
| F3 | fixed 0.75x | Custom 0.625-0.75 | SGSR1 1.5 | auto | off | the proposed ceiling |
| F4 | F0 | | | | on | does Steam's switch change anything for us (log line) |
| F5 | F0 with `VK_LOADER_LAYERS_DISABLE=*rpo*` | | | | | RPO's cost or faults |

Scenes: Outset Island (water, grass, characters), Windfall (many characters, UI), sailing at speed (sea, head turns),
Forsaken Fortress or a combat room (particles, sword), a dialogue (UI layer), a loading door (freezes).

Success: VERIFIED/PLAYABLE status; ≥ 99 % frames on time at the chosen rate; no visible smearing on static cliffs
during head turns; dynres minimum ≥ 0.625x.

---

## 12. Changes implemented (this audit)

| Change | Files | Why | Validation |
|---|---|---|---|
| Motion vectors scaled for a cropped eye (`motionVectorScale`) | `runtime/src/vr/openxr_session.cpp` | the motion was too small by crop/full whenever an eye was submitted as part of its image | built [build]; the default per-eye path keeps scale 1; needs a headset run with `WWHD_VR_EYE_PROJECTION=shared vr.eyeCrop=on` to see it |
| The game's UI layer on frames shown again | `runtime/src/gfx/vulkan/vr_vulkan.cpp` (`Shown`, `frame_end`, `headset_frame`) | the UI quad was left out of `headset_frame`'s xrEndFrame: the HUD, text and menus blinked off during loads and shader waits | built [build]; needs a headset run through a load with text on screen |
| Log of Valve's injected layers | `runtime/src/gfx/vulkan/vr_vulkan.cpp` (`create_instance`) | a Frame log now says whether `VK_LAYER_VALVE_rpo` / `fdm_injection` and `FDM_DEBUG` were in the process | built [build]; logs only when one is set (PC logs unchanged) |

Build: `cmake --build . --target wwhd --target wwhd-vr-launcher` in `build/game`: exit 0 (existing warnings only),
2026-11-03. No tests run (project rule). Nothing committed. The working tree also holds another session's uncommitted
streamed-headset changes (`interp*`, `openxr_session.h/.cpp`, `interp_pacing_test.cpp`, `docs/VR_STREAMING.md`); the
build included them, and this audit's edits touch none of their lines.

## 13. Remaining issues and hardware validation

- All Steam Frame items: the matrix in 11.3; arm64 builds of these changes (not built here: no arm64 toolchain).
- Quest 3: the end-frame thread's effect on the 30 fps CPU bound; the both-eyes motion pass on Adreno.
- Decisions for the user: the standalone dynres ceiling (5.1); frame synthesis default on the Frame (section 6);
  whether to ship `vrpreferences.json` after the matrix (3.3).
- Not done: foveation (P3), refresh rates from the runtime's list (P2), 72/80 fps full-rate schedules (P2),
  lens hidden-area mask for depth (QuakeVR's finding, P2).

## Sources

- [bs-arm64](https://github.com/DaVarga/bs-arm64), [bs-manager steam-frame.md](https://github.com/DaVarga/bs-manager/blob/bs-arm64/docs/steam-frame.md)
- [QuakeVR PR #75](https://github.com/vittorioromeo/quakevr/pull/75)
- [Valve: vrpreferences](https://partner.steamgames.com/doc/steamhardware/steamframe/vrpreferences),
  [Valve: performance criteria](https://partner.steamgames.com/doc/steamhardware/steamframe/compat/perf_criteria),
  [Valve: custom engines](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom)
- [SteamOS#2973 (RPO layer crash)](https://github.com/ValveSoftware/SteamOS/issues/2973), [framefov](https://github.com/wakira/framefov),
  [FramePort runtime reference](https://frameport.app/docs/frame-runtime/)
- Half-Life: Alyx: [mixed-news](https://mixed-news.com/en/half-life-alyx-runs-standalone-steam-frame-arm-port/),
  [GamingOnLinux](https://www.gamingonlinux.com/2026/09/half-life-alyx-ported-to-arm64-for-steam-frame-update-out-now/),
  [vr.org](https://vr.org/articles/half-life-alyx-foveated-rendering-pc-eye-tracking-13-headsets-2026)
- [Turnip foveation at XDC 2026 (report)](https://www.neoteo.com/en/connor-abbott-presented-valves-reported-steam-frame-foveated-rendering-work-at-xdc-2026)
- HL2VR: [HL2VRU fork](https://www.Github.com/vittorioromeo/HL2VRU), [Road to VR](https://roadtovr.com/half-life-2-vr-mod-coming-to-oculus-rift-and-htc-vive/)

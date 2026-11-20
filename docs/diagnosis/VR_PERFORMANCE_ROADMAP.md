# VR performance roadmap (2026-11-03)

The evidence and reasoning for each item are in
[STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md](STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md). Project rules apply to every item:
the 30 Hz logic never changes, graphics stay exact unless a setting says otherwise, multiview stays on, tests are
headset-only and asked for first, and saved settings (`settings.ini`) always win over new defaults.

"Measure" uses the game's 10 s `[vr]` timing lines (fps to the headset, % of frames on time, interval p99, missed
periods, uneven frames, xrWaitFrame p50), plus the Steam Frame Performance Assessment and `perfcriteria.txt` there.

---

## Priority 1: high impact, low risk

### 1.1 Done: motion vectors for a cropped eye
- **Files:** `runtime/src/vr/openxr_session.cpp` (`end_frame`, `XrFrameSynthesisInfoEXT::motionVectorScale`).
- **Change:** scale = image size / submitted part per axis (1 when the whole image is submitted).
- **Why:** with `WWHD_VR_EYE_PROJECTION=shared` and `vr.eyeCrop` on, the motion was too small by part/full, so
  synthesized frames lagged moving pixels.
- **Risk:** none on the default path (scale 1). FB space warp has no scale field (commented).
- **Test:** Quest 3 or Frame, shared projection + eye crop, frame synthesis on, head turns past a cliff.
- **Success:** no lag trail on static geometry during turns; the default path unchanged.

### 1.2 Done: the game UI layer on frames shown again
- **Files:** `runtime/src/gfx/vulkan/vr_vulkan.cpp` (`Shown`, `frame_end`, `headset_frame`).
- **Why:** `headset_frame` re-submitted every layer but the UI quad, so the HUD, text and menus blinked during loads and
  shader waits.
- **Risk:** low (the swapchain's last released image, like the eyes').
- **Test:** walk through a loading door and open a dialogue during a shader-heavy first visit.
- **Success:** no UI blink; `headset_frame` count in the `[vr] timing` line unchanged.

### 1.3 Done: log Valve's injected layers
- **Files:** `vr_vulkan.cpp` `create_instance`.
- **Why:** a Frame log must say whether `VK_LAYER_VALVE_rpo` / `fdm_injection` and `FDM_DEBUG` were present.
- **Test:** the next Frame log has `[vr] Vulkan layers from the environment: ...`.

### 1.4 Done: Steam Frame frame synthesis off by default until A/B'd (the user's choice, 2026-11-03)
- **Files:** `vr_config.h` (`kSteamFrameBuild`, the `vr.frameSynthesis` default), `vr_test.cpp`, docs/VR_FRAME_SYNTHESIS.md, docs/VR_STEAM_FRAME.md.
- **Why:** the smeared player run had synthesis on; QuakeVR saw artifacts and ships it off; Valve's badge does not
  need it (the motion/depth warnings do not affect the badge).
- **Risk:** low; loses half-rate smoothing where the GPU cannot hold 72. Quest unchanged unless chosen.
- **Test:** audit 11.3 runs F0 vs F1.
- **Success:** F0 holds ≥ 99 % frames at 72 Hz (36 fps Auto half otherwise) without smearing.

### 1.5 Done: standalone dynamic resolution ceiling at the fixed factor (the user's choice, 2026-11-03)
- **Files:** `runtime/src/vr/vr_dynres.h` (`profile`, a `Device::fixedMax`), `vr_vulkan.cpp`
  (`update_dynamic_resolution`), `docs/VR_DYNAMIC_RESOLUTION.md`.
- **Change:** on standalone builds the profile's maximum is `dynres::fixed_factor()` (0.75x) unless Custom raises it.
- **Why:** today turning dynres on raises standalone resolution from 0.75x to up to 1.0x and overloads the GPU first.
- **Risk:** low; a headset with room loses the chance to climb above 0.75x by default.
- **Test:** F2 vs F3 in audit 11.3; `[dynres]` change lines vs smearing times.
- **Success:** no step up past 0.75x without Custom; fewer `[dynres]` swings; no smearing.

### 1.6 Done: the documentation mismatch about Lepton's driver
- **Files:** `docs/VR_DYNAMIC_RESOLUTION.md` ("Where it runs"), `vr_dynres.h` comment.
- **Why:** Lepton's Vulkan is Turnip (docs/VR_STEAM_FRAME.md); the code already decides by `VkDriverId`.
- **Test:** none (docs).

### 1.7 Done: Steam Frame launch advice (docs/VR_STEAM_FRAME.md)
- Tell players to leave Steam's per-game **Foveated Rendering** off for this game: Valve's layer foveates only the XR
  swapchain, which here receives the upscale, not the scene.
- If a Frame run shows GPU faults: retry with `VK_LOADER_LAYERS_DISABLE=*rpo*` (SteamOS#2973).
- **Files:** `docs/VR_STEAM_FRAME.md`.

---

## Priority 2: high impact, moderate effort

### 2.1 Refresh rates from the runtime's list
- **Files:** `vr_config.h` (`kRefreshRates`, `supported_refresh_rate`), the F1 VR menu, launcher Detect Headset,
  `openxr_session.cpp` (`xrEnumerateDisplayRefreshRatesFB`).
- **Change:** offer exactly the rates the runtime lists (the Frame: 72/80/90/96/108/120/144 reported); fixed list only
  where the runtime does not let the game choose (SteamVR on PC: show the running rate).
- **Risk:** settings migration (keep a saved rate the runtime still offers; never rewrite the file).
- **Test:** Frame and Quest menus list the runtime's rates; choosing 96 asks for 96 (log).
- **Success:** no rate offered that the runtime rejects.

### 2.2 Parked: full-rate schedules for 72 and 80 fps (the user, 2026-11-03: until a Frame run holds 72 fps)
- **Files:** `interp_pacing.h`, `interp.cpp`, `interp_pacing_test`.
- **Why:** "Capped 72/80" draw at 60 today: uneven on 72/80 Hz headsets (the Frame's default is 72).
- **Risk:** high, several days (found 2026-11-03): at 72/80 fps most steps end on a frame between two game states, so
  their *last* pass must draw blended while recording the exact state. The 36/40/45 patterns allow that only on a step's
  first (logic) pass (`g_record_logic`, about 85 sites in `interp.cpp` and `interp_fx.cpp`: models, camera, cloth, sea,
  effects); generalizing it needs desktop picture comparisons. A cheaper variant (each step ending exact, 2 or 3
  frames a step) gives a new frame every refresh but uneven game time (about ±20%).
- **Why parked:** the Frame ran 36 fps at 0.75x; 72 fps full rate is out of reach until that changes.
- **Test:** unit tests; Frame at 72 Hz Capped 72: `uneven` count in the timing line.
- **Success:** uneven frames < 1 % at full rate.

### 2.3 Lens hidden-area mask in the exported depth and motion
- **Files:** `gfx/vulkan/vr_framesynth.cpp` (export shaders), `openxr_session.cpp`
  (`XR_KHR_visibility_mask` if offered).
- **Why:** QuakeVR found the hidden area (15.65 % of each eye) was reprojected as a near surface.
- **Risk:** low (export only).
- **Test:** "Draw Performance Assessment Game Textures in Headset" shows far depth in the hidden area.

### 2.4 Done: faster first reaction of dynres at half rate (quarter-second windows, `dynres_core.h` `window_seconds`)
- **Files:** `dynres_core.h`, `vr_dynres.h`.
- **Why:** at 36 fps a late frame costs two refreshes; reach the minimum level sooner.
- **Test:** `WWHD_DYNRES_TEST_BUDGET` on the desktop; Frame F3.
- **Success:** time from overload to a stable level < 1 s; no oscillation.

### 2.5 The standalone CPU bound (Quest 3)
- **Files:** `draw.cpp`, `buffer_cache.cpp`, `shaders.cpp`, `surfaces.cpp` (lock sites from `WWHD_VK_LOCK_COUNT=1`),
  `vr_vulkan.cpp` end-frame thread on Android.
- **Why:** ~30 fps at every refresh rate; render thread waited 13-17 ms in xrEndFrame; atlas-thread lock waits
  1.5-3 ms/frame.
- **Risk:** medium (threading).
- **Test:** Quest 3 log with the end-frame thread (default on) vs `WWHD_VR_END_FRAME_THREAD=0`; lock counts.
- **Success:** Quest 36/45 fps half-rate steady in gameplay; render thread wait p50 < 2 ms.

### 2.6 Loading freezes
- **Files:** `pipeline_worker.cpp`, `pipeline_demand.cpp`, `headstart.cpp` (pre-built pipelines), the shader disk cache.
- **Why:** multi-second freezes on the Frame while loading (player video).
- **Test:** first and second visit to the same stage; freeze length from the interval max.
- **Success:** second visits without a freeze over 100 ms; first visits re-show frames (no runtime timeout).

### 2.7 A platform capability struct
- **Files:** new `runtime/src/vr/vr_platform_caps.h`; `vr_vulkan.cpp` (`update_dynamic_resolution`, upscale caps),
  `vr.cpp` (`fov_margin`), `vr_config.h` (`kPcVrBuild`), `openxr_session.cpp`.
- **Change:** one struct filled at session start (audit section 10); the existing `choose` functions take it.
- **Risk:** low if behaviour-neutral (log every derived default, compare logs before/after).
- **Success:** identical `[vr]` start-up decisions on PC; one place to read for each platform.

---

## Priority 3: advanced platform work

### 3.1 App-side foveation (fragment density maps on the eye passes)
- **Files:** `pass.cpp` / `pass.h` (dynamic rendering info), `surfaces.cpp` (density map images, per layer for
  multiview), `pipeline_recipe.cpp` (subsampled flags where needed), `stereo.cpp`, `vr_vulkan.cpp`, new
  `vr_foveation.cpp`; `openxr_session.cpp` for gaze (`XR_EXT_eye_gaze_interaction`).
- **Change:** `vr.foveation` off | fixed | eye; FDM on Adreno (Frame, Quest), KHR fragment shading rate on PC; never
  on UI, shadow maps, reflections, the atlas; mild maps (no 1/16 tier).
- **Risk:** high (every scene pass; post passes reading subsampled images; multiview per-layer maps).
- **Test:** desktop picture comparisons with a fixed map (centre identical to off), then the Frame: GPU busy share,
  outline and text inspection in the periphery while looking around.
- **Success:** ≥ 15 % less GPU time on the Frame at equal resolution with no visible change at the gaze point.

### 3.2 Eye-tracked foveation
- Builds on 3.1 with gaze; FDM offset (`VK_QCOM_fragment_density_map_offset`-style) to move the map without rebuilding.
- **Test:** gaze latency (fast saccades), dropouts falling back to fixed.

### 3.3 Frame synthesis quality
- Motion z from the depth change (EXT's 3D); per-pixel motion for transparent effects where one dominates; motion for
  batched/instanced draws (the motion pass skips them today).
- **Files:** `motion_pass.cpp`, `motion_glsl.cpp`, `vr_framesynth.cpp`.
- **Success:** F1 indistinguishable from F0 in head turns; sword swings acceptable.

### 3.4 Turnip specifics
- `TU_DEBUG=sysmem` A/B (QuakeVR: ~3 % faster, hang workaround), GMEM-friendly load/store ops, `fdperf` counters
  for bandwidth.
- **Success:** a measured choice, documented per driver version.

### 3.5 `vrpreferences.json` for the Native package
- **Files:** `tools/release/package.py` (`--target steamframe-native`), a new `packaging/steamframe/vrpreferences.json`.
- Only after the audit's 11.3 matrix decides each key; verified with `vrserver.txt`'s "Loading app-provided
  preferences from".

---

## Priority 4: visual and gameplay polish

| Item | Files | Test / success |
|---|---|---|
| Done: SGSR1 sharpness default 1.5 on the Frame (2 may halo outlines) | `vr_config.h` | outlines and text at 1, 1.5, 2; choose by eye |
| Anisotropic filtering per preset (8x on the Frame) | settings presets | GPU busy share vs texture shimmer on the sea and paths |
| Half-resolution bloom/AO on standalone (measure first) | post passes | GPU time; no visible change at the gaze point |
| Dialogue placed at the speaker's distance (UI quad depth) | `vr_vulkan.cpp` `frame_end` | readable, no depth conflict with the scene |
| Comfort vignette for smooth turning | `vr_locomotion.h`, a quad layer | opt-in; comfort reports |
| Theatre screen option for long cutscenes | `vr_game.cpp` | opt-in; no forced camera motion |
| Wrist HUD sizes on the Frame | `vr_wrist_hud.h` | readable at arm's length |

---

## Validation status

| Item | Built here | Run on PC | Needs the headset |
|---|---|---|---|
| 1.1, 1.2, 1.3 | yes (Windows x64, `build/game`, exit 0) | no (no tests asked) | yes (Frame/Quest for 1.1, any for 1.2, Frame for 1.3) |
| 1.4, 1.5, SGSR1 1.5 | Windows x64 builds (the Frame branches compile only in arm64 builds, not built here); `vr_test` compiled, not run | no | yes (Frame: matrix in the audit's 11.3) |
| everything else | — | — | yes |

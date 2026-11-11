# Quest 3 standalone VR ports: research (2026-11-03)

How successful native Quest ports reach their frame rates, and what applies to ZeldaWWHD-VR. Marks: **[src]** read in
the project's source (cloned, read-only), **[readme]** its README or write-up, **[claim]** the author's statement
without a published method, **[est]** our inference. Companion reports: [QUEST3_PERFORMANCE_AUDIT.md](QUEST3_PERFORMANCE_AUDIT.md),
[QUEST3_GRAPHICS_OPTIMIZATION.md](QUEST3_GRAPHICS_OPTIMIZATION.md), [QUEST3_VR_EXPERIENCE_AUDIT.md](QUEST3_VR_EXPERIENCE_AUDIT.md),
[QUEST3_IMPLEMENTATION_ROADMAP.md](QUEST3_IMPLEMENTATION_ROADMAP.md), [QUEST3_BENCHMARK_RESULTS.md](QUEST3_BENCHMARK_RESULTS.md).

## Summary

| Project | Engine | Graphics API | VR API | Stereo | Key performance techniques | Applies to us |
|---|---|---|---|---|---|---|
| [Ocarina of Time VR (Shipwright-VR)](https://morganblairmakes.com/work/quest-3-oot) | Ship of Harkinian (PC port of OoT) | OpenGL ES 3 | OpenXR | `GL_OVR_multiview2` | compiler -O0 -> -O2 (~3x), multiview (~2x), cached lookups (~1.3x), persistent vertex buffer (~1.15x), less redundant work (~1.2x), no lock per command, half-rate world redraw; ASTC texture packs (2,133 -> 534 MB) | -O3 already; multiview is our biggest open item; half rate already (Auto); ASTC is a texture-pack topic |
| [Doom3Quest](https://github.com/DrBeef/Doom3Quest) | dhewm3 / d3es-multithread | OpenGL ES 3.0 | (Oculus Mobile SDK era) | GLES multiview | multithreaded front-end/back-end renderer | our renderer already splits game, render, record, atlas, end-frame threads |
| [JKXR](https://github.com/DrBeef/JKXR) | OpenJK | GLES (gl4es) | OpenXR | GL multiview (`GL_OVR_multiview2` + multisampled RTT) | **`XR_EXT_performance_settings` (CPU/GPU BOOST) and `XR_KHR_android_thread_settings` (main and render threads declared)**, MSAA 2x into the swapchain, supersampling cvar, refresh rate via `XR_FB_display_refresh_rate` | **thread declaration: implemented here**; performance levels: measure first (Sustained High is the default) |
| [Lambda1VR](https://github.com/DrBeef/Lambda1VR) | Xash3D-FWGS | GLES 2 (gl4es) | Oculus Mobile SDK 1.24 | — | supersampling default 1.25, MSAA 4x, CPU level 2 / GPU level 3 | CPU/GPU levels (VrApi era) map to `XR_EXT_performance_settings` today |
| [QuestZDoom](https://github.com/DrBeef/QuestZDoom) | LZDoom 3.83a | GLES | (Oculus Mobile SDK era) | — | "reduce supersampling to 0.9" for mods | resolution scale as the main knob (we have it) |
| [Beef Raider XR](https://github.com/DrBeef/BeefRaiderXR) | OpenLara | — | OpenXR | — | (none documented) | first-person and diorama views; little performance information |

The common pattern [src + readme]: the game draws straight into the swapchain, both eyes in one multiview pass;
foveation then comes from the runtime's density map on the swapchain (FB foveation); clocks and threads are hinted to
the runtime; the main knob is a resolution scale. Our renderer differs: it emulates the Wii U's GPU (GX2 on Vulkan),
draws the scene offscreen, and is CPU-bound in translating the game's draws (audit).

---

## A. Ocarina of Time VR on Quest 3 (Morgan Blair, Shipwright-VR)

Source: [write-up](https://morganblairmakes.com/work/quest-3-oot); releases at
[ChocoTorp/Shipwright-VR](https://github.com/ChocoTorp/Shipwright-VR/releases). Not cloned: the write-up names no
source repository for the Quest renderer.

1. **Game / engine:** Ship of Harkinian (PC port of OoT) plus ShinyWindow's SoH VR mod (D3D11 + OpenVR, Windows only).
2. **Platform:** Quest 3 standalone, Android.
3. **Graphics API:** OpenGL ES 3, rewritten from D3D11.
4. **Android:** NDK; ARM pointer tagging broke the renderer's pointer checks (stripping the tag fixed missing
   scenery); the app now shuts down fully on close (relaunch crash).
5. **VR:** OpenXR, replacing OpenVR.
6. **Rendering optimizations, 8 -> 72 fps in Hyrule Field [claim, author's rounded, overlapping multipliers]:**
   multiview ~2x; -O0 -> -O2 ~3x ("the biggest single win"); cached resource lookups ~1.3x; persistent vertex buffer
   ~1.15x; less redundant work ~1.2x; no lock per command (2.3 % of the game thread); half-rate world redraw in busy
   areas with the headset reprojecting between (~2x headroom). A shader-key field left unset caused random misses and
   slow searches: zeroing it fixed it everywhere.
7. **Memory:** HD texture packs compressed to ASTC on the headset in the background (about 3 minutes, once): GPU memory
   for the pack 2,133 -> 534 MB [claim]; spare cores preload an area's textures (no freezes entering areas).
8. **Graphics quality:** HD packs kept (ASTC rather than dropping resolution).
9. **Results:** locked 72 fps in Hyrule Field [claim]; the Quest clocks down with spare power.
10. **For us:**
   - **Optimization level:** our Android build is Release and the game code is compiled at `-O3`
     (`CMakeLists.txt` `gamecode`), so that 3x is already ours. Not tried: ThinLTO (`WWHD_LTO`, off) and an `-mcpu`
     for the XR2 Gen 2 (only Apple gets one today). Both need a Quest A/B.
   - **Multiview:** our multiview is **off on Android** (black-square artifacts; most draws fell back to per eye), so
     the render thread records every draw twice on the Quest's CPU. This is the single largest gap against OoT VR.
   - **Half-rate redraw:** ours is Auto (36 fps at 72 Hz, 45 at 90) with exact interpolation of the 30 Hz logic.
   - **Lookup caches, lock per command:** our equivalents are the descriptor/pipeline caches and the measured lock
     waits (1.5-3 ms a frame on the atlas thread on Quest; notes, fix 6).
   - **ASTC:** relevant to our texture packs (QUEST3_GRAPHICS_OPTIMIZATION.md); the game's own textures are BC.

## B. Doom3Quest (DrBeef)

[repo](https://github.com/DrBeef/Doom3Quest) [readme]: dhewm3 + d3wasm on emileb's d3es-multithread; renderer moved
to GLES 3.0 "to take advantage of MultiView to dramatically improve VR rendering performance"; multithreaded front
and back end; VR arms by reverse IK, physical menus, VR weapon models (from the Fully Possessed PC mod). Quest 1/2 era.
- **For us:** front/back-end threading is what our game/render/record threads already are; multiview again.

## C. JKXR (DrBeef, Team Beef)

[repo](https://github.com/DrBeef/JKXR), cloned and read (`Projects/Android/jni/OpenJK/JKXR/android/TBXR_Common.cpp`,
`VrInput*.cpp`, `VrCvars.h`) [src]:
- **OpenXR on Android**, the open-source loader; GLES through gl4es; `GL_OVR_multiview2` with
  `GL_OVR_multiview_multisampled_render_to_texture`; colour swapchain `GL_SRGB8_ALPHA8`, `NUM_MULTI_SAMPLES = 2`
  (MSAA into the swapchain's tiles, resolved on chip); `vr_super_sampling` scales the recommended size.
- **At session begin:** `XR_EXT_performance_settings` CPU and GPU to `XR_PERF_SETTINGS_LEVEL_BOOST_EXT`; with
  `XR_KHR_android_thread_settings` on a Meta runtime, `xrSetAndroidApplicationThreadKHR` for the main thread
  (`APPLICATION_MAIN`) and the render thread (`RENDERER_MAIN`). Listens for `XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT`.
- **Refresh rate:** enumerates the rates (`xrEnumerateDisplayRefreshRatesFB`), asks for the system default (0).
- **Foveation:** a `FOVEATION_LEVEL` setting with the FB foveation extensions (into the swapchain it draws into).
- **Interaction:** a fully motion-controlled saber, collision-based blaster deflection, `vr_weapon_velocity_trigger`
  and `vr_force_velocity_trigger` (swing speed thresholds), `vr_saber_block_debounce_time`, `vr_haptic_intensity`,
  weapon alignment (`VrInputWeaponAlign.cpp`), scopes, weapon and Force wheels, gesture use and throws;
  `vr_comfort_vignette`, snap/smooth turn (`vr_turn_mode`, `vr_turn_angle`).
- **For us:** thread declaration **implemented** (section "Changes"); performance levels: Meta's default is Sustained
  High; Boost is a short burst, active only with Battery Saver off
  ([Meta](https://developers.meta.com/horizon/documentation/unreal/po-quest-boost/),
  [Unity OpenXR docs](https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.13/manual/features/performance-settings.html)),
  so a hint is worth trying around loads (P2), not as a constant. A comfort vignette and a block debounce are
  experience items (QUEST3_VR_EXPERIENCE_AUDIT.md).

## D. Lambda1VR (DrBeef)

[repo](https://github.com/DrBeef/Lambda1VR) [readme]: Xash3D-FWGS, GLES 2 through gl4es, Oculus Mobile SDK 1.24;
`supersampling` 1.25 by default, `msaa` 4, CPU level 2 and GPU level 3; physical crowbar swings (except multiplayer),
two-handed stabilising with the grip, reaching over the shoulder for the crowbar, laser aim toggle.
- **For us:** physical melee by swing (ours: the tracked sword drives the game's capsules), "reach to holster" item
  access (an idea for our item wheel), recoil off by default.

## E. Beef Raider XR

[repo](https://github.com/DrBeef/BeefRaiderXR) [readme]: OpenXR build of OpenLara; first-person and diorama views,
switchable; Mixed Reality on Quest and Pico; tank or modern controls. No performance notes.
- **For us:** a diorama/third-person VR view is an existing option of ours (third person, right stick click).

## F. QuestZDoom

[repo](https://github.com/DrBeef/QuestZDoom) [readme]: LZDoom 3.83a, Quest only; "reduce supersampling to 0.9 to get
much improved performance"; heavy mods drop frames; smooth, teleport, snap/smooth turn; no vignette.
- **For us:** resolution as the main scalability knob (ours: internal resolution and its 0.75x fixed factor).

## G. Others and platform references

- Meta's OpenXR guidance used in this audit: CPU/GPU levels
  ([Boosting CPU and GPU levels](https://developers.meta.com/horizon/documentation/unreal/po-quest-boost/)),
  `XR_KHR_android_thread_settings`
  ([spec](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrSetAndroidApplicationThreadKHR.html)),
  `XR_FB_foveation_configuration` levels none/low/medium/high and dynamic foveation
  ([spec](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_FB_foveation_configuration.html)).
- Our own Steam Frame research (STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md): QuakeVR's frame-synthesis findings and Beat
  Saber's native-vs-emulated measurements apply to standalone Adreno headsets generally.

## Not verified

- Doom3Quest's, QuestZDoom's and Lambda1VR's source were not cloned (their READMEs only); the multiview and level claims
  for them are from those READMEs.
- OoT VR's Quest renderer source: not found published; its numbers are the author's.

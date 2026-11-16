# VR frame synthesis and depth reprojection (Quest 3, Steam Frame, PC VR)

The OpenXR runtime can make the in-between frames itself: the game draws at half the headset's refresh rate (45 fps
at 90 Hz), and the compositor synthesizes every other frame from each eye's motion vectors and depth. With depth alone
it reprojects head translation as well as rotation. This document records what each runtime offers, what is built
(2026-11-01), and what is left to check in a headset. No headset run has been made with it yet.

## Runtimes (verified from the Khronos OpenXR-Inventory, Meta's and Valve's docs)

| | Quest 3 (Meta's runtime) | Steam Frame (SteamVR) | PC SteamVR (Windows, Linux) |
|---|---|---|---|
| `XR_EXT_frame_synthesis` | yes (Quest 2, 3, 3S, Pro) | yes | no ([openvr#1936](https://github.com/ValveSoftware/openvr/issues/1936) asks) |
| `XR_FB_space_warp` (Application SpaceWarp; deprecated by the EXT in SDK 1.1.58) | yes | yes | no |
| `XR_KHR_composition_layer_depth` | yes | yes | yes (used for reprojection) |
| Asynchronous TimeWarp / reprojection | always, in the compositor | always | always (plus Motion Smoothing) |

- Valve: "We strongly recommend submitting both motion vector and depth suitable for reprojection"; the Steam Frame's
  performance overlay warns when they are missing ([perf criteria](https://partner.steamgames.com/doc/steamhardware/steamframe/compat/perf_criteria)).
  Submitting them does not throttle the app by itself; users may run higher refresh rates with them.
- Meta: Positional TimeWarp is enabled automatically in OpenXR AppSW apps (from their depth); whether depth alone
  turns it on is not documented ([AppSW](https://developers.meta.com/horizon/documentation/native/android/mobile-asw/)).
- Timewarp and reprojection are the runtime's own; nothing of ours replaces or competes with them.

## Held at Capped 30 on 90 and 120 Hz (2026-11-13)

The runtime synthesizes one frame between two real ones only, so the game must draw at exactly half the refresh rate
(Meta: "render at half rate from the actual display refresh rate, for example 36 compared to 72 FPS"; 45 at 90 Hz).
Used with Capped 30 at 90 Hz, it raised the chosen 30 to 45: a Quest 3 player's log (90 Hz, Capped 30, frame synthesis
on) had 45 forced in every scene, about 24 fps drawn, the GPU at 90-99 %, the game logic slowed and 85 % of frames with
uneven game time. Now (`vr_framesynth::set_held`, from `vr_vulkan.cpp` each frame) while the frame rate chosen is
Capped 30 at a refresh rate 30 divides above 60 Hz (90, 120), frame synthesis is held: the frames carry nothing for it,
the motion pass pauses (`motion_pass::set_needed`), and the chosen 30 is kept. At 72 and 80 Hz Capped 30 is already
drawn at half (36, 40: `vr_mode_resolved`), so frame synthesis runs there as before. The log: `[vr frame synthesis] held:
...` and `used again ...` on each change. 30 fps with frame synthesis would need a 60 Hz headset mode.

## No refresh filling while the runtime synthesizes (2026-11-14, untested in a headset)

Refresh filling (`planned_repeats`: the last game frame sent again on the refreshes between game frames at 30/45 fps)
pauses while the frame just shown carried both eyes' motion vectors (`vr_framesynth::carrying()`): at 72 Hz Capped 30
(drawn at 36) and Capped 45 at 90 Hz, its plain frames would take the refreshes the runtime synthesizes. Depth only never
carries, so PC SteamVR keeps its fills. Steady 30 at 90 or 120 Hz holds frame synthesis (above; `vr.frameSynthesis`
auto or on alike: the session's synthesis swapchains decide), so its 30 is never raised to 45, its frames carry nothing
and every refresh between them is filled. The log: `[vr] refresh filling: on (frame synthesis held: ...)`, `on (no frame
synthesis)` or `paused ...`, at the first decision and each change (docs/VR_MOVING_OBJECTS_PLAN.md, "Separate small fix").

**Auto Mode running slow (2026-11-14, untested in a headset).** In Auto Mode, frames carrying motion vectors pace the game
at half the refresh rate (45 at 90 Hz). When the game cannot hold that (more than 2.5 refreshes a frame, under 36 fps at
90 Hz, for 1 s), one synthesized frame cannot fill the gap: frame synthesis is held as at Steady 30 and refresh filling
fills the refreshes (`update_auto_synth_hold` in `vr_vulkan.cpp`, after each game frame). It is used again after 5 s at 2.2
refreshes a frame or less (41 fps at 90 Hz); held again within 10 s of that, the next wait doubles (up to 60 s), since the
motion pass (paused while held) costs time again once used. The VR menu and the second after it are not judged. Only
where the session has frame synthesis swapchains (a Quest, the Steam Frame with it on): PC VR is unchanged. The log:
`[vr] Auto Mode frame synthesis hold: on (the game at N fps ...)` / `off (...)`, with `[vr frame synthesis] held: Auto
Mode, ...` and `[vr] refresh filling: on (frame synthesis held ...)`.

## The data (from the specs and Meta's XrSpaceWarp sample)
- One struct per projection **view** (eye): `XrFrameSynthesisInfoEXT` or `XrCompositionLayerSpaceWarpInfoFB`, with a
  motion vector subimage, a depth subimage, `minDepth`/`maxDepth` (0, 1), `nearZ`/`farZ` (positive metres; infinite
  allowed; equal is invalid) and `appSpaceDeltaPose`. Depth alone: `XrCompositionLayerDepthInfoKHR` per view.
- Motion vectors: RGBA16F, **current minus previous** NDC (Vulkan NDC for the EXT, decoded as `sample * scale +
  offset`), including head motion and locomotion (Meta's sample uses the full view-projection of both frames); .z the
  depth change (0 here: not known), .w unused.
- Depth swapchain: `VK_FORMAT_D24_UNORM_S8_UINT` (Meta's example; we take it, else D32F, X8_D24, D16), at the runtime's
  recommended size (about a quarter of the eye's per axis: `XrFrameSynthesisConfigViewEXT` /
  `XrSystemSpaceWarpPropertiesFB`). Higher gives no quality, per Meta.
- Pacing: with the struct attached the runtime paces `xrWaitFrame` at half the refresh rate itself (Meta's sample has
  no throttling of its own); a frame without it is not synthesized.

## What is built

### Choice (`vr/vr_framesynth.h` `choose`, at VR start in `openxr_session.cpp`)
| `vr.frameSynthesis` | Standalone (Quest, Frame) | PC |
|---|---|---|
| `auto` | `XR_EXT_frame_synthesis`, else `XR_FB_space_warp`, else depth | depth (`XR_KHR_composition_layer_depth`) |
| `on` | the same | frame synthesis where offered, else depth |
| `off` (**the default on PC VR and the Steam Frame**, the user's choices of 2026-11-03 and 2026-11-04: PC VR; the Steam Frame, Native and Lepton, until it is tested there: a player's run with it smeared and tore, and QuakeVR's Frame port ships it off after artifacts. A Quest test build of 2026-11-04 had `auto` by default; settings format 15 moved the `auto` it saved back to `off`. **On a Quest the default is `auto` since 2026-11-13** (the user's choice, after a Quest 3 player's run with it on felt good to them: about 21 fps drawn and shown about twice as smooth, the GPU 86-98% busy, head-to-picture delay 70-120 ms); settings format 28 moves an `off` saved with the old default to `auto`) | nothing new | nothing new |

By runtime extension enumeration only. A failure to make the swapchains turns the feature off; the eyes are untouched.
The log: `[vr] frame synthesis: ... chosen: ...` at start, `[vr frame synthesis] <what the frames carry>` on change.
F1 > VR "VR frame synthesis" (with a "Now:" line) and the VR launcher; applies at the next start.

### Motion vectors: the motion pass (docs/VR_DLSS.md, sections 5d and 10)
- Every depth-writing draw, and the background (sky, sea) where no geometry wrote depth, drawn again with its own
  vertex shader on the previous and current frame's data. So the motion holds the full 6DoF head pose (rotation and
  translation, at each pixel's real depth: translation parallax falls with distance, points at infinity rotate only),
  locomotion, Link, the boat, enemies and animation, per eye with each eye's own projection. No camera writer added,
  no smoothing, no extra latency in tracking (`motion_6dof_test`).
- It ran for DLSS only; now also when the session chose a frame synthesis backend (`vr::framesynth::motion_wanted`,
  set before the renderer's device; `ngx_vk::motion_wanted` and `motion_pass::on` follow it).
- **Its cost:** the scene's geometry is drawn a second time (CPU on the render thread, GPU for a depth-tested pass). The
  game then draws half as many frames, so the CPU per second should still fall; to be measured in the headset.
- Each eye's result (motion target, the scene depth it was tested against, whether the viewports flip y) is kept for
  the frame (`motion_pass::eye_result`).

### The export (`gfx/vulkan/vr_framesynth.cpp`, at the eye copy, `vr_vulkan.cpp` `record_eyes`)
Per eye, into the session's swapchain images (acquired, written, released before `xrEndFrame`):
- **Depth:** the scene depth (0 near, 1 far as drawn, not reversed) sampled into the runtime's depth image
  (`gl_FragDepth`). Which depth: the motion pass's; without it (depth only), the depth target most of the eye's camera
  draws drew into; a multiview eye 1 takes eye 0's layer twin.
- **Planes:** from the game's camera projection in the scene draws' vertex blocks (`stereo::find_camera_projection`,
  the same matrix the eye shift finds): `nearZ = p11 / (p10 - 1)` (clip z -1..1) or `p11 / p10` (GX2 HalfZ),
  `farZ = p11 / (p10 + 1)` (infinite when p10 = -1); game units to metres by the world scale.
- **Motion:** the motion target's `(prev - cur) * 0.5` in the game's clip space turned into `cur - prev` in Vulkan NDC:
  x times -2, y times +2 when the game's viewport flips y (it does: negative height), else -2
  (`vr::framesynth::motion_scale`). Non-finite values become 0. Nearest sampling (no blending across edges).
- `appSpaceDeltaPose` is identity: the motion vectors carry the locomotion (the game's camera holds it), and the
  background draws give the sky its motion.
- A frame shown again (the keep-alive submit) carries none; a frame without scene depth (menus, loading) carries none.

### Pacing
While the frames carry both eyes' motion vectors, the game draws at half the refresh rate (`vr.frameRate` treated as
half; a `WWHD_VR_FRAME_RATE` test rate still wins). Menus and loading screens keep `vr.frameRate`.

## Tests (automated; no headset)
- `vr_test` `test_frame_synthesis`: the backend per runtime (Quest, Frame, PC SteamVR, none; EXT before FB; no motion
  gives depth only; off gives nothing); the planes recovered from projections made with known planes (clip z -1..1 and
  0..1, finite and infinite far) and the depth they predict at a point; the motion conversion for a point under a
  turned and moved head (flipping and non-flipping viewports); the setting saved and read back.
- The three export shaders compiled with glslang and validated with `spirv-val` (Vulkan 1.1).
- Builds: Linux x86_64, Quest APK, Steam Frame native.

## Limits
- **Motion z** is 0 (the depth change between frames is not computed); the EXT's 2D flag would say so but "may decrease
  quality"; 3D with z = 0 is used.
- **Draws without motion:** batched (instanced) draws, draws with the depth test off, transparent effects (one motion
  per pixel): they move at the game's rate in synthesized frames, as Meta warns. The HUD is drawn into the eye
  picture (not a quad layer), so it is synthesized with the scene's motion beneath it.
- **The motion pass on Adreno** has never run: it was built and checked on NVIDIA for DLSS. If it fails, the session
  still submits depth only (`vr.frameSynthesis=off` turns everything off).
- **Below half rate** (the Quest has run 13 to 22 fps in busy scenes) synthesis cannot hold 90 Hz; Meta gives no
  guidance there.
- **Application-side interpolation** (our own frame generation) is not built: the runtimes do it with our data, and two
  would compete. FSR 3 frame generation is desktop D3D12/Vulkan with HLSL shaders: not for Adreno.
- **Depth only, while text or a menu shows:** not sent (docs/VR_CUTSCENE_TEXT.md, "No conversation text on Quest 3
  through VDXR"): a runtime compositing by it hid the game's UI layer behind the scene.
- **A Quest on a PC** (VDXR, Link, Steam Link): Auto sends no depth at all (user, 2026-11-03); `on` still sends it.
  The headset is known only after the extension is enabled, so it stays enabled and unused (`openxr_session.cpp`).
- **Latency:** synthesized frames are extrapolated; controller input reaches the picture at 45 Hz. Not equal to
  native 90 fps.

## Headset tests still needed
1. Quest 3: the log's `chosen: XR_EXT_frame_synthesis ...`, then `[vr frame synthesis] frame synthesis: motion vectors
   and depth`; logcat `ASW=..., Type=App`; Meta's overlays: `adb shell setprop debug.oculus.spaceWarpDebug 1`,
   `adb shell setprop debug.oculus.MVOverlay 4` (motion vector direction and the y sign).
2. Steam Frame: the same log lines; the compositor's "Using motion encoding AppMVs"; the performance overlay's motion
   and depth warnings gone.
3. Both: sailing, sword swings, fast head turns, walking past near objects (parallax), particles and water; frame
   times against `vr.frameSynthesis=off`.
4. PC SteamVR: `chosen: depth only`; reprojection under head translation with depth.

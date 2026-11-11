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
| `off` (**the default on every build**, the user's choices of 2026-11-03 and 2026-11-04: PC VR; the Steam Frame, Native and Lepton, until it is tested there: a player's run with it smeared and tore, and QuakeVR's Frame port ships it off after artifacts; Quest and other Android devices. A Quest test build of 2026-11-04 had `auto` by default; settings format 15 moves the `auto` it saved back to `off`) | nothing new | nothing new |

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

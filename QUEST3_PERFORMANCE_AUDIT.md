# Quest 3 standalone performance audit (2026-11-03)

The native Android/arm64 build on Quest 3 (not Link, Air Link, Steam Link or Virtual Desktop). Marks: **[code]** read in
this repository, **[log]** a Quest 3 log already recorded in `docs/` or the session notes (date named), **[build]**
built here, **[est]** inference. **No Quest 3 hardware run was made for this audit, and this PC cannot build the
Android target** (no NDK or Android platform 36). Shared architecture is described in
[STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md](STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md), section 1; this report covers what is
different or decisive on Quest.

## 1. What the Quest build actually runs

| Area | Quest 3 path | Where |
|---|---|---|
| Build | `android/build_native.sh VR=1` (NDK 30, `CMAKE_BUILD_TYPE=Release`, `WWHD_ANDROID_VR`), Gradle `-PwwhdVr assembleVrRelease`, Khronos OpenXR loader 1.1.63; game code `-O3 -ffp-contract=off`; no LTO (`WWHD_LTO` off), no `-mcpu` (only Apple gets one) | [code] `android/build_native.sh`, `CMakeLists.txt` |
| OpenXR | `XR_KHR_android_create_instance`, `xrInitializeLoaderKHR`, OpenXR 1.0 then 1.1; `XR_KHR_vulkan_enable2` (the runtime makes the instance and device); Touch Plus profile; `XR_FB_display_refresh_rate` (the rate chosen is asked for); Quest Super Resolution (`XR_FB_composition_layer_settings`, auto); frame synthesis `XR_EXT_frame_synthesis` (Auto) | [code] `openxr_session.cpp` |
| Vulkan | Qualcomm's Adreno driver (Adreno 740); BC textures natively where the driver reports them, else a compute (or CPU, old drivers) decoder to RGBA8 | [code] docs/android-gpu.md |
| Stereo | **multiview off by default on Android** (`stereo_switches.h` `multiview_default_on`); eye 1 replays eye 0's stream with eye-shifted uniforms; per-eye projection on (shared code default) | [code] |
| Threads | game; render; record worker (on on Quest); sun atlas stream and thread (on on Quest: `atlas_default_on`); end-frame thread (on); scene threads off on Android (`env_default_on`) | [code] |
| Resolution | fixed 0.75x of the internal resolution (960x540 at 1x; `dynres.cpp` `default_fixed`); dynamic resolution off by default and forced off with multiview on the Qualcomm driver | [code] |
| Upscale | Quest Super Resolution in the compositor (auto); SGSR1 by choice | [code] |
| Pacing | Auto: full rate while it fits, else half (36 fps at 72 Hz, 45 at 90) with exact 30 Hz logic interpolation; clock locked to the headset | [code] |
| Window | the app's window picture skipped while in VR (`skip_window_tv`) | [code] |
| Memory log | **new** (this audit): every 10 s, resident memory, its peak, the system's available memory | [code] |
| Thread scheduling | **new** (this audit): game, render and end-frame threads declared to Meta's runtime | [code] |

## 2. Measured history on Quest 3 [log]

| Date / build | Result | Diagnosis recorded |
|---|---|---|
| runtime 209, early build | ~24 frames/s, 23.7 logic steps/s; frames 33-65 ms; work after xrWaitFrame ~1.8 ms | multiview/atlas off, record worker off, window composed every frame (all fixed since) |
| runtime 209, 0c8b3d2 | black squares; 27 fps | multiview + atlas paths on: artifacts |
| v0.1+574, runtime 207 | 30 fps at most (median 24.6 at 72 Hz, 30 at 90); GPU 50-60 % busy | **CPU-bound**; multiview supported but off |
| v0.1+577, driver 512.837.9 | left-eye artifacts; of ~3,400 eye-0 draws, 215 multiview, 3,192 "not now (state)" | fallback causes split in the log; lead: `maxDescriptorSetUniformBuffersDynamic` |
| 2026-10-31 crash logs | Adreno crash in `vkCmdEndRendering` after two-eye targets re-paired at a rescale | rescales avoided on Quest; end-of-rendering guard |
| user logs 2026-10-31 | 22-29 fps at 90 Hz; passes of 28-44 ms | **the game thread's CPU**, not the GPU |
| Downloads\wwhd.log 2026-11-02 | ~30 fps at 72/90/108/144 Hz; logic 30 steps/s fine; "released + xrEndFrame" p50 13.4-16.9 ms; render thread's own ops ~1 ms; atlas-thread lock waits 1.5-3 ms/frame | frame end serial with the next frame's CPU work (end-frame thread since, on by default; unmeasured on Quest) |

No Quest log since the end-frame thread, the per-eye projection default and the UI quad layer.

## 3. Bottlenecks, ranked

| # | Bottleneck | Evidence | CPU/GPU | Impact | Effort | Risk |
|---|---|---|---|---|---|---|
| 1 | **Both eyes recorded on the CPU** (multiview off on Android) | [log] 24-30 fps CPU-bound, GPU 50-60 %; [code] | CPU (render thread) | high | high | high (the black squares) |
| 2 | **Multiview's fallback**: pipelines whose uniform blocks exceed `maxDescriptorSetUniformBuffersDynamic` are neither multiview (`draw.cpp` `kMvNotDynamic`) nor replayable for eye 1 (`bind_stage`: `replayable` needs `dynamicUniforms`) | [code] draw.cpp 1154, 2888, 3963; [log] 3,192 of 3,400 "not now" | CPU | high (the cause of #1's fallback, and of mixed passes that likely drew the squares) | high | medium |
| 3 | Game-thread pass time (28-44 ms in busy scenes) | [log] | CPU (game thread) | high | — | — |
| 4 | Frame end serial with CPU work | [log] 13-17 ms; end-frame thread now default | CPU | high, if not already fixed | done, unmeasured | low |
| 5 | Lock contention (buffers, shaders) | [log] atlas 1.5-3 ms/frame; Quest waits 5-7x PC's | CPU | medium | medium | medium |
| 6 | BC textures decoded to RGBA8 if the driver lacks BC (4-8x memory and bandwidth for those textures) | [code]; whether Quest 3's driver reports BC is **not in any recorded log** | GPU, memory | medium-high if so | — | — |
| 7 | Thread placement: XR-critical threads not declared to the runtime | [code]; JKXR declares them | CPU | unknown | **done** | low |
| 8 | Full-screen post per eye (bloom, AO, FXAA) at eye size | [code] | GPU | low (GPU 50-60 %) | medium | low |

Not found: `vkQueueWaitIdle` in the VR frame path (async presentation is the default; the window present is skipped
in VR) [code].

### 3.1 Why #2 is the lever

Without dynamic uniform buffers, a pipeline's per-draw uniform blocks are bound by descriptor offset, so eye 1 cannot
re-use eye 0's descriptor writes with only its eye-shifted rows changed, and a multiview twin cannot index per-view
blocks. Adreno reports as few as 8 dynamic uniform buffers per set (about 15 on NVIDIA) [log: limits line]; GX2 shaders
bind up to 16 blocks per stage plus a support block [code]. Options (P2, roadmap): pack a draw's blocks into one dynamic
buffer with offsets in a push constant; or plain uniform buffers with per-eye descriptor sets for the over-limit
pipelines. Either should let nearly every draw take multiview, which also removes the mixed single-layer/two-layer
writes the black squares were traced to.

## 4. Memory

- Quest 3 gives an app about 5.75 GiB (docs/VR_ANDROID.md). Unknown: the game's resident size in a busy stage, the
  texture-pack residency (packs keep prefetched loads up to 5 GiB on PC: `f02c70e6`; the Android default needs
  checking against 5.75 GiB), the RGBA8 cost of decoded BC textures.
- **New monitoring** (this audit, `vr_vulkan.cpp` `log_stats`, Android and Linux): every 10 s `[vr] memory: the game N
  MiB resident (peak M MiB); the system A of T MiB available`. Not run yet.
- Dynamic resolution's kept level images are not used on Quest (dynres off), so no spare images there [code].

## 5. Frame pacing on Quest

- Refresh rates: Quest 3 runs 72, 80, 90 and 120 Hz; the game asks for `vr.refreshRate` through
  `XR_FB_display_refresh_rate` and follows the rate the runtime reports [code]. 80 Hz full rate draws at 60 (no 80 fps
  schedule; parked) [code].
- Below half rate (the logs' 24-30 fps at 72/90 Hz) every frame is reprojected by the runtime; head rotation stays
  smooth (ATW), animation and locomotion run at the game's rate. At 72 Hz, Auto's 36 fps half rate is the realistic
  first target.
- Frame synthesis (Auto on Quest): untested there; with the game below 36 fps it cannot hold 72 [est]. The Steam
  Frame's smeared run and QuakeVR's artifacts argue for a Quest A/B before relying on it.

## 6. Changes made in this audit

| Change | Files | Validation |
|---|---|---|
| `XR_KHR_android_thread_settings`: enabled when offered (Android); the game thread (application main, at its first input sync), render thread (renderer main, at frame begin) and end-frame thread (renderer worker) declared once per session; logged | `runtime/src/vr/openxr_session.h/.cpp`, `runtime/src/gfx/vulkan/vr_vulkan.cpp` | Windows build passes (the calls are no-ops there); the Android-only code compiled on its own against the pinned OpenXR headers (syntax check); **not built for Android, not run on a Quest** |
| Memory log every 10 s (Android, Linux) | `vr_vulkan.cpp` | the parsing code compiled on its own; not built for Android or Linux here; not run |

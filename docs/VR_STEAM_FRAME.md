# VR on Steam Frame: two targets

Steam Frame runs the game two ways, as two build targets with the same game, renderer and VR code:

- **Steam Frame - Lepton** (Android VR, arm64): the Android VR app in Lepton, Valve's Android container.
- **Steam Frame - Native** (Linux arm64, SteamOS arm64): the Linux build, run directly on SteamOS.

A log says which build it is and what it runs on (`[vr] target: ...`, `runtime/src/vr/vr_platform.h`).

**Status (2026-10-30):** neither target has been built or run since this split. The Lepton APK of an earlier build
was tried once: its launcher opened and was ended by AndroidX's profile installer (fixed, below). Everything under
"Verified" comes from that log and from Valve's and FramePort's published notes, not from tests of these builds.

## Verified requirements

**Lepton** (from the 2026-10-30 log, Valve's Steamworks pages, FramePort's runtime notes):
- Lepton v3.0.5 is a Waydroid fork running LineageOS 11 (`lepton_arm64_only:11/RQ3A`): Android 11, API 30,
  arm64 only. Valve asks custom engines to target Android 10 or Linux arm64. It reports a newer Android version to
  apps (the profile installer took its Android 13 path) yet lacks Android 12/13 methods.
- OpenXR: SteamVR's Android runtime (`/data/steamvr/runtime`), found by the Khronos Android loader the APK bundles;
  OpenXR 1.0. Offers `XR_KHR_android_create_instance`, `XR_KHR_vulkan_enable2`; drops
  `XR_KHR_composition_layer_cylinder` (the game's curved menus are optional: flat quads instead). Controllers bind
  as Touch by default; `XR_VALVE_frame_controller_interaction` gives the Frame controllers' own profile.
- Vulkan: Mesa Turnip on the Snapdragon 8 Gen 3 (Adreno 750). Eye swapchain formats `R8G8B8A8_SRGB` (43) and
  `B8G8R8A8_SRGB` (50) only; the game asks for 43. Display 72 to 144 Hz.
- Lifecycle: Lepton starts the APK's `MAIN` + `LAUNCHER` activity in a container, watches the app's process and
  ends the container when it exits; it clears the app's data after an early exit. Steam's Exit signals the process
  tree. No on-screen keyboard.

**SteamOS arm64** (Valve's Steamworks pages, FramePort's notes):
- SteamOS is Arch-based on the Snapdragon 8 Gen 3; its glibc is newer than the build's 2.35 baseline.
- OpenXR: SteamVR is the host runtime for Linux processes (`steamxr_linuxarm64.json`). **SteamVR on Linux takes
  OpenXR 1.0 only** (1.1 returns `XR_ERROR_API_VERSION_UNSUPPORTED`): the game asks for 1.0.
- Steam Linux Runtime: SLR 3.0 "sniper" arm64 and `steamrt4-arm64` exist. **Sniper's glibc is 2.31, older than the
  build's 2.35**: run the Native build directly (a non-Steam shortcut without a compatibility tool) or under
  steamrt4, not under sniper. The SLR container drops `XR_API_LAYER_PATH`.
- Deployment: the SteamOS Devkit Client deploys Linux binaries; a non-Steam shortcut works too.

## Architecture

```text
game code, GX2 -> Vulkan renderer, VR (runtime/src/vr: OpenXR session, tracking, game hooks)   shared
        |
platform layer (existing): SDL3 host, Vulkan device, OpenXR loader lookup                       shared, #ifdef'd
        |
        +-- Steam Frame - Lepton: Android activities (src/vr: VrLauncherActivity, WwhdActivity), the Khronos
        |   Android loader, XR_KHR_android_create_instance, API 30, Android 12/13 functions looked up
        |   (platform/android_late_api.h)
        +-- Steam Frame - Native: Linux SDL3 host, the OpenXR loader next to the game, glibc 2.35
```

No game, renderer or VR code is duplicated; the targets differ in build configuration, packaging and the few
platform files above. `vr_platform.h` only names the target for the log; no behaviour depends on it.

## Dependency matrix

| Component | Lepton (Android arm64) | Native (Linux arm64) |
|---|---|---|
| C++ runtime | NDK libc++ (`c++_shared`, in the APK) | zig's libc++ (static), glibc 2.35 |
| OpenXR loader | Khronos `openxr_loader_for_android` 1.1.63 (Gradle) | built from OpenXR-SDK release-1.1.63, next to the game |
| OpenXR runtime | SteamVR (Android), 1.0 | SteamVR (Linux), 1.0 |
| Graphics | Vulkan 1.3 (Turnip), `XR_KHR_vulkan_enable2`, multiview | the same |
| Windowing / input | SDL3 (Android), OpenXR actions | SDL3 (X11/Wayland), OpenXR actions |
| Audio | SDL3 (AAudio/OpenSL) | SDL3 (PipeWire/Pulse/ALSA) |
| Filesystem | app storage, All files access (`MANAGE_EXTERNAL_STORAGE`) | next to the game / XDG folders (installer) |
| Threading | pthreads | pthreads |
| Third-party | SDL3, glslang, zlib, LZ4, zstd (pinned, built per ABI) | the same, plus the loader |
| Packaging | APK, flavor `steamframeLepton` | release zip / AppImage `linux-aarch64-steamframe-native` |

The one piece of inline assembly (`gx2/thread_identity.h`, the thread pointer) has its arm64 Linux/Android path;
no x86 intrinsics are used in the runtime.

## Builds

**Steam Frame - Lepton** (NDK 30.0.16248370, Android SDK, JDK 17 or 21 for Gradle):

```bash
TARGET=steamframe-lepton android/build_native.sh   # API 30, build folder game-steamframe-lepton, app/libs-steamframe-lepton
cd android && ./gradlew -PwwhdVr assembleSteamframeLeptonRelease
```

- CMake `WWHD_STEAM_FRAME_LEPTON` (arm64-v8a Android with `WWHD_ANDROID_VR`, API 30 or lower; not together with
  `WWHD_STEAM_FRAME_NATIVE`); preset `steam-frame-lepton` (env `ANDROID_NDK`).
- Gradle flavor `steamframeLepton` (`android/app/build.gradle`, only with `-PwwhdVr`): the `vr` flavor's sources and
  manifest, `minSdk 30` (the `vr` flavor, for Quest and Android XR, stays at 33), its native libraries from
  `app/libs-steamframe-lepton`.
- Android 12/13 functions are looked up at run time (`platform/android_late_api.h`): `backtrace` (crash reports),
  `APerformanceHint_*`, `AThermal_getThermalHeadroom`. Linked directly, they kept `libmain.so` from loading on
  Android 11; on Lepton the crash report then has no host frames and the performance hint and heat guard are off.
- Install: Lepton Development, `adb connect`, `adb install` (Valve's "adb_lepton" page).

**Steam Frame - Native** (an arm64 Linux machine, or the release's arm64 runner):

```bash
cmake --preset steam-frame-native && cmake --build --preset steam-frame-native
```

- CMake `WWHD_STEAM_FRAME_NATIVE` (Linux arm64 with `WWHD_VR` only): builds the OpenXR loader from the pinned SDK
  and copies `libopenxr_loader.so.1` next to the game, which opens it before the system's.
- Release: `.github/workflows/release.yml`, Linux job `target: steamframe-native` (the `linux-aarch64` runner and
  zig toolchain, `aarch64-linux-gnu.2.35`), packaged by `tools/release/package.py --target steamframe-native`:
  `WindWakerHD-<version>-linux-aarch64-steamframe-native` (zip, AppImage). The installer treats it as
  `linux-aarch64` and installs `libopenxr_loader.so.1` and `wwhd-vr-launcher` next to the game with SDL3; the job
  checks that both are installed and the launcher's libraries are found.

**The other targets** are unchanged: presets `windows-vr` (docs/VR_BUILD_WINDOWS.md) and `linux-vr`
(docs/VR_BUILD_LINUX.md); Quest and Android XR stay `android/build_native.sh VR=1` + `assembleVrRelease`.

## Defaults and launch advice (2026-11-03)

Defaults of the Steam Frame builds (Native and Lepton; a saved `settings.ini` value still wins), from
STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md:
- **Frame synthesis off** (`vr.frameSynthesis=off`) until it is tested on the Frame: a player's run with it on smeared
  and tore; QuakeVR's Frame port ships it off after artifacts. `on` or `auto` in F1 > VR or the launcher still try it.
- **Dynamic resolution profiles top out at the fixed 0.75x** (docs/VR_DYNAMIC_RESOLUTION.md); Custom can go higher.
- **SGSR1 sharpness 1.5** (2 may halo the game's dark outlines).

Launch advice:
- **Leave Steam's per-game "Foveated Rendering" switch off** (Properties > Performance). It enables Valve's
  `VK_LAYER_VALVE_fdm_injection`, which foveates only images the XR runtime creates (the eye swapchains). The game
  draws its scene into its own images and only upscales into the swapchain, so it would lower the sharpness of the
  view's edges and save almost nothing.
- **SteamOS adds `VK_LAYER_VALVE_rpo` and `VK_LAYER_VALVE_fdm_injection` to every game** through `VK_INSTANCE_LAYERS`
  (`steam-launch-wrapper`). The RPO layer crashed another game on the Frame with GPU faults
  ([SteamOS#2973](https://github.com/ValveSoftware/SteamOS/issues/2973)). If the game crashes or the kernel log shows
  Adreno faults, try the launch option `VK_LOADER_LAYERS_DISABLE=*rpo* %command%`.
- The log says which of them were set: `[vr] Vulkan layers from the environment: ...` (only when one is).

## CPU speed-ups from PC VR and the Quest (2026-11-04)

Steam Frame - Native builds as Linux on ARM, so it already had the PC defaults (multiview, the atlas thread, both
scene drawing threads, the record worker, the DrawDone skip) and the mobile GPU paths (`__aarch64__`: the multiview
no-mix guard, the wait before one eye's clear). Two were missing and are on now:
- **Uniform snapshot reuse** (`WWHD_VK_REUSE_UNIFORM_SNAPSHOTS`, draw.cpp): on for all desktop Linux, ARM included,
  as on Windows and x86-64 Linux. `=0` turns it off.
- **The busy threads on the fastest cores** (`platform/fast_cores.h`, shared with Android): the render thread, the
  game thread and the record worker (`perf_hint::add_current_thread`, which on Linux ARM now pins as on Android) and
  the scene's two drawing threads (`core_groups` Role::Scene) go on the cores with the highest maximum clock in
  sysfs; the other threads stay where the scheduler puts them. The log has `[perf] thread N on the K fastest cores`.
  `WWHD_FAST_CORES=0` or `WWHD_VK_PLACE_THREADS=0` leaves them unplaced. (Android's performance-hint session and heat
  guard are Android APIs and stay Android-only.)
Built for Windows only here (no ARM Linux toolchain on this PC); not run on a Frame.

## Diagnostics

At VR start: `[vr] target: <build target>, <arch>; running on <Steam Frame device>`, `[vr] OpenXR loader: ...`
(next to the game, the system's or the APK's), `[vr] OpenXR runtime: ...`, `[vr] OpenXR extensions offered / enabled`,
`[vr] headset: ...`, the swapchains, tracking space, refresh rates and controller profiles as before. The Vulkan
device and driver are logged by the renderer (`[vulkan] device: ...`).

## Not checked

Nothing here is built or run yet. To check, in order:

- Lepton: `build_native.sh TARGET=steamframe-lepton` and the Gradle flavor build (lint at `minSdk 30`: the shared Java
  has Android 12+ calls behind version checks, which Lepton's reported version may pass: SDL's haptics use
  `VibratorManager`); the launcher past 6 s; Play reaching the OpenXR session; whether Lepton keeps the container when
  the launcher's activity finishes (the launcher no longer exits the process unless it is `:launcher`; a launcher in
  the main process then keeps SDL's state, so opening it again in that process is untested); controllers, audio,
  storage (Lepton clears app data after an early exit).
- Native: the release job on the arm64 runner (the loader built with zig), the game and launcher on SteamOS arm64,
  SteamVR found by the shipped loader, the Frame controllers, Vulkan on Turnip, audio, performance.
- Performance on either target: no measurements yet.

## Sources

- Valve, Steamworks: [Steam Frame](https://partner.steamgames.com/doc/steamhardware/steamframe),
  [Custom engines](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom)
- [FramePort: Steam Frame runtime reference](https://frameport.app/docs/frame-runtime/)
- [gHacks: Valve open-sources Lepton](https://www.ghacks.net/2026/09/20/valve-open-sources-lepton-its-android-compatibility-layer-for-the-steam-frame/)
- the Lepton log of 2026-10-30 (docs/VR_ANDROID.md, "Steam Frame - Lepton")

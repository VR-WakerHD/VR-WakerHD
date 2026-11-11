# VR on Android (standalone headsets)

The Android app can be built with VR through OpenXR, for standalone headsets: Meta Quest 3 (and Quest 2, Pro, 3S),
and, untested, Android apps on other headsets with a Khronos-conformant OpenXR runtime. It is the same VR mode as on
Windows and Linux (the same `runtime/src/vr` code, settings and controls), in the same Android app. It is off unless
asked for: the phone build, and the Windows and Linux builds, are unchanged.

**Status:** it builds. It has not been run on a headset yet (no headset was available); see "Still to check".
Performance is not addressed yet: the game's stereo rendering costs 12 to 16 ms of render-thread CPU per frame on a
desktop Ryzen 9 (docs/VR_PERFORMANCE.md), more than a headset's frame budget, so expect a low frame rate at first.

## Building

As the Android port's PC build (docs/android.md, "Build it on a PC"), with two switches: `VR=1` for the native
libraries and `-PwwhdVr` for the app.

```bash
python tools/recomp/recomp.py game/code/cking.rpx build/gen   # your own game: the APK contains it; keep it to yourself
VR=1 android/build_native.sh                                  # android/build/game-vr, then android/app/libs/arm64-v8a
                                                              # (with the VR launcher and wwhd-extract; a later phone
                                                              # build, without VR=1, removes those two again)
cd android && JAVA_HOME=<JDK 17> ./gradlew -PwwhdVr assembleVrRelease
adb install -r app/build/outputs/apk/vr/release/app-vr-release.apk
```

Needs: the Android SDK (platform 36, build tools 35.0.0), NDK 30.0.16248370, CMake and Ninja, JDK 17 (Gradle 8.12
does not run on newer JDKs). The headset in developer mode (Meta: a developer account, then developer mode in the
Meta Horizon app; `adb devices` lists it). Then copy your game files as for the PC build:
`/sdcard/Android/data/org.wwhdrecomp.wwhd/files/game` (or use the app's setup screens).

The VR APK has the same package name as the phone app (one replaces the other on a device).

## What is different from the phone build

All of it is Android-only, and only in the VR build:

| Where | What |
|---|---|
| `CMakeLists.txt` | `WWHD_ANDROID_VR` (off by default): the VR sources and `WWHD_VR` in the Android build. The desktop loader build and the VR launcher stay desktop-only |
| `android/build_native.sh` | `VR=1`: `-DWWHD_ANDROID_VR=ON`, its own build folder (`game-vr`) |
| `android/app/build.gradle` | `-PwwhdVr`: a `vr` flavor, target SDK 34 (Meta's store rule), the Khronos OpenXR loader for Android (`org.khronos.openxr:openxr_loader_for_android:1.1.63`, the SDK release of the game's OpenXR headers) |
| `android/app/src/vr/AndroidManifest.xml` | head tracking required; the OpenXR permissions and the loader's runtime-broker queries; `com.oculus.supportedDevices`, focus-aware; the game activity (`WwhdActivity`) with `com.oculus.intent.category.VR` and `org.khronos.openxr.intent.category.IMMERSIVE_HMD`, landscape, not resizeable. The setup screens stay a 2D panel. Vulkan 1.3 is checked at run time, not required to install |
| `android/app/src/{main,vr}/res/values/vr.xml`, `WwhdActivity.getArguments` | the VR variant starts the game with `--vr` (only `WwhdActivity` itself: not shader preparation or the display smoke test) |
| `runtime/src/vr/openxr_session.h/.cpp` | `XR_USE_PLATFORM_ANDROID`; before any other call, `xrInitializeLoaderKHR` with `XrLoaderInitInfoAndroidKHR` (the app's Java VM and Activity, from SDL); `XR_KHR_android_create_instance` required and `XrInstanceCreateInfoAndroidKHR` chained to the instance; the loader is the APK's `libopenxr_loader.so` (the existing `dlopen` fallback name). Quest 3's Touch Plus controllers get their own profile (`/interaction_profiles/meta/touch_controller_plus`, with `XR_META_touch_controller_plus` when offered), the same bindings as Touch. Steam Frame's controllers get theirs (`/interaction_profiles/valve/frame_controller_valve`, with `XR_VALVE_frame_controller_interaction` when offered; the name as Valve documents it, not in the pinned headers): A and B on the right controller's A and B, the left hand's X and Y on the left D-pad's up and left (where they sit on the GamePad; the left controller has no X or Y), the menu on the left view and the right menu button; trigger, grip, stick, stick click, poses and haptics as on Touch. Without the extension the runtime binds them as Touch controllers |
| `runtime/src/gfx/vulkan/backend.cpp`, `vr_vulkan.cpp/.h` | in a headset the app's window surface is not shown (or not there): while a VR session runs the TV present returns at once (`vrvk::skip_window_tv`), before it composes a picture for the window or records the eye copies into the headset's images. Then `swap()` records them itself (`vrvk::eyes_pending`) and submits them before the frame ends (OpenXR needs the work on the queue before the images are released) |

**Headsets.** One APK for three kinds:

| | Meta Quest (2, Pro, 3, 3S) | Android XR (Galaxy XR...) | Steam Frame - Lepton (Android apps; Steam Frame - Native is the Linux arm64 build, docs/VR_STEAM_FRAME.md) |
|---|---|---|---|
| Runtime found by | the Khronos loader (runtime broker) | the Khronos loader; `<uses-native-library android:name="libopenxr.google.so">`, without which Android XR does not load its runtime | the Khronos loader (SteamVR's Android runtime, reported OpenXR 1.0: the game asks for 1.0) |
| Immersive game activity | `com.oculus.intent.category.VR`, `IMMERSIVE_HMD` | `PROPERTY_XR_ACTIVITY_START_MODE = XR_ACTIVITY_START_MODE_FULL_SPACE_UNMANAGED` on the game activity (the launcher stays a panel; if Android XR only honours it on `<application>`, it moves there) | `IMMERSIVE_HMD`; the launcher is the `MAIN` + `LAUNCHER` activity Lepton starts |
| Features | head tracking, `com.oculus.supportedDevices` | `android.software.xr.api.openxr` (not required, so the others install it too) | — |
| Controllers | Touch, Touch Plus (`XR_META_touch_controller_plus`) | hands: `XR_EXT_hand_interaction` (aim, grip, pinch as the trigger, grasp as the grip; no buttons or sticks, so moving needs a controller); controllers through the profiles they offer | its own (`XR_VALVE_frame_controller_interaction`), else Touch |

`android.hardware.vr.headtracking` is required (Meta's store asks for it); a store that filters on it may hide the
app on headsets that do not list it, sideloading is not affected. Play VR passes `--vr` to the game activity; the
game asks for VR and, without a headset runtime, falls back to the flat screen.

**No conflicts between them.** Each headset's part is ignored by the others or used only when offered:
- manifest: the Quest categories and `com.oculus.*` entries, Android XR's Full Space property and its runtime library
  (`required="false"`), and `IMMERSIVE_HMD` sit side by side; a system acts on its own and ignores the others'.
  Android XR's OpenXR feature is not required, so the APK installs on all three;
- OpenXR version: 1.0 first (Steam Frame's Android runtime takes only 1.0); only if a runtime refuses 1.0
  (`XR_ERROR_API_VERSION_UNSUPPORTED`) is it asked again with 1.1 (Android implementations of 1.1). The game uses
  nothing of 1.1;
- extensions: each optional one is enabled only when the runtime lists it (Touch Plus, Steam Frame's controllers,
  hand interaction, refresh rate, cylinder layers), and its controller profile is suggested only then. Several
  profiles bound at once is how OpenXR works: the runtime uses the one of the device in hand (controllers or hands);
- the refresh rate: asked for once; a runtime that refuses (reported on Steam Frame) only logs it;
- one store-level difference only: `android.hardware.vr.headtracking` is required (Meta's store asks for it), target
  SDK 34 (Meta's rule) and All files access (reviewed on stores). Sideloading on all three is not affected; a
  store release on Google Play would need its own variant of these.

Everything else is the desktop VR mode as it is: the same OpenXR session, Vulkan through `XR_KHR_vulkan_enable2`
(the runtime creates the instance and device), the same layers, controls, settings and VR menu. The phone's touch
controls are hidden while a VR session runs: the buttons, the sticks, their toggle and the controller hint, and
their virtual gamepad is detached (`runtime/src/platform/touch_pad.cpp` `nativeTouchState` reports the session,
`TouchControls.java`). If VR falls back to the flat screen (no headset), they come back. The phone's rumble and heat
guard are not turned off in VR.

## The VR launcher (the app's first screen)

In the VR build the app opens on the VR launcher, the same program as the desktop's `wwhd-vr-launcher`
(docs/VR_LAUNCHER.md), and the game starts from it.

- **How:** `VrLauncherActivity` (`android/app/src/vr/java`, only in the VR variant) is the app's start activity
  (`MAIN` + `LAUNCHER`). It runs the launcher, built for Android as `libvrlauncher.so` (`CMakeLists.txt`, with
  `WWHD_ANDROID_VR`), as SDL's main, in its own process (`:launcher`): Android allows one SDL program per process,
  and the game has its own, as shader preparation does. The setup screens lose their start entry in this variant;
  "Set up your game" in the launcher's General tab opens them.
- **Play:** Play VR, Play Flat (a flat panel) and VR Weapons Test start the game activity with the same
  arguments the desktop launcher gives `wwhd` (`--vr` / `--no-vr`, `--no-stereo` with the desktop, so it is the plain
  picture and not a saved 3D output; `--vr-test`, `--game` when a game folder is set), through `VrLauncherActivity.launchGame`, and the launcher closes (its process ends).
- **Settings:** the same `settings.ini` as the game: the launcher sets `XDG_CONFIG_HOME` to the app's external files
  folder as the game does (`runtime/src/main.cpp`), so `host::config_dir()` is the game's.
- **Not on Android:** the OpenXR runtime section (the headset's runtime is the only one), the game program and
  game-folder fields (the General tab shows the game folder and whether the game is in it), "Open the settings
  folder".
- **Install game:** extracting a Cemu archive (`.wua`) or a disc image (`.wud`, `.wux` with its keys) works as on the
  desktop, with these differences:
  - `wwhd-extract` is in the APK as `libwwhd-extract-driver.so` (`android/build_native.sh VR=1`), in the app's
    native-library folder: the one place Android lets an app run a program. SDL has no processes on Android (its
    process API is a stub there), so the launcher's runner starts it with `posix_spawn`
    (`tools/vr_launcher/extract_runner.h`, Android only);
  - Android's file picker gives a `content://` link, not a path: the chosen files are copied into the app's folder
    (`import/`) first, and the copies are used. Several files can be chosen at once: each goes to what its name says
    (`*.key` the disc's key, `common.key` or a key file named "common" the console's common key, anything else the
    game source). Keys are copied next to the image under their own names, so `wwhd-extract` finds them there as on
    the desktop (the image's `<name>.key`, `common.key`), and they are kept for the next time (an earlier common key
    in `import/` is used again). The image's copy is removed once the game is extracted. Keep room for it;
  - the "Extraction progress" bar shows every stage: the copy (its size from the file's provider, as SDL may not know
    it), reading the game's information, the archive's checksum, the extraction. Cancel stops the copy too. The
    copy, the extractor's result and its errors also go to the device log (`adb logcat -s SDL`, `[launcher]`);
  - decryption: a `.wua` is already decrypted; a `.wud` or `.wux` is decrypted by `wwhd-extract` as it extracts,
    with the keys above. The game reads the extracted, decrypted files;
  - the extract location: the game's own folder in the app's storage by default (`<app files>/game`, where the game
    looks without any setting). "Choose..." opens the launcher's own folder browser (Android has no folder picker for
    paths): the app's storage, or the device's shared storage (`/storage/emulated/0`, your Download folder for
    example) once the app has All files access (`MANAGE_EXTERNAL_STORAGE`, VR variant only; "Allow device storage"
    opens the system's screen for it). New folders can be made there. A chosen folder is set as the game's folder
    (`launcher.gameDir`, passed to the game with `--game`); there is no "Open the game folder";
  - the Android runner (`posix_spawn`) is tested on Linux too: `launcher_install_posix` runs the launcher's install
    test through it with the real extractor;
  - whether the game program was built from the same version is checked by the game itself when it starts.

On Meta's store, an immersive app's start activity is expected to carry the VR category; the launcher is a 2D panel,
which is fine for sideloading. Steam Frame's Android layer reportedly launches only `MAIN` + `LAUNCHER` activities,
which the launcher is.

## Standalone frame rate

A Quest 3 log (Oculus runtime 209, build 6d71691) ran at about 24 game frames/s and 23.7 logic steps/s (30
expected), each frame 33 to 65 ms, while the work after `xrWaitFrame` took about 1.8 ms: the frames were slow to
make, not to submit. Three causes in that build:

- multiview, the sun atlas's stream and its thread were off: `stereo_switches.h` `env_default_on` kept the desktop's
  defaults off on Android, so every draw was recorded twice on the headset's CPU. They were turned on by default on
  a Quest (`quest_theater::device`, 458e5876) and are off again (see below); `=1` in env.txt turns each on;
- the record worker was off; it is on by default on a Quest now (`backend.cpp`, the record worker's saved choice);
- the app's window, never seen in the headset, got a composed 4128x2208 picture every frame, more pixels than both
  eyes. It is skipped while a VR session runs (`vrvk::skip_window_tv`).

The same log ended in a crash: the session stopping changed the window's present mode as its surface went, and
`make_swapchain`'s "surface capabilities" failed outside the code that waits for a new surface; on Android that
change now goes through it. The crash report then faulted in `crash_context::note`, whose 32 KB copy overran the
thread's signal stack (Android's own, small, on every thread but the first); the copy is static now.

The first standalone run with them on by default (Quest 3, Oculus runtime 209, build 0c8b3d2) showed black squares
all over the picture, at about the same frame rate as before (27 frames/s, game logic far below 30 steps/s). Never
tried in a headset before they were turned on, they are off by default on every Android device again
(`stereo_switches.h` `android_default_on`), the configuration of the earlier Quest runs. Not yet known which of the
three (multiview, the atlas stream, the atlas thread) drew the squares: env.txt with `WWHD_VK_MULTIVIEW=1`,
`WWHD_VK_ATLAS_STREAM=1` or `WWHD_VK_ATLAS_THREAD=1` turns one on to find out. The record worker, on by default on a
Quest since upstream's Quest theater (b0738082), stays on; `WWHD_VK_RECORD_WORKER=0` turns it off if squares remain.

**Multiview on again on a Quest (2026-10-31), the atlas paths still off.** A later Quest 3 log (build v0.1+574, Oculus
runtime 207) logged `multiview: supported (max 6 views)` but not enabled, and ran at 30 frames/s at most (median 24.6 at
72 Hz, 30 at 90 Hz) with the GPU 50 to 60 % busy: the CPU was the limit, and without multiview every draw is recorded
twice on it. Multiview (`WWHD_VK_MULTIVIEW`, `WWHD_VK_MULTIVIEW_DRAWS`) is on by default on a Quest again on its own
(`stereo_switches.h` `multiview_default_on`, `quest_theater::device`); the atlas stream and thread stay off on every
Android device. Should the squares come back with it, `WWHD_VK_MULTIVIEW=0` in env.txt turns it off. Not yet tried in
the headset.

**A margin beyond the field of view (2026-10-31).** The same user saw "stretched pixels on the edges of the screen,
like the bottom and the right". At 30 frames/s on a 72 or 90 Hz headset every frame is shown again, turned to the head
by the runtime (timewarp), and where it moves in past the picture the runtime stretches the edge pixels. Each eye was
submitted cut exactly to its own field of view (`fov_crop`), and the symmetric picture both eyes are drawn with ends at
the outer and bottom edges of the eyes' fields of view: there was nothing past them. Now each eye is drawn 5 degrees
wider on every side and submitted with that margin (`vr::fov_margin`, `as_rendered` widens the drawn and the submitted
field of view; `WWHD_VR_FOV_MARGIN=<degrees>`, 0 off). Android and Steam Frame - Native (2026-11-03: black edges in a turn); elsewhere 0, as before. The same pixels cover the
wider view (about 8 % fewer per degree). `vr_test` checks the crops. Not yet tried in the headset.

**A crash in the Adreno driver, and a Quest-only guard (2026-10-31).** The first Quest 3 session with multiview on
crashed within a minute (`crash-20261031-001430.log`): signal 11 in `vulkan.adreno.so` `vkCmdEndRendering`, a read at
0x4e0 (the driver's rendering state null), the record worker replaying (`PodCommands::replay`). The last lines: the
two-eye targets made again at a new size ("shares eye 0's image again after a rescale") and dynamic resolution's
neighbouring levels made. An end of rendering with no rendering begun on that command buffer is what the driver met;
NVIDIA's lets it pass. Where it comes from is not found yet (no Quest at hand). On a Quest (`quest_theater::device`)
`loader.cpp` now wraps `vkCmdBeginRendering`, `vkCmdEndRendering` and `vkBeginCommandBuffer`: each command buffer's
open rendering is tracked, and an end with none begun is left out and logged (`[vulkan] Quest: an end of rendering
with none begun ... left out (N so far)`), so a log shows where instead of a crash. The presenter's own begin and end
(`present.cpp`, loaded straight from the driver) pair in one function and are not wrapped. Not yet tried in the headset.

**The guard in the headset, a second crash, and its cause avoided (2026-10-31).** With the guard
(`crash-20261031-004953.log`) it left out one end of rendering, right after the two-eye targets were re-paired after a
rescale (the picture's aspect at the start) and dynamic resolution's neighbouring levels were made: the place of the
first crash. The game crashed later in the Adreno driver all the same (a garbage address, another command of the record
worker's replay), right after dynamic resolution changed scale (0.875x to 1x). So both crashes follow a rescale of
multiview's two-eye targets. Not found which command goes wrong (multiview also keeps eye 1's commands in a stream of
their own); on a Quest both rescales are now avoided:
- Dynamic resolution stays off on a Quest whatever the setting (`dynres.cpp` `set_min`; `WWHD_DYNRES_QUEST=1` uses the
  setting). It could not help there: the Quest's frame rate is held by the CPU (it went down at 90 % GPU busy and back
  up at 55 %, the game at 24 to 30 fps throughout).
- Two-eye targets are not re-paired after a rescale on a Quest (`surfaces.cpp` `repair_after_rescale`): eye 1 keeps
  its own image and those targets are drawn per eye, as the 658 targets a frame that are not two-eye already were.
- The guard stays. Multiview did little in that session (27 draws a frame for both eyes, 2,327 "not now"): the
  rest is drawn per eye. Not yet tried in the headset.

## Steam Frame - Lepton

Lepton v3.0.5 runs LineageOS Android 11 (`RQ3A`) and installed the APK (`minSdk 33`), but reports a newer Android to
apps. The first log (2026-10-30): the launcher opened, then 6 s later AndroidX's profile installer (pulled in by
`androidx.core`) took its Android 13 path and ended the app (`NoSuchMethodError: PackageInfoFlags.of`). The VR
manifest no longer starts it (`ProfileInstallerInitializer`, `tools:node="remove"`).

Lepton is now a target of its own, Steam Frame - Lepton (docs/VR_STEAM_FRAME.md): `build_native.sh
TARGET=steamframe-lepton` (API 30) and the APK flavor `steamframeLepton` (`minSdk 30`), with Android 12/13 functions
looked up at run time (`platform/android_late_api.h`); the `vr` flavor above stays the Quest and Android XR build.
What is still to check there is listed in docs/VR_STEAM_FRAME.md, "Not checked".

## Still to check (on a headset)

- the launcher opens as a panel, its settings save, extracting a `.wua` works (the copy, then `wwhd-extract`), and
  Play hands over to the game in VR;
- the app starts, the OpenXR session reaches "running", and the game shows in both eyes with head tracking;
- the eye-copy path without a window surface (`eyes_pending`), and with one if the headset gives the app a surface;
- the controllers (Touch Plus; Steam Frame's), haptics, the VR menu;
- leaving and coming back (the system menu, taking the headset off), and quitting;
- the Linux-only crop of each eye's field of view (`kOwnFieldOfView`, docs/VR_MENU.md) is active on Android too:
  whether menus look right on the headset's runtime;
- frame rate (expected low; performance comes later), memory (Quest 3: 5.75 GiB for an app);
- Steam Frame's Android apps (Lepton): reported to offer OpenXR 1.0 (the game asks for 1.0) and to launch only
  `MAIN` + `LAUNCHER` activities (the setup screens are one). Not tested.

The on-device setup (the release app that builds the game on the device) is not changed for VR. Whether a headset
store or runtime allows loading a game library the app compiled itself is not documented by Meta or Valve; the PC
build above does not need it.

## Multiview falls back (2026-10-31)

A Quest 3 log (v0.1+577, Adreno 740, driver 512.837.9) reported left-eye artifacts; no Vulkan errors. Of about 3,400
eye-0 draws a frame only 215 were drawn for both eyes, 3,192 "not now (state)": each frame mixes multiview passes with
per-eye passes on the same two-layer images (on the PC nearly every draw is multiview). Checked against Meta's Vulkan VR
guidance and the spec (2D array targets, view mask 0b11, layerCount 1, no geometry or tessellation stages): as required.
The lead: a pipeline gets a multiview twin only when both stages' uniform blocks fit
`maxDescriptorSetUniformBuffersDynamic` (draw.cpp), which Adreno drivers report as low as 8 (about 15 on NVIDIA).
Diagnostics: the device's limits are logged at start (`[vulkan] limits: ...`), and "not now (state)" is split by cause
(over the dynamic limit, no multiview base, compiling, eye/thread/held stream, AO at full size, light atlas depth, eye
1's held lookups). The tests for the reporter, one env.txt line each: `WWHD_VK_MULTIVIEW=0`,
`WWHD_VK_MULTIVIEW_DRAWS=0`, `WWHD_VR_FOV_MARGIN=0`, `WWHD_VK_KEEP_DESCRIPTORS=0` with `WWHD_VK_BUFFER_CACHE=0`,
`WWHD_VK_FASTPATH=0`, `WWHD_VK_BATCH_RUNS=0`, `WWHD_VK_RECORD_WORKER=0`, `WWHD_VK_INDEX_RUNS=0`. Should the dynamic
limit be the cause, multiview pipelines over it would take plain uniform buffers instead.

**Then, the same day:** multiview stays on on a Quest, for its log to say why draws fall back
(`WWHD_VK_MULTIVIEW=0` turns it off; `WWHD_VK_MULTIVIEW_DRAWS` now follows it unless `=0`). The sun atlas's stream and thread on by default on a Quest
(`atlas_default_on`; `=0` turns each off): the Quest is CPU-bound and the thread takes the atlas's 650 to 800 draws a
frame off the render thread. Not yet tried in the headset; the black squares of the first run with all three on are
the thing to watch. 6 submission slots on a Quest as on the desktop (`backend.h` `submission_slots`), 4 on phones.

## Internal resolution 0.75 on a Quest (2026-10-31)

Asked for after a Quest 3 user's logs, which showed 22–29 fps at 90 Hz. A Quest now draws the 3D view at a fixed 0.75x of the internal resolution: 960x540 of the 1x setting's 1280x720, with VR's eye picture as tall in proportion. 0.70 was asked for. 0.75 is the nearest of the sizes the renderer already draws at below 1x (dynamic resolution's levels 1, 0.875, 0.75, 0.625, 0.5), so the code drawing below 1x is the same, tested code.

- **How** (`dynres.cpp` `fixed_factor`, returned by `dynres::latch` while dynamic resolution is off). The factor never changes, so none of dynamic resolution's image swaps, kept images or levels made ahead happen. `dynres::on()` stays false, which keeps multiview's two-eye targets and avoids the Adreno crashes that changes of scale caused. The renderer starts at the factor (`surfaces.cpp` `g_dyn_frame`, from `dynres::fixed_factor()`), so its first targets are made at that size, not at full size and then rescaled at the first frame boundary. Rescales are what preceded the Adreno crashes. Shadow maps keep the internal resolution, as with dynamic resolution.
- **Where:** Quest headsets only (`quest_theater::device()`). PC VR and desktop draw at the internal resolution as before. `WWHD_RES_FIXED=f` sets a factor for a run, and `WWHD_RES_FIXED=0` draws a Quest at full size.
- **The log** says `[dynres] fixed 0.75x of the internal resolution ...` once. The performance report's line reads `Dynamic resolution: off; fixed 0.75x of 1x (3D view 960x540)`.
- **Expect little from it on a Quest.** The user's logs point to the CPU running the game (passes of 28–44 ms), not the GPU. A smaller picture lightens the GPU and the render thread, but not the game thread.

Not yet run on a Quest: the Android build was not built here.

**In the Graphics menu (same day):** the internal resolution now offers 0.5x (640x360) and 0.75x (960x540) next to 1x to 3x. That covers the F1 Settings window, the item menu's settings page, and the R key's cycle. Below 1x is 1x drawn at this fixed factor (`gfxvk::set_res_scale` -> `dynres::set_fixed`). `requested_res_scale` returns the product, so the menu shows the choice and saves it as `resScale` (0.5 or 0.75). A Quest or Steam Frame with nothing saved starts at 0.75. On a Quest or Frame a new choice is drawn from the next start, and the menu says so: the size drawn is the one at the first frame, since a rescale preceded the Adreno crashes there. On PC it changes at once, as dynamic resolution's levels do. The Metal renderer (macOS) still draws 1x for the two new choices.

## The Steam Frame (2026-10-31)

A Steam Frame runs the Android build in Lepton, Valve's Android layer. A user's diagnostics (`diagnose-FramePort-diag-...zip`) and video showed stutter: SteamVR's compositor counted 7,201 frames shown at 72 Hz, 4,878 of them reprojected (68%), so the game gave about 23 fps. That is the same CPU limit as the Quest's.

**Recognised as a standalone headset:** the Frame's model is not a Quest's, so none of the Quest-only paths applied to it. `quest_theater::steam_frame()` recognises Lepton's build fingerprint (`ro.build.fingerprint` starting `lepton/`). `standalone_headset()` (a Quest or a Frame) now gates:
- the end-of-rendering guard (`loader.cpp`);
- no re-pairing of two-eye targets after a rescale (`surfaces.cpp`);
- dynamic resolution forced off (`dynres.cpp` `set_min`);
- the record worker on by default (`backend.cpp`);
- the 0.75x default above.

Its GPU is an Adreno 750 (`gen70900`) under the same Qualcomm driver family.

**Also seen in that bundle, not ours to change:**
- The game's own log is not in it. It writes `captures/wwhd.log` in the app's files, not to logcat.
- Lepton loads Valve's Fossilize layer into the game's Vulkan (`Vulkan debug layer list: VK_LAYER_fossilize`).
- The Frame's tracking service rebuilt its room map while the game ran: tasks of 4 to 19 s, and many of 15 to 150 ms.
- Lepton has no thermal service (`No Thermal HAL service on this device`), so the heat guard reads nothing there.

## Multiview off on a Quest (2026-10-31, later)

The player's footage of the left eye showed black squares again:
axis-aligned, tile-shaped holes with stepped edges along the horizon and the far sea (title screen, Outset), slivers of
sea colour inside them, the near water, island, Link and HUD drawn. Their build had multiview on and the atlas paths
off; the first run with squares had all three on, the runs with all off had none: multiview is the common factor. The
likely mechanism: with most draws per eye, passes drawing both layers of the shared two-eye images alternate with
passes drawing layer 0 alone (the left eye) on the same images, which a tiling GPU loads and stores between. Off by
default on every Android device again (`multiview_default_on`); `WWHD_VK_MULTIVIEW=1` turns it on for the fallback's
log. The atlas stream and thread stay on on a Quest. To turn it on again: nearly every draw multiview (the causes the
log names fixed), so the images are never written both ways in a frame.

## Multiview's uniform limit and mixed passes (2026-11-03, mobile GPU builds only)

From the Quest 3 audit (QUEST3_PERFORMANCE_AUDIT.md, QUEST3_IMPLEMENTATION_ROADMAP.md 2.1): compiled only into mobile GPU
builds (`mobile_gpu.h`: Android, arm64, or a desktop test build with `-DWWHD_TEST_MOBILE_GPU_PATHS=ON`); Windows and
Linux on x64 keep their code. **Multiview is on by default on a Quest (standalone) again, with these fixes and the
mixed-pass guard** (the user's choice, 2026-11-03; `stereo_switches.h` `multiview_default_on`: `quest_theater::device()`);
other Android devices (Steam Frame - Lepton, phones) stay off. `WWHD_VK_MULTIVIEW=0` in env.txt turns it off on a Quest.

- **Some blocks dynamic, not none** (`uniform_plan.h`): a pipeline over the device's dynamic uniform buffer cap
  (Adreno: 8) used to bind every block as a plain buffer, so eye 1 could not replay its draws and they could not be
  multiview (`kMvNotDynamic`; 3,192 of about 3,400 draws a frame on a Quest log). Now the vertex stage's support block
  and blocks, then the pixel stage's, are dynamic up to the cap less 2 (room for multiview's per-view copies); the
  rest stay plain. A draw whose eye-shifted blocks are among the dynamic ones replays (also with multiview off) and can
  be multiview; the others stay per eye as before. Saved pipelines match by the same choice (`headstart.cpp`).
  `WWHD_VK_PARTIAL_DYNAMIC=0` turns it off.
- **The multiview twin within the cap** (`multiview_pipeline.cpp`): a twin doubles each per-view block's descriptor;
  near the cap that made a layout over it (invalid; a driver need not reject it). Over the cap the twin now fails and
  its draws stay per eye (`pipeline failed`).
- **No mixed passes** (`draw.cpp` `kMultiviewNoMix`): a two-eye image written one eye at a time takes no multiview draws
  that frame and the next (`not now (its targets were drawn one eye at a time ...)` in the fallback log), so a tiled GPU
  never writes one both ways in a frame, the mix the black squares came with. `WWHD_VK_MULTIVIEW_NO_MIX=0` turns it off.
  - **Held for 90 frames since the last per-eye write (2026-11-04).** It held for this frame and the last. A Quest 3 run
    with it showed black squares in the sky only. An image written one eye at a time every frame (the sky's) took
    multiview draws again two frames later, at the start of that frame, before its per-eye draw marked it again. So it
    was written both ways in that frame, and the squares flickered.
  - Now such an image stays per eye from the start of every frame. `WWHD_VK_MULTIVIEW_NO_MIX_FRAMES=n` sets the hold
    (1: as before).
  - The check comes after the draw's own reasons (its twin, its blocks). A draw refused here could otherwise be
    multiview, so a draw that could never be multiview marks its images and keeps them per eye.
- **Test on a desktop:** the test build with `WWHD_VK_TEST_DYNAMIC_UBO_LIMIT=8` makes Adreno's choices on another GPU.

Not run anywhere yet: built in the desktop test build only.

## Threads, CPU level, build options, frame synthesis (2026-11-03)

- **XR thread scheduling** (`openxr_session.cpp` `declare_thread`): with `XR_KHR_android_thread_settings` offered, the game
  thread (application main), render thread (renderer main) and end-frame thread (renderer worker) are declared to the
  runtime once per session, as DrBeef's Quest ports do. Log: `[vr] XR thread scheduling: ...`.
- **CPU Boost while stalled** (`cpu_boost`, standalone builds, `XR_EXT_performance_settings` offered): Boost while the
  game has no new frame (the render thread showing its last one again: loads, shader waits), Sustained High (the
  runtime's default) 2 s after the last such frame. Log: `[vr] CPU level: ...`.
- **Memory** every 10 s: `[vr] memory: the game N MiB resident (peak M MiB); the system A of T MiB available`.
- **Build options** (`android/build_native.sh`): ThinLTO (`WWHD_LTO`) and `CPU=quest3` (`-march=armv8.2-a
  -mtune=cortex-a78`, what both of the XR2 Gen 2's core types run) are **on by default for a `VR=1` build without a
  `TARGET`** (Quest, Android XR; the user's choice, 2026-11-03; not yet measured on the headset). `CPU=generic` and
  `LTO=0` turn them off; a VR build names its flags every time, so the build folder's cache never keeps the last build's.
  The phone build and `TARGET=steamframe-lepton` are unchanged (generic, no LTO).
- **Frame synthesis** off by default on Quest as everywhere (the user's choice, 2026-11-03), until tested on a Quest.
- **The scene's drawing threads** (`WWHD_VK_SCENE_THREAD`, `WWHD_VK_SCENE_TWO`; docs/VR_SCENE_THREADS.md) on by default on
  a Quest (standalone) too (the user's choice, 2026-11-04; `stereo_switches.h` `scene_default_on`, the atlas thread's
  rule): multiview, which the second drawing thread's span needs, is on there again, and the Quest is CPU-bound. They
  start in frames of 1,400 scene draws or more. Never run on a Quest before: the log's `[vulkan scene thread]` lines
  give their cost a draw and the render thread's waits at the joins. `WWHD_VK_SCENE_THREAD=0` in env.txt turns them
  off; other Android devices keep them off (`=1` turns them on). The thread placement on core groups is Windows only.

None of this has been built for Android or run on a headset here.

**Not yet tried on a Quest with multiview on again:** watch for the black squares (left eye, horizon, far sea) and
the Adreno crash after a resolution change (dynamic resolution stays off with multiview on Qualcomm's driver). The
fallback log (`[vulkan multiview]`, every 600 frames) shows how many draws went both eyes and why the others did not.

## Steam Frame - Lepton with the Quest 3's settings (2026-11-04)

The user's choice: Lepton takes the standalone Quest 3's defaults (`quest_theater::standalone_headset`, a Quest or
Lepton, where `device` was a Quest only):
- multiview on, with the mixed-pass guard (`stereo_switches.h` `multiview_default_on`);
- the sun atlas's stream and thread on, and the scene's drawing threads (`atlas_default_on`, `scene_default_on`);
- 6 submission slots (`backend.h` `submission_slots`).

Already shared before this: the fixed 0.75x resolution, the 0.75x shadow maps, and dynamic resolution only with
multiview off (Qualcomm's driver).

Frame synthesis is off by default on both, and on every other build. A Quest test build that day had `auto`; settings
format 15 moves the `auto` it saved back to `off`.

Phones keep their own defaults. `WWHD_VK_MULTIVIEW=0`, `WWHD_VK_ATLAS_STREAM=0`, `WWHD_VK_ATLAS_THREAD=0`,
`WWHD_VK_SCENE_THREAD=0` and `WWHD_VK_SUBMISSION_SLOTS=n` in env.txt change each one. Not run on Lepton yet.

## The scene's drawing threads on the fast cores (2026-11-04)

Android's form of the PC's core groups (`core_groups.h`, `docs/VR_SCENE_THREADS.md`): a Quest has no shared-cache
groups, its cores differ in speed instead. The render thread, the game thread and the record worker were already on
the fastest cores (`perf_hint::add_current_thread`, `[perf] thread N on the 4 fastest cores` in the log); the scene's
two drawing threads, on by default on a Quest since 2026-11-04, were left to the scheduler and drew at 17 to 21 us a
draw in a Quest 3 player's Forsaken Fortress log (7 to 10 on a PC). They now join the render thread on the fastest
cores (`perf_hint::pin_to_fastest_cores`, the same log line once for each). The atlas's thread and the end-frame
thread stay where the system puts them: the other cores are the slow ones. `WWHD_VK_PLACE_THREADS=0` or
`WWHD_FAST_CORES=0` leaves them unplaced. Built for Windows only so far (no Android build here); not run on a Quest.

## The scene drawn once for both eyes again on a Quest (2026-11-05)

A Quest 3 player's logs (three sessions, 72 Hz): 30 to 36 frames a second, the GPU 75 to 91 % busy at about 22 ms a
frame, the VR work on the game thread 0.2 to 0.4 ms. The scene's own colour and depth targets (924x1000, `F5C7B800` and
`F58F6000`) were "no eye 1 twin" all session: 160 to 460 of their draws a frame drawn once per eye. They were made at
1x on the title screen and rescaled once to the Quest's fixed 0.75x, and since 2026-10-31 a Quest re-paired nothing
after a rescale (the copy, made where the rescale happens while the draws resolve their targets, had been followed by
a crash in the Adreno driver).

Now a Quest (and the Steam Frame) re-pairs them between frames: `repair_after_rescale` queues the rescaled two-eye
target, and `run_pending_eye_repairs` (`surfaces.cpp`) pairs it with eye 1 again right after the headset's frame is
submitted (`backend.cpp`, the TV's presentation), when the frame's draws are all submitted and none of the next is
recorded; the copy goes first in the next command buffer. Logged: `[vulkan multiview] Quest: target ... paired with eye 1
again between frames`, or `... has no matching eye 1 target yet`, or a failure (then it stays per eye). Other platforms
re-pair as before. Not yet tried in the headset.

**Full-size torchlight off on a standalone headset** (2026-11-05): always, whatever was saved (`render::full_light`);
the player's view was better with it off and the GPU is the limit there.

**The log for the re-pairing** (2026-11-05): each rescale queued on a Quest (`... rescaled to WxH (format F) at frame N:
its pairing ... waits for the frame's end`), each pairing done between frames (`... paired with eye 1 again between
frames at frame N: copied in X ms`), a target with no match of the other eye, and a failure (it stays drawn per eye);
the 10 s multiview summary adds `targets paired again after a rescale: ...` (counts, the last target and how many
frames ago), and a crash report's context has `multiview_repairs=` with the same, at most 1 s old. Reports of other
Adreno driver crashes (an X-Plane developer's, Adreno 750 device losses under Unreal) found the fault surfacing frames
after its cause, with no validation error, once the driver kept an object the app had destroyed: the crash report
says how near a re-pairing it was.

## Multiview: the re-pairing run in VR, one-eye targets made two-eye (2026-11-06)

A Quest 3 log (`b632e78`): multiview fell from 53-67 draws a frame for both eyes to 7 after the first 20 s, the GPU 75-87 %
busy at 21-24 ms a frame (30-40 fps; about 13 ms and 59 fps before). Its target lines:
- **The scene's colour and depth (924x1000), "no eye 1 twin"**, up to 439 draws a frame: queued for pairing again after
  the 0.75x rescale (above) but never paired, "0 paired again (20 queued)". The queue ran only in the TV present, which
  returns early in a standalone headset (the window not shown). It now also runs in `swap` right after the frame's eyes
  are recorded and flushed (`backend.cpp`), between frames as intended.
- **A target with one eye's image** (the scene's colour address at 641x360), 10-138 draws a frame: made before stereo or
  first as a texture, it never had a second layer. A draw that misses multiview for that alone flags it
  (`Surface::wantTwoEyeFrame`, `draw.cpp`); at its next use in a later frame `rescale` rebuilds it as a two-eye image (its
  pixels copied) and its pairing with eye 1 follows between frames. `[vulkan multiview] ... had one eye's image: made
  two-eye`. On a standalone headset the rebuild waits for the frame's end, as the pairing does (queued, then run in
  `run_pending_eye_repairs` right after the frame's eyes are submitted, before the pairings it queues); PC builds do it at
  once. `WWHD_VK_MULTIVIEW_PROMOTE=0` leaves such targets per eye, nothing else changed.
- **The light pass's small buffers** (format 2070), eye 1's at 480x270 against eye 0's 462x500: eye 1's rescale to the
  matching size could not be queued while the never-run queue still held it; with the queue run each frame it is.
- **"Drawn one eye at a time recently"** (167-330 draws a frame): its main source, the torches' and searchlights'
  per-eye passes, is gone with them not drawn on a standalone headset (2026-11-06).
Not yet seen in a headset.

### The promotion queued from the miss itself (2026-11-07)

Two Quest 3 logs (`f505de0`): the re-pairing ran (all 12 rescaled targets, the scene's colour and depth among them, paired
between frames, no crash), but no one-eye target was made two-eye: the scene's colour address at 641x360 kept 75-158 of
its draws a frame drawn twice. The promotion was queued from `rescale`, which a target's lookups no longer reach once
kept (the lookup memos). `note_one_eye_target` (from `multiview_draw`'s miss) now flags it and, on a standalone headset,
queues it at once (its own lock: drawing threads call it); the frame's end rebuilds it, a flag of that same frame
included. `[vulkan multiview] Quest: ... has one eye's image: made two-eye at the frame's end`, or `... not eye 0's (eye
N): not made two-eye` when it belongs to no eye's map.

### Multiview diagnostics without a cost per draw (2026-11-07)

The targets that miss multiview were logged by formatting a string and updating a map for each missed draw (about 1-2 us
a draw on a Quest, 50-176 a frame in the latest logs, and from the scene's drawing threads with no lock). Each target now
counts its own (`Surface::mvMissDraws`, one relaxed atomic add); `take_multiview_target_stats` collects and resets them
every 10 s for `multiview_log`. The per-eye hold ("its targets were drawn one eye at a time recently", 436-718 draws a
frame in those logs) is now listed too: `[vulkan multiview]   held per eye: <target>: N draws per frame refused; held by
a <sun shadow / point light / other ...> draw (<why that draw was per eye>)`, from two byte stores when a draw marks a
target (`perEyeWhy`, `perEyeKind`) and one atomic add when the hold refuses one (`mvHeldDraws`).

## The headset taken off: the game held, the record worker stopped (2026-11-07)

A Quest 3 player took the headset off and put it back on: the game crashed in the Adreno driver (`vulkan.adreno.so`,
signal 11 at 0xf8) in a `vkCmdDrawIndexed` the record worker replayed (`RecordWorker::run`, `PodCommands::replay`), right
after `CPU level: Boost (the game is stalled)`. Not the multiview pairing (its last 12,262 frames before), nor the
promotions (at the start). On a standalone headset (`vr::headset_away`, vr.h) the headset counts as away while the app is
in the background (Android's lifecycle, `backend.cpp lifecycle_watch`) or the session is out of focus (OpenXR's state
not focused, `openxr_session.cpp`). Meanwhile:
- the game is held, as with the VR menu (`menu_pause_pass`): no logic, timers or effects advance;
- the record worker is stopped at the next frame's start (`record_worker_pause_service`: its commands drained, its
  thread joined), so nothing it recorded is replayed across the pause and the surface recreated after it; it starts
  again, with nothing kept, 30 frames after the headset is back.
The log says `[vr] headset away: ...` / `[vr] headset back: ...` and `[vulkan record worker] stopped while the headset is
away` / `started again`. Not yet seen in a headset; `WWHD_VK_RECORD_WORKER=0` keeps the worker off altogether.

## The per-eye hold only for images drawn one eye at a time in most frames (2026-11-07)

The new `held per eye` lines named the Quest's main blocker: the scene's depth (924x1000) held per eye with 499-562 draws
a frame refused, held by a draw on the scene's thread refused while eye 1 held commands ("not now (state)",
`multiview_draw`), less than once every two frames. Each re-armed the 90-frame hold, so the depth stayed per eye all
session; and the second drawing thread, which takes the frame's longest run of two-eye passes, had 0-1 draws a frame.
The hold is for an image written one eye at a time every frame (the sky's: black squares when it lapsed between them).
Each image now keeps its last 8 frames' marks (`Surface::perEyeHistory`): marked in this frame or the last it is per eye
as before; marked in `kMixRepeatMarks` (4) or more of the last 8 it keeps the 90-frame hold; marked now and then it is
not held past the next frame, the frames between having no per-eye draw into it. `WWHD_VK_MULTIVIEW_NO_MIX_REPEAT=n`
sets the count, `=0` restores the hold for every mark. Not yet seen in a headset: watch for black squares along the
horizon (the sky's) and the `held per eye` lines.

### What the next log showed, and the two causes fixed (2026-11-07, later)

The next Quest 3 log (the 18:12 build): 34.9 fps on average, GPU 20.4 ms a frame, the depth still held per eye with
150-580 draws a frame refused: the adaptive hold changed nothing, because the depth was marked in 4 or more of 8 frames.
Its last marks were "pipeline building" and "not now (state)". Both are fixed:
- **A pipeline's two-eye twin still being built** (`kMvBuilding`): the draw went one eye at a time for frames in a row
  as each new shader came (268 to 605 pipelines in the session). Such a draw into two-eye images no one holds per eye is
  now left out of the frame for both eyes (`kMvSkipBuilding`, as `skip_while_made` leaves out a new pipeline's draws),
  at most 30 frames in a row for one pipeline, then per eye as before. `WWHD_VK_MULTIVIEW_SKIP_BUILDING=0` turns it off.
- **Eye 1's held commands during the atlas's overlap** (`kMvState`): the render thread refused instead of flushing
  them, since the flush ends the sun atlas thread's overlap. It now flushes for a draw whose targets passed the two-eye and
  mixed checks (`kMvAtlasJoined`, rare: under one a frame). `WWHD_VK_MULTIVIEW_JOIN_ATLAS=0` refuses as before. The scene
  thread's own case cannot flush them and is counted on its own: "not now (state, on the scene's thread)".

The log also ended with `[runtime error] GX2 render thread: record submission outlived its worker` as the headset came
off: the frame slots kept the stopped worker's serials. The stop drains the worker, so they are now cleared with it.

## PGO on the Quest 3 build (2026-11-07)

ThinLTO is already on for the VR build (`LTO=1` by default). `PGO=generate android/build_native.sh` (with `VR=1`) makes an
instrumented build in `android/build/game-vr-pgo-gen`. It runs slower, so use it for a training session only. It writes
`captures/wwhd-pgo-<pid>.profraw` in the app's folder (`Android/data/org.wwhdrecomp.wwhd/files`) as the headset comes
off, every 3,600 frames and at the quit; the log says `[pgo] profile written`. Merge with the NDK's own `llvm-profdata`
(the same LLVM as its clang):
`$ANDROID_HOME/ndk/30.0.16248370/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-profdata merge -o quest.profdata *.profraw`,
then `VR=1 PGO=/path/quest.profdata android/build_native.sh`. The profile goes stale as the code changes (unmatched
functions are only optimized without it), so retrain after large changes.

## Black squares in the sky again: a global barrier when an image switches eyes (2026-11-08)

With the shorter per-eye hold and the new-shader skip (2026-11-07), a Quest 3 player saw the black squares in the sky
again: images held per eye less often were written both ways in some frames. Rather than hold them longer (the
multiview gain lost), the cause is fixed: each pass's barriers (`transition_image`) name only its own surface's layer,
and Quest's GPU driver then lets passes on the two layers of one image overlap, losing blocks of it, as it did for a
layer's clear (`wait_before_layer_clear`). A pass into a two-eye image that switches between both eyes (multiview) and
one layer at a time, this frame or the last, now starts after a global memory barrier (`mix_barrier`, draw.cpp; the
eye 0 surface's `mvPassFrame` / `layerPassFrame`). An image written only one way gets none, so frames without a mix
cost nothing. The log: `[vulkan multiview] passes switching a two-eye image ...: n per frame`.
`WWHD_VK_MULTIVIEW_MIX_BARRIER=0` turns it off. Not yet seen in a headset.

## The Quest's fixes on Steam Frame - Native (2026-11-08)

Steam Frame - Native (Linux on arm64) already had the mobile GPU paths (`WWHD_MOBILE_GPU_PATHS`): the per-eye hold,
the new-shader skip, the atlas flush, the mix barrier. Two Quest fixes were Android-only and now apply there too:
- the pairing after a rescale and the promotion of one-eye targets wait for the frame's end (`run_pending_eye_repairs`;
  `WWHD_REPAIR_BETWEEN_FRAMES`, surfaces.cpp), as on a Quest, instead of mid-frame;
- the headset taken off (the session out of focus): the game held and the record worker stopped, its frame slots'
  serials cleared (`vr::headset_away`, `record_worker_pause_service`).

## Black squares: draws that sample their own target, now drawn for both eyes (2026-11-09)

The player logs' main blocker of multiview was the main colour target (`F5C7B800`, 924x1000) held per eye with ~413
draws a frame refused, "held by a other draw (a stage eye 1 binds itself)": about 2 draws a frame that sample the target
they draw into. Eye 0 binds a copy of it (`feedback_view`), and that copy made the stage not replayable for eye 1
(`bind_stage`'s `replayTexture`, own), so those draws went one eye at a time, marked the target, and its passes kept
switching between both eyes and one layer (~12 a frame, each after the mix barrier): the switch the black squares
come with. Player report the same day: the black artifacts appear above Headset resolution 0.65 only.

Now such a texture is an eye texture with `feedback` set (`StageReplay::EyeTexture`): eye 1's replay (`replay_stage`)
and a multiview draw's eye 1 lookups make eye 1's own copy of its own layer (`feedback_view` on the twin, eye 1's
slots) and bind it, view 1's descriptor. The draw then goes to both eyes, the target is not held per eye for it and its
passes stop switching. The log: `[vulkan multiview] draws sampling their own target drawn for both eyes: n per frame`;
the `held per eye: colour target ...` line should drop. `WWHD_VK_FEEDBACK_EYE_REPLAY=0` restores the old way.
Not yet seen in a headset. (The AO substitute and the sun-shadow / point-light eye passes still bind for themselves.)

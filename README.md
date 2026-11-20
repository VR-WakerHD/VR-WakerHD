# VR-WakerHD: The Legend of Zelda: The Wind Waker HD in VR

**VR-WakerHD is a fan-made VR version of *The Legend of Zelda: The Wind Waker HD*.** You play in first person through
Link's eyes, with head tracking, motion controls, your arms as Link's arms and a sword you swing yourself. It runs on
PC VR headsets through OpenXR (SteamVR, Meta Quest Link, Virtual Desktop, Monado) and standalone on Meta Quest 3 and
3S. It is built on [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp), the native PC port
(recompilation) of the Wii U game.

> **Unofficial, and no game included.** VR-WakerHD is not made by or affiliated with Nintendo. You need your own,
> legally obtained copy of the Wii U game: setup builds the game on your own device from your own game files. The
> downloads contain no game code, no game data and no keys.

**[Download the latest release](https://github.com/VR-WakerHD/VR-WakerHD/releases/latest)** ·
[Website](https://vr-wakerhd.github.io/VR-WakerHD/) · [Install](docs/INSTALLATION.md) ·
[Features](docs/FEATURES.md) · [Compatibility](docs/COMPATIBILITY.md) · [Quest guide](docs/QUEST.md) ·
[Controls](docs/PLAYING.md) · [FAQ](docs/FAQ.md)

## Gameplay tutorial

[![The Legend of Zelda Wind Waker HD VR: (VR-WakerHD) Tutorial + Gameplay #1 (video on YouTube)](https://img.youtube.com/vi/a98maOC3x24/hqdefault.jpg)](https://youtu.be/a98maOC3x24)

**[The Legend of Zelda Wind Waker HD VR: (VR-WakerHD) Tutorial + Gameplay! #1](https://youtu.be/a98maOC3x24)** (YouTube): how to
play the Wind Waker HD VR port, with first-person gameplay in the headset. It shows the controls, the motion-controlled
sword and the in-headset menus.

## What is VR-WakerHD?

Three projects are involved, and it helps to keep them apart:

| | What it is |
|---|---|
| *The Legend of Zelda: The Wind Waker HD* | Nintendo's 2013 Wii U game. Not included; you need your own copy. |
| [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) | The open-source native port: it recompiles the Wii U game's code so it runs natively on PC, Linux, macOS and Android, flat-screen. |
| **VR-WakerHD** (this repository) | A VR version built on that port. It adds the OpenXR VR mode, the first-person body, the motion controls, the VR launcher, the Quest 3 build and the VR performance work. |

So VR-WakerHD is a **Wind Waker HD VR port** and a **Zelda recomp VR** project: the game runs natively through the
recompilation (no emulator), and the VR is built into the renderer and the game's own logic rather than drawn on top of
a flat picture. Earlier copies of this project were called **ZeldaWWHD-VR**; it is the same project.

## What makes it real VR

- **First person at Link's eyes**, in true stereo 3D with full 6DoF head tracking, leaning and room-scale movement.
  The world is scaled to Link's arms. Third person is a stick click away.
- **Your arms are Link's arms.** Inverse kinematics put his hands where your controllers are, with natural elbows
  and shoulders.
- **Wind Waker motion controls.** You draw the sword over your shoulder and swing it yourself. The bow is pulled with
  your hand, and you conduct the Wind Waker with your hand.
- **Built into the game**, not a view of a flat screen. Menus, dialogue and the GamePad screen sit on panels in the
  headset, and the HUD is on Link's wrists.

## VR features

- **Head tracking (6DoF)**: the view at Link's eyes; leaning followed; Link turns to face where you look.
- **First and third person**: right stick click toggles; third person uses the game's camera, turned by your head.
- **Motion controllers and arm IK**: Link's arms follow your controllers; hands keep their pose through short
  tracking gaps.
- **Physical sword**: draw it over your shoulder; full swings play the game's attacks; the blade also cuts and hits
  along its tracked path; hit vibration.
- **Combat**: lock-on on the trigger, automatic parry, shield that guards on its own while locked on, grip to block.
- **Items in your hands**: bow, Skull Hammer, the Wind Waker baton, boomerang, grappling hook, telescope, Picto Box.
- **Item wheel at your shoulder**: reach over your shield shoulder, choose, pull the item out.
- **Wrist HUD**: hearts, items, rupees and magic on Link's wrists, shown when you look at them.
- **Menus in VR**: pause, map, bag and the GamePad screen on a panel in front of you; an in-headset settings menu
  (left stick click).
- **Getting around**: smooth or snap turning, "move where you look", sailing views on the Great Sea.
- **Comfort**: depth of field, bloom and ambient occlusion can be turned off; steady hands and smooth camera while
  turning.
- **Performance**: multiview (both eyes in one pass), foveated rendering on Quest, headset resolution and dynamic
  resolution, upscalers (Quest Super Resolution, SGSR, FSR 1, CAS), frame synthesis and refresh-rate choice.

The full list, with the docs for each: [docs/FEATURES.md](docs/FEATURES.md). Every setting:
[docs/VR_SETTINGS.md](docs/VR_SETTINGS.md).

## OpenXR implementation

VR-WakerHD talks to the headset through **OpenXR**, with the game's **Vulkan** renderer
(`XR_KHR_vulkan_enable2`), so it is not tied to one headset maker. It draws both eyes in one pass (multiview) where the
GPU allows, at the headset's own resolution, and submits the menus and the GamePad screen as separate layers. It uses
optional OpenXR extensions where the runtime offers them: refresh-rate selection, frame synthesis, foveation on Quest
and recommended resolution. Design: [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md).

## Supported hardware and platforms

| Platform | Download | Status |
|---|---|---|
| **Linux PC** (x86-64) | `VR-WakerHD-Linux-<version>-x86_64.AppImage` | Played in the headset: SteamVR and native Monado |
| **Windows PC** (x86-64) | `VR-WakerHD-Windows-<version>-x86_64.zip` | Released; SteamVR, Meta Quest Link, Virtual Desktop |
| **Meta Quest 3 / 3S** (standalone) | `VR-WakerHD-Quest3-<version>.apk` | Played standalone; see the [Quest guide](docs/QUEST.md) |
| **Steam Frame** (SteamOS, arm64) | `VR-WakerHD-SteamOS-aarch64-<version>.AppImage` | Built; not yet confirmed on the device |
| macOS | — | The base port runs; no VR build |

**Headsets tested most:** PlayStation VR2 (through SteamVR, and through Monado on Linux) and Meta Quest 3 / 3S
(standalone and over a Link cable). Any OpenXR headset with a Vulkan-capable runtime should work.
**PCVR** needs a Vulkan GPU. **Wireless streaming** (Virtual Desktop, Air Link) works but is experimental: use the
*Streaming optimal* setting. Details per headset and runtime: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Installation

1. **Download** the file for your platform from the
   [latest release](https://github.com/VR-WakerHD/VR-WakerHD/releases/latest).
2. **Start it.** On Linux, `chmod +x` the AppImage and run it. On Quest, install the APK (for example with
   SideQuest) and open it. On Windows, unzip and start `wind-waker-hd-vr.exe`.
3. **Set up the game, once.** Choose your game files: an extracted folder, or a `.wua` / `.wud` / `.wux` image. Setup
   compiles the game for your device. That takes a few minutes on a PC and longer on a headset.
4. **Play.** The VR launcher opens: choose your headset settings, then **Play VR** (or **Play Flat**).

Step by step, per platform: [docs/INSTALLATION.md](docs/INSTALLATION.md).

## Required original game files

You need *The Wind Waker HD* for Wii U, dumped from your own disc and console: a `.wud` / `.wux` disc image with its
disc key, a Cemu `.wua` archive, or an already extracted game folder (`code`, `content`, `meta`). The USA version is
the one tested most for VR; the base port also supports the European version. Everything made from your game files
(the extracted files, the recompiled code, shader caches) stays on your device and must not be shared.

## Building from source

For developers and for platforms without a release: [docs/BUILDING.md](docs/BUILDING.md) (Windows, Linux and Arch
Linux), [docs/VR_ANDROID.md](docs/VR_ANDROID.md) (Quest), [docs/VR_STEAM_FRAME.md](docs/VR_STEAM_FRAME.md) (Steam
Frame). You need Git, CMake, Ninja, Clang, Python 3 and a Vulkan driver; CMake downloads the OpenXR SDK itself. Code
layout, tests and the upstream project: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Configuration and controls

- **The VR launcher** sets the headset, refresh rate, controls and graphics before you play.
- **In the headset**, click the left stick to open the settings menu. The grips switch tabs.
- **Sword hand** is the right controller and **shield hand** the left; *Left-handed* swaps them. Recentre with both
  stick clicks.

The controller mapping for first person, third person and the boat, plus the sword and the item wheel, is in
[docs/PLAYING.md](docs/PLAYING.md). Every setting is in [docs/VR_SETTINGS.md](docs/VR_SETTINGS.md).

## Development status

**Experimental and in active development.** The whole game is playable in VR. Current releases:
[0.5.1](https://github.com/VR-WakerHD/VR-WakerHD/releases/tag/v0.5.1) (stability update) and
[0.5.0](https://github.com/VR-WakerHD/VR-WakerHD/releases/tag/v0.5.0) (first public release). What changed:
[Releases](https://github.com/VR-WakerHD/VR-WakerHD/releases).

## Known limitations

- **Quest 3 / 3S** is limited by the headset: try 72 Hz with 36 fps, a lower headset resolution and a smaller field of
  view if it is not smooth. See the [Quest guide](docs/QUEST.md).
- **The first start on a headset** is slow, because the game is compiled on the device. The first visit to a new area
  can stutter while its shaders are made.
- **Wireless streaming** can lag or stutter. Use *Streaming optimal*, and a cable if you can.
- **Steam Frame** builds have not been confirmed on the device yet.
- **Less-tested headsets** (Index, Vive, Windows Mixed Reality) should work through OpenXR but have had little
  testing.

## Roadmap

- Higher, steadier frame rates on Quest 3 / 3S (the render thread's work per frame).
- Steadier frame pacing under SteamVR on Linux.
- Steam Frame confirmed on the device.
- More headset and runtime testing, and fixes from player reports.

## FAQ

**Is this an official Nintendo VR game?** No. It is an unofficial fan project, built on the open-source
ZeldaWWHDRecomp port.

**Does it include the game?** No. You need your own copy of the Wii U game; setup builds it on your device.

**Is it an emulator?** No. The game's code is recompiled to run natively (ZeldaWWHDRecomp), and VR-WakerHD adds VR to
that native port.

**Does it work on Quest without a PC?** Yes, Quest 3 and 3S run it standalone. It also works on Quest over Link or
Virtual Desktop as PC VR.

**Which VR headsets work?** Any headset with an OpenXR runtime that supports Vulkan. PS VR2 and Quest 3 / 3S are tested
most.

More answers, including troubleshooting: [docs/FAQ.md](docs/FAQ.md).

## Contributing and bug reports

Bug reports and ideas are welcome in [Issues](https://github.com/VR-WakerHD/VR-WakerHD/issues). Please use the bug
report form and attach your `wwhd.log`; the form shows where to find it on each platform. To change the code, see
[docs/BUILDING.md](docs/BUILDING.md); the VR logic's tests (`vr_test`) need no headset.

## Credits

### VR-WakerHD

VR mode by VR-WakerHD (https://github.com/VR-WakerHD/).

- **Game Tester:** [legaiaflame](https://www.youtube.com/@legaiaflame)

### Original project credits

*(From the original project's README.)*

European-game builds and the executable-derived address map are by
[ElFDA](https://github.com/ElFDA), contributed in [PR #77](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp/pull/77).
The combined integration adds the runtime address audit and regional regression scenarios.
Thanks also to [GreenNaugahyde](https://github.com/GreenNaugahyde) and the MPL-2.0
[ZeldaWWHDRecompAndroid](https://github.com/GreenNaugahyde/ZeldaWWHDRecompAndroid) fork: its EUR
address mapping informed the desktop prototype, and the rendered mip chains and sun depth peeks
(bloom, haze, the sun's glare), the GPU decoder for compressed textures, custom Adreno driver
support and the run/swim speed boost were adapted from or modelled on its work.

The Android port (`android/`, the Android parts of the runtime, the single-screen view) is by
[rhemfur](https://github.com/rhemfur), who also contributed the paced frame interpolation, the
Vulkan presentation and feedback-image work, and Linux/Windows fixes.

GPU address library, shader decompiler and a few reference structures are vendored from
[Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0, see `runtime/third_party/cemu/LICENSE.txt`);
`tools/wudextract.py`, `runtime/src/espresso_fp.c` and parts of the OS layer are ported from or
follow Cemu as noted in those files (in the Vulkan renderer: the vertex-format table and the
shader parser glue). Also vendored: [metal-cpp](https://developer.apple.com/metal/cpp/)
(Apache-2.0, `runtime/third_party/metal-cpp/LICENSE.txt`), [{fmt}](https://github.com/fmtlib/fmt)
(MIT, `runtime/third_party/fmt/LICENSE`) and [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b by
Omar Cornut and contributors, for the settings overlay (MIT, `runtime/third_party/imgui/LICENSE.txt`), and
[ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) by Cedric Guillemet, for the world editor's move and turn
handles (MIT, `runtime/third_party/imguizmo/LICENSE`).
The Cemu archive (`.wua`) reader in `tools/wudextract/zarchive.cpp` is written from the format of
[ZArchive](https://github.com/Exzap/ZArchive) by Exzap (MIT No Attribution; nothing of it is
vendored); [zstd](https://github.com/facebook/zstd) by Meta Platforms (BSD-3-Clause) decompresses
it: release builds compile its pinned 1.5.7 release (URL + SHA-256 in `cmake/Zstd.cmake`) statically
into `wwhd-extract` and include its license in `third-party-licenses/`; source builds use a system
zstd when there is one.
The Linux AppImage starts with the AppImage project's
[type2-runtime](https://github.com/AppImage/type2-runtime/tree/8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa)
(commit 8f39b89, MIT), which links libfuse 3.15.0 (LGPL-2.1), squashfuse 0.5.2 (BSD-2-Clause),
zstd 1.5.6 (BSD-3-Clause), zlib 1.3.2 (Zlib), mimalloc 2.1.7 (MIT) and musl 1.2.5 (MIT) statically;
their licenses and where to get the source are in the AppImage's `third-party-licenses/`
(`AppImage-runtime-README.txt`).

### Upstream projects

- **Original project:** [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp). VR-WakerHD is built on
  its native port and keeps its full Git history; its authors and contributors keep their contributions and the
  history that records them. Its own README (macOS and Android builds, the full flat-screen controls, mods, save
  states) is kept as [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).
- **VR runtime:** the [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (Apache-2.0), downloaded at
  build time; the loader is built from it on Windows.

## License and legal notice

Two licenses apply (see [LICENSE-VR.md](LICENSE-VR.md)):

- **The VR mode** written for this project (the files listed in `LICENSES/VR-FILES.txt`, by VR-WakerHD) is under the
  **GPL-3.0-or-later, with a credit term**. Anyone who distributes it, changed or not, must share their program's
  source under the GPL and keep the credit "VR mode by VR-WakerHD (https://github.com/VR-WakerHD/)" in the source and
  where users see it.
- **The original port's code**, including the changes made to its files, stays under the **Mozilla Public License
  2.0** ([LICENSE](LICENSE)).

Vendored third-party code keeps its own license: Cemu (MPL-2.0), metal-cpp (Apache-2.0), {fmt} (MIT), Dear ImGui (MIT)
and ImGuizmo (MIT); see Credits. The extractor links zstd (BSD-3-Clause), and the OpenXR SDK used by the VR build is
Apache-2.0.

This is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo, or by the original
project's authors. "The Legend of Zelda", "The Wind Waker", "Wii U" and related names are trademarks of their
respective owners and are used here only to describe what this software is compatible with.

The repository contains **no game code, no game assets and no keys**: no executable, no recompiled or disassembled
game code, no textures, models, audio, shaders, screenshots or other material from the game, and no console
encryption keys. It contains only the tools and the runtime written for this project, plus the third-party code
listed under Credits. The game itself is Nintendo's property and is not included.

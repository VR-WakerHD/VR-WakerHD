# ZeldaWWHD-VR

A virtual-reality version of [Zelda: The Wind Waker HD Recomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp),
the native PC port of *The Legend of Zelda: The Wind Waker HD* (Wii U). It adds an OpenXR VR mode: the view at Link's
eyes, tracked head and hands, Link's arms following your controllers, a physical sword, a shoulder-reach item wheel,
and HUD displays on Link's wrists. It is a derivative of the original project and keeps its history; this repository
is a standalone private repository with the original as its `upstream` remote, not a GitHub-native fork (see
[Relationship to the original project](#relationship-to-the-original-project)).

**License.** Two licenses (see [LICENSE-VR.md](LICENSE-VR.md)): the VR mode written for this project (the files in
`LICENSES/VR-FILES.txt`, by VR-WakerHD) is under the **GPL-3.0-or-later, with a credit term**: anyone who
distributes it, changed or not, must share their program's source under the GPL and keep the credit "VR mode by
VR-WakerHD (https://github.com/VR-WakerHD/)" in the source and where users see it. The original port's code,
including the changes made to its files, stays under the **MPL-2.0** ([LICENSE](LICENSE)).

**No game is included.** This repository contains no game code, no game-disc images, no keys and no game assets. To
play you need your own, legally obtained copy of the Wii U game, dumped from your own disc and console. Everything
built from it (the extracted files, the recompiled code in `build/gen/`, shader caches) is generated on your machine
and must not be redistributed; the `.gitignore` keeps it out of the repository.

## Status and compatibility

**Experimental.** The VR mode is playable and under active development. What has been checked, and how:

| | |
|---|---|
| Operating system | **Windows** (x86-64): built, tested in a headset. **Linux**: the VR code is portable and documented, but has **not been compiled or run**. macOS and Android: the base port runs there; VR is not built for them (VR needs the Vulkan renderer) |
| Headset tested | **PS VR2 through SteamVR** (Touch-style controller profile), at 90 Hz, RTX 5080. Other OpenXR runtimes and headsets should work through OpenXR, but have not been tested |
| VR runtime | any OpenXR runtime with `XR_KHR_vulkan_enable2`, set as the active runtime: SteamVR, Windows Mixed Reality, Meta Quest Link, Virtual Desktop (VDXR); on Linux Monado, WiVRn, SteamVR |
| Controller profiles suggested | Khronos simple controller, Oculus Touch (Quest, Rift; PS VR2 Sense through SteamVR), Valve Index, HTC Vive, Windows Mixed Reality |
| Game | the Wii U game, USA (`cking.rpx`, title `0005000010143500`) as tested for VR; the base port also supports the European game |
| Performance | about **55–60 fps** on a 90 Hz headset in the busiest scenes on the tested PC (the render thread is the limit, not the GPU; see [docs/VR_PERFORMANCE.md](docs/VR_PERFORMANCE.md)). The runtime's reprojection covers the gap |

**Known limits:**
- **Wi-Fi streaming:** playing over Wi-Fi (for example with Virtual Desktop) can feel laggy or stuttery. If it does, turn on **Streaming optimal** (work in progress) in the VR launcher (Graphics) or in game (F1 > VR) for a steadier picture.

## VR features

**Confirmed in the headset** unless marked *(new)*: built and unit-tested, not yet tried in a headset.

| Feature | What it does | Docs |
|---|---|---|
| Head tracking (6DoF) | the view at Link's eyes, following your head's rotation and position (leaning up to `vr.maxHeadOffset`); world scale matched to Link's arms or set directly | [VR_FIRST_PERSON_CAMERA.md](docs/VR_FIRST_PERSON_CAMERA.md) |
| First and third person | right stick click toggles; third person uses the game's own camera turned by your head | [VR_CONTROLS.md](docs/VR_CONTROLS.md) |
| Motion controllers and arm IK | Link's arms follow the controllers (two-bone IK with natural elbows and shoulder movement); hands keep their poses through short tracking gaps | [VR_ARM_ANIMATION.md](docs/VR_ARM_ANIMATION.md) |
| Physical sword | draw over the sword hand's shoulder with the grip; the sword follows the controller; full swings attack; *(new)* a physical blade hits along its tracked path; hit vibration | [VR_HELD_ITEMS.md](docs/VR_HELD_ITEMS.md) |
| Swing detection | a full swing (down, left to right, right to left) presses the game's attack; tunable sensitivity | [VR_HELD_ITEMS.md](docs/VR_HELD_ITEMS.md) |
| Item wheel | hold the shield hand's grip over its shoulder, choose with the right stick, pull the hand forward to take the item out | [VR_ITEM_WHEEL.md](docs/VR_ITEM_WHEEL.md) |
| Wrist displays | the game's own hearts, item / B buttons and rupees on Link's wrists, shown when you look at them | [VR_WRIST_HUD.md](docs/VR_WRIST_HUD.md) |
| HUD in VR | the edge HUD drawn nearer the view's centre and smaller (`vr.hudInset`, `vr.hudSize`); A / ZR prompts hidden in VR play | [VR_HUD.md](docs/VR_HUD.md) |
| Menus and the GamePad screen | pause, item, map and bag menus on a panel in front of you | [VR_PAUSE_MENU.md](docs/VR_PAUSE_MENU.md) |


Architecture: [docs/VR_ARCHITECTURE.md](docs/VR_ARCHITECTURE.md). All settings: [docs/VR_SETTINGS.md](docs/VR_SETTINGS.md).

## Prerequisites

**From your own console and disc** (none of it is included or provided):
- a disc image of The Wind Waker HD in `.wud` or `.wux` format (or a Cemu `.wua` archive);
- its disc key, in a `.key` file next to the image with the same base name;
- the Wii U common key: a `common.key` file (16 raw bytes or 32 hex digits) next to the image or in the current
  folder, or the `WIIU_COMMON_KEY` environment variable.

**Tools, every platform:**
- Git.
- CMake 3.20 or newer.
- Ninja.
- Clang. Microsoft's `cl.exe` is not supported: the recompiled game needs Clang's `musttail`.
- Python 3 with `pycryptodome` (`pip install pycryptodome`), for `tools/wudextract.py` and the recompiler.
- A Vulkan driver for your GPU, with Vulkan 1.3, or 1.1/1.2 with `VK_KHR_dynamic_rendering`.

**VR:**
- an OpenXR runtime with Vulkan support, set as the active runtime;
- the OpenXR SDK, which CMake downloads itself (Khronos release-1.1.63, SHA-256 checked);
- on Windows, the loader `openxr_loader.dll`, built from that SDK next to the game;
- on Linux, the distribution's `libopenxr_loader.so.1`, opened at run time.

The first configure needs internet access: CMake downloads pinned sources of what it does not find (OpenXR SDK,
and on Windows glslang, SDL3, zlib and LZ4).

Optional:
- the `decomp` submodule (`git submodule update --init decomp`), only for the decompilation tools;
- `capstone` (`pip install capstone`), for the disassembler helper.

## Get the source

```bash
git clone https://github.com/VR-WakerHD/VR-WakerHD.git ZeldaWWHD-VR
```

```bash
cd ZeldaWWHD-VR
```

```bash
git remote add upstream https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp.git
```

## Prepare your game (all platforms)

```bash
python3 tools/wudextract.py game.wux extract game
```

That writes `game/code/cking.rpx`, `game/content/...` and `game/meta/...`. Then recompile the game's code to C. This
writes `build/gen/` and stays on your machine:

```bash
python3 tools/recomp/recomp.py game/code/cking.rpx build/gen
```

The build stops with a clear message if `build/gen` is missing. At start-up the game checks that
`game/code/cking.rpx` matches the recompiled code. To build without the game (tests only):

```bash
python3 tools/recomp/stubgen.py build/gen-stub
```

Then configure with `-DGEN_DIR=$PWD/build/gen-stub`.

## Building on Windows

VR is part of the Vulkan build (`WWHD_VR=ON` and `WWHD_VR_LAUNCHER=ON` by default). The VR build was made and tested
with **llvm-mingw**, the release toolchain. The steps are from [docs/VR_BUILD_WINDOWS.md](docs/VR_BUILD_WINDOWS.md),
from Git Bash or MSYS2; the `/c/deps` paths are examples.

1. Get llvm-mingw 20260922:
   ```bash
   curl -fsSL -o llvm-mingw.zip https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip
   ```
   ```bash
   echo "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666  llvm-mingw.zip" | sha256sum -c - && unzip -q llvm-mingw.zip -d /c/deps
   ```
2. Vulkan headers, and an import library for the tests that call Vulkan directly (the game loads `vulkan-1.dll` from
   your driver):
   ```bash
   curl -fsSL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.309.tar.gz | tar -xz -C /c/deps
   ```
   ```bash
   (echo "LIBRARY vulkan-1.dll"; echo EXPORTS; /c/deps/llvm-mingw-20260922-ucrt-x86_64/bin/llvm-readobj --coff-exports /c/Windows/System32/vulkan-1.dll | awk '/Name: vk/{print $2}') > /c/deps/vulkan-1.def
   ```
   ```bash
   /c/deps/llvm-mingw-20260922-ucrt-x86_64/bin/llvm-dlltool -m i386:x86-64 -d /c/deps/vulkan-1.def -l /c/deps/libvulkan-1.a
   ```
3. Configure (Ninja: `pip install ninja`, or the one in Visual Studio's CMake component):
   ```bash
   L=C:/deps/llvm-mingw-20260922-ucrt-x86_64/bin; cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$L/x86_64-w64-mingw32-clang.exe -DCMAKE_CXX_COMPILER=$L/x86_64-w64-mingw32-clang++.exe -DCMAKE_RC_COMPILER=$L/x86_64-w64-mingw32-windres.exe -DCMAKE_EXE_LINKER_FLAGS=-static -DWWHD_BUNDLED_DEPS=ON -DVulkan_INCLUDE_DIR=C:/deps/Vulkan-Headers-1.4.309/include -DVulkan_LIBRARY=C:/deps/libvulkan-1.a
   ```
4. Build, then test:
   ```bash
   cmake --build build/mingw
   ```
   ```bash
   ctest --test-dir build/mingw --output-on-failure
   ```

`wwhd.exe`, `wwhd-vr-launcher.exe` and `openxr_loader.dll` land in `build/mingw` (static C++ runtime: only Windows
system DLLs, plus `SDL3.dll`).

The original project's other Windows paths (native LLVM with the MSVC ABI, MSYS2 CLANG64) are described in
[docs/UPSTREAM_README.md](docs/UPSTREAM_README.md#windows). The native LLVM path did not build the unmodified
runtime with Visual Studio 2022's clang 19 at the time of the VR work. MSYS2 should build VR the same way, but has
not been tested.

## Building on Linux

> The Linux VR build has **not been compiled or run** yet. The steps follow the original project's Linux build and
> [docs/VR_BUILD_LINUX.md](docs/VR_BUILD_LINUX.md), and need checking on a Linux machine.

**Required packages on Ubuntu 24.04:** the original project's list plus the OpenXR loader.

```bash
sudo apt install clang cmake ninja-build zlib1g-dev liblz4-dev libvulkan-dev glslang-dev mesa-vulkan-drivers libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxkbcommon-dev libwayland-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libopenxr-loader1 libopenxr-dev python3-pycryptodome
```

SDL3 is not packaged in Ubuntu 24.04: build it from source (https://github.com/libsdl-org/SDL, `release-3.2.x`).
Fedora's OpenXR loader is `openxr-libs`.

**Configure, build, test:**

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/linux
```

```bash
ctest --test-dir build/linux --output-on-failure
```

If linking fails with unwinder errors (missing `_Unwind_*` or `-lunwind`), add
`-DCMAKE_EXE_LINKER_FLAGS="--unwindlib=libgcc"` to the configure.

To build the OpenXR loader from the pinned SDK instead of the system's, add `-DWWHD_VR_BUILD_LOADER=ON`. It lands
next to the game as `libopenxr_loader.so.1`; point `LD_LIBRARY_PATH` there.

**Runtimes:**
- Monado: start `monado-service` first.
- WiVRn: start its server first.
- SteamVR: set it as the OpenXR runtime in its settings.
- Envision: installs and starts Monado or WiVRn profiles.

The loader finds the runtime through `XR_RUNTIME_JSON`, then `$XDG_CONFIG_HOME/openxr/1/active_runtime.json`, then
`/etc/xdg/openxr/1/active_runtime.json`.

## Arch Linux

The packages are the original project's Arch list plus the OpenXR loader, all from the official repositories. No AUR
package is needed.

```bash
sudo pacman -Syu
```

```bash
sudo pacman -S --needed base-devel git clang cmake ninja python python-pycryptodome sdl3 vulkan-headers vulkan-icd-loader glslang shaderc zlib lz4 openxr
```

Add the Vulkan driver for your GPU:
- `vulkan-radeon` (AMD);
- `vulkan-intel` (Intel);
- `nvidia-utils` (NVIDIA proprietary).

Optional:
- `zstd`, for the extractor's `.wua` support (otherwise CMake downloads the pinned source);
- `python-capstone`.

Then, with Ninja (the recommended generator, as in the original project):

```bash
git clone https://github.com/VR-WakerHD/VR-WakerHD.git ZeldaWWHD-VR && cd ZeldaWWHD-VR
```

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/linux
```

Prepare your game first (see above). The same untested status as the Linux section applies.

## Starting VR with the launcher

The launcher, `wwhd-vr-launcher` (Windows: `wwhd-vr-launcher.exe`, titled "Wind Waker HD VR"), sets the VR options
and starts the game, on the desktop or in VR. It is optional: `wwhd --vr` starts VR without it.

1. Start your OpenXR runtime and make it the active one (SteamVR: Settings > OpenXR > "Set SteamVR as OpenXR
   runtime").
2. Start `wwhd-vr-launcher` from the build folder, next to `wwhd`. It finds the game there.
3. In **General**:
   - **OpenXR** shows the active runtime. **Check for a headset** starts the runtime briefly and asks it for a headset.
   - **Game program** is filled in when the game is next to the launcher; otherwise enter the path of `wwhd(.exe)`.
   - **Game files (--game)**: see the next section.
4. Set what you like in the **Camera and comfort**, **Controllers** and **Graphics** tabs. Nothing is written until
   **Save** (or Play, which saves first).
5. Press **Play VR**, or **Play Flat**. **Start in VR (also without the launcher)** makes every start a
   VR start (`vr.enabled=1`).

The headset shows the game; the desktop window mirrors the left eye.

Without the launcher:

```bash
wwhd --vr --game <game folder>
```

Check the VR path without any game files. The headset shows a colour per eye for about 3 s and the controllers
vibrate once; `[vr smoke] PASS` means the loader, runtime, Vulkan binding, swapchains and frame loop work:

```bash
WWHD_VR=1 ./build/mingw/wwhd.exe --renderer-smoke
```

## Setting the game location in the launcher

The launcher needs the **extracted game folder**, not the disc image: the folder that holds `code`, `content` and
`meta` after [preparing your game](#prepare-your-game-all-platforms). In the **General** tab, enter it under **Game
files (--game)**. The launcher passes it to a source build as `--game <folder>`.

- It is kept between starts in `vr-launcher.ini` (`launcher.gameDir`), in the game's settings folder:
  `%APPDATA%\WWHD` on Windows, `~/.config/wwhd` on Linux. **Open the settings folder** opens it.
- To change it, edit the field and **Save**.
- **Not recognised:** if the folder is wrong or the game does not match the recompiled code, the game does not
  start, and the launcher says it ended right after starting. The game's log says why, for example
  `cannot load game/code/cking.rpx`. The game must be the same dump that `build/gen` was recompiled from.

## Controller mapping

The controllers become the Wii U GamePad's buttons and sticks, so every action works as the game decides. The
**sword hand** is the right controller and the **shield hand** the left; **Left-handed** (`vr.leftHanded=1`) swaps
them. Button defaults are from `runtime/src/vr/vr_bindings.h`; change them in the launcher's **Controllers** tab or
with `vr.bind.<action>=<button>`. The physical buttons can also be moved in the runtime's binding editor.

| Controller input | Default | In the game |
|---|---|---|
| Left stick | movement | forward is where you look (`vr.moveDirection=hand`: where the left controller points) |
| Right stick left / right | turning | smooth (the default) or snap (30° by default; `vr.turn`); aims while aiming the bow, hookshot or boomerang |
| Right stick down (held) | ZR | shield / crouch |
| Right stick click | | first / third person; cancels the item wheel |
| Both stick clicks held 0.6 s | | recentre |
| Sword-hand trigger | A | talk, open, pick up, climb |
| Sword-hand primary (A / Cross) | A | the same |
| Sword-hand secondary (B / Circle) | B | sword (also without a swing) |
| Sword-hand grip | R | the item on R; over the sword hand's shoulder: draw the sword |
| Sword-hand stick click | Minus | Minus |
| Shield-hand trigger | ZL | target lock |
| Shield-hand grip | (none) | closes Link's hand; over the shield hand's shoulder: opens the item wheel |
| Shield-hand primary (X / Square) | X | the item on X |
| Shield-hand secondary (Y / Triangle) | Y | the item on Y |
| Shield-hand stick click | LeftStickClick | |
| Menu button (left controller) | Plus | pause menu (on the GamePad panel) |
| Head | | the view: rotation and position |

The Valve Index has no menu button: bind "Menu" in SteamVR's binding editor. Full reference:
[docs/VR_CONTROL_MAPPING.md](docs/VR_CONTROL_MAPPING.md), [docs/VR_CONTROLS.md](docs/VR_CONTROLS.md).

## Motion controls and physical interactions

- **Head:** rotation and position drive the view at Link's eyes. Leaning is followed up to `vr.maxHeadOffset` (default
  0.45 m). Recentre with both stick clicks.
- **Hands:** each of Link's arms follows the controller on its side by IK, and the hand turns as the controller does.
  Through a tracking gap up to 2 s, the last tracked pose is held.
- **Body:** standing still in first person, Link turns to face where you look past 30° *(new)*. Walking, he faces
  where he walks; lock-on, attacks and events keep the game's own facing.
- **Sword and shield:** the sword at the sword hand, the shield on the shield hand *(the shield's orientation fix is
  new)*.
- **Shoulder reaches:** over the sword hand's shoulder (with the grip) draws the sword; over the shield hand's
  shoulder (with the grip held) opens the item wheel. The two shoulders are kept apart: neither gesture starts the
  other.
- **Wrist displays:** hearts and the item / B buttons on the left wrist, rupees on the right. Each shows while you
  look at it with its face turned to you.
- **Ladders** *(new)*: climbing down, the view turns to Link's facing; the head looks around freely.
- **Haptics:** sword hits vibrate (`vr.hapticStrength`), as do the item wheel's steps and choice.

## The physical sword

1. **Draw:** reach over the **sword hand's** shoulder (the right one; the left when left-handed) and **press** its
   grip. Pressing B also draws it, as in the game, with its draw slash. The draw's slash never hits.
2. **Hold:** the sword sits in Link's hand and points where the controller points. **Sword tilt** sets the blade's
   angle, and **Sword wrist straight** keeps the wrist in line with the forearm.
3. **Attack:** a **full swing** (down, left to right, or right to left) presses the game's attack. The game picks the
   cut, combos and damage, and the hit capsules follow your tracked blade. Thrusts, upward swings, wind-ups and wrist
   turns do not attack. Sensitivity is in the launcher's **Controllers > Swing sensitivity**.
4. **Physical sword** *(new)*: while the sword hand swings, the blade also cuts grass and hits enemies along its path
   without Link's attack animation (**Physical sword**, `vr.physicalSword`).
5. **Hits** vibrate the sword hand, once per target per swing.
6. **Sheathe:** as in the game, with A ("Put Away"). There is no sheathing gesture, because A also talks and rolls.
7. **While sheathed:** swings do nothing.

Third person plays as on the GamePad, with no swing detection.

## The item wheel

1. **Open:** put the **shield hand** (the left one; the right when left-handed) over its shoulder and **hold its
   grip**.
2. **Choose:** **right stick** left / right moves around the ring; up / down changes the page (Items, Spoils Bag,
   Bait Bag, Delivery Bag). Holding the stick repeats, and letting go keeps the choice.
3. **Take it out:** **pull the hand forward**. The item goes on **X** (`vr.itemWheelButton`: X, Y or R), and that
   button is pressed for you, so Link takes it out.
4. **Cancel:** let go of the grip at the shoulder, click the right stick, or lower the hand to your side.


Details: [docs/VR_ITEM_WHEEL.md](docs/VR_ITEM_WHEEL.md).

## Troubleshooting

The game writes `[vr]` lines to its log. Turn on **Graphics > Write a log file** in the settings overlay (F1), or
start with `WWHD_LOG_FILE=1`. The log is `captures/wwhd.log`, next to the game program.

| Problem | What to check |
|---|---|
| VR does not start | the log's `[vr]` line says why: no loader (`openxr_loader.dll` must be next to `wwhd.exe`), no active runtime, no headset, a runtime without Vulkan, the headset on another GPU, the session not starting within 15 s. Run the `--renderer-smoke` check above |
| Launcher: "Active runtime: none set" | install an OpenXR runtime and set it active |
| Hands drift or stop matching the controllers | recentre (both stick clicks). Since the latest changes the view re-anchors after events. The log's `[vr] tracking:` line (every 10 s) tells a runtime losing the controller (many *estimated* reads) from Link's body being away from you (*beyond reach*, *head ... from the view's anchor*) |
| The view faces away from Link | recentre; it also re-anchors when first person resumes after an event |
| The game does not start from the launcher | check **Game program** and **Game files (--game)**, then the game's log |
| `cannot load game/code/cking.rpx` | the game folder is wrong: it must hold `code`, `content` and `meta` |
| Build: `build/gen` missing | run the recompiler step above (or build against `build/gen-stub` for tests) |
| Build: Vulkan or OpenXR not found | Windows: see the Vulkan import library step; the OpenXR SDK is downloaded on the first configure (needs internet) |
| Low frame rate | the launcher's **Graphics** tab: **Frame interpolation for VR** on; the **GamePad window on the desktop too** off (it costs about 8 ms per frame). See [docs/VR_PERFORMANCE.md](docs/VR_PERFORMANCE.md) |

More: [docs/VR_BUILD_WINDOWS.md](docs/VR_BUILD_WINDOWS.md), [docs/VR_BUILD_LINUX.md](docs/VR_BUILD_LINUX.md),
[docs/VR_TESTING.md](docs/VR_TESTING.md).

## Development

- **Tests:** `ctest --test-dir <build folder> --output-on-failure`. The VR logic's unit tests are `vr_test`
  (`runtime/tools/vr_test.cpp`) and need no headset.
- **VR code:**
  - `runtime/src/vr/` (`vr_game.cpp`: the game side; `openxr_session.cpp`: OpenXR; the pure, tested logic in the
    `vr_*.h` headers);
  - `runtime/src/gfx/vulkan/vr_vulkan.cpp` (the renderer side);
  - `tools/vr_launcher/`.
- **Scripted test aids** need no headset. They are described in the docs they belong to, for example
  `WWHD_VR_TEST_WHEEL` and `WWHD_VR_TEST_BODY`.
- **Contributing:** this is a private repository. Access, issues and changes go through its owner; there is no public
  issue tracker or release channel for the VR version.

### Relationship to the original project

ZeldaWWHD-VR builds on [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) and keeps its full Git
history; the VR work is a series of commits on top of its `main` (v0.4.0).
- **Not a native fork:** GitHub does not allow a private fork of a public repository, so this is a standalone private
  repository rather than a GitHub-native fork.
- **Upstream remote:** the original is configured as `upstream`. Bring in its changes with:
  ```bash
  git fetch upstream
  ```
  ```bash
  git merge upstream/main
  ```
- **The original README** (macOS and Android builds, the full controls, mods, save states, shaders) is kept as
  [docs/UPSTREAM_README.md](docs/UPSTREAM_README.md).

## Credits

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

### ZeldaWWHD-VR

- **Original project:** [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp.git). ZeldaWWHD-VR is
  built on its native port, and its authors and contributors keep their contributions and the history that records
  them.
- **VR runtime:** the [Khronos OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) (Apache-2.0), downloaded at
  build time; the loader is built from it on Windows.

## License and legal notice

The code is licensed under the **Mozilla Public License 2.0** (see [`LICENSE`](LICENSE)), as in the original
project. The VR additions are under the same license. Vendored third-party code keeps its own license: Cemu
(MPL-2.0), metal-cpp (Apache-2.0), {fmt} (MIT), Dear ImGui (MIT) and ImGuizmo (MIT); see Credits. The extractor links
zstd (BSD-3-Clause), and the OpenXR SDK used by the VR build is Apache-2.0.

This is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo, or by the original
project's authors. "The Legend of Zelda", "The Wind Waker", "Wii U" and related names are trademarks of their
respective owners and are used here only to describe what this software is compatible with.

The repository contains **no game code, no game assets and no keys**: no executable, no recompiled or disassembled
game code, no textures, models, audio, shaders, screenshots or other material from the game, and no console
encryption keys. It contains only the tools and the runtime written for this project, plus the third-party code
listed under Credits. The game itself is Nintendo's property and is not included.

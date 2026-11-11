# VR mode: building on Windows

VR is part of the normal Vulkan build (`WWHD_VR=ON` by default). It needs nothing beyond the
[README's Windows build](../README.md#windows): CMake downloads the pinned Khronos OpenXR SDK
(release-1.1.63, Apache-2.0, SHA-256 checked) and builds its loader, `openxr_loader.dll`, which is copied
next to `wwhd.exe` (`WWHD_VR_BUILD_LOADER=ON` by default on Windows). The game opens the loader at run time,
so a build with VR still starts without it.

You also need your own game (README: extraction and recompilation into `build/gen`); placeholder code
(`tools/recomp/stubgen.py`) builds and runs the unit and renderer tests but not the game.

## llvm-mingw (the release toolchain; used to build and test VR)

The same steps as `.github/workflows/release.yml`'s Windows job, from Git Bash or MSYS2 (paths are examples):

```bash
curl -fsSL -o llvm-mingw.zip https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip
```

```bash
echo "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666  llvm-mingw.zip" | sha256sum -c - && unzip -q llvm-mingw.zip -d /c/deps
```

Vulkan headers and an import library (the game loads `vulkan-1.dll` from the driver at run time; the
import library is only for the tests that call Vulkan directly):

```bash
curl -fsSL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.309.tar.gz | tar -xz -C /c/deps
```

```bash
(echo "LIBRARY vulkan-1.dll"; echo EXPORTS; /c/deps/llvm-mingw-20260922-ucrt-x86_64/bin/llvm-readobj --coff-exports /c/Windows/System32/vulkan-1.dll | awk '/Name: vk/{print $2}') > /c/deps/vulkan-1.def
```

```bash
/c/deps/llvm-mingw-20260922-ucrt-x86_64/bin/llvm-dlltool -m i386:x86-64 -d /c/deps/vulkan-1.def -l /c/deps/libvulkan-1.a
```

Configure, build, test (Ninja: `pip install ninja`, or the one in Visual Studio's CMake component):

```bash
L=C:/deps/llvm-mingw-20260922-ucrt-x86_64/bin; cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=$L/x86_64-w64-mingw32-clang.exe -DCMAKE_CXX_COMPILER=$L/x86_64-w64-mingw32-clang++.exe -DCMAKE_RC_COMPILER=$L/x86_64-w64-mingw32-windres.exe -DCMAKE_EXE_LINKER_FLAGS=-static -DWWHD_BUNDLED_DEPS=ON -DVulkan_INCLUDE_DIR=C:/deps/Vulkan-Headers-1.4.309/include -DVulkan_LIBRARY=C:/deps/libvulkan-1.a
```

```bash
cmake --build build/mingw
```

```bash
ctest --test-dir build/mingw --output-on-failure
```

Add `-DGEN_DIR=$PWD/build/gen-stub` (after `python tools/recomp/stubgen.py build/gen-stub`) to build
without the game. `wwhd.exe` and `openxr_loader.dll` (static C++ runtime: only Windows system DLLs) land in
`build/mingw`.

The llvm-mingw loader build needs two adjustments the CMakeLists.txt makes itself: the loader's GNU version
script is replaced by its module-definition file, and the DLL is named `openxr_loader.dll` (not
`libopenxr_loader.dll`).

## MSYS2 CLANG64

As in the README, plus nothing: `cmake -S . -B build/windows-msys2 -G Ninja -DCMAKE_C_COMPILER=clang
-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release`. The loader is built from the pinned SDK the same
way (not tested on MSYS2 in this work). Copy `openxr_loader.dll` with the other DLLs when running outside
the CLANG64 shell.

## Native LLVM (MSVC ABI)

The README's native LLVM path did not build the unmodified runtime with Visual Studio 2022's clang 19 at
the time of this work (missing `<array>` include in `mods/story_notebook_font.h`, `M_PI` without
`_USE_MATH_DEFINES` in `aspect.cpp` and some tests); these are not VR changes. Use llvm-mingw.

## Options

| Option | Default | Effect |
|---|---|---|
| `WWHD_VR` | ON | VR mode (Vulkan builds on Windows and Linux) |
| `WWHD_VR_BUILD_LOADER` | ON on Windows | build `openxr_loader.dll` from the pinned SDK next to the game; OFF: provide one yourself (the runtime's or the Khronos SDK's) next to `wwhd.exe` |

## Running

1. Start the OpenXR runtime (SteamVR, Windows Mixed Reality, Meta Quest Link, Virtual Desktop's
   VDXR, Pimax...) and make it the **active OpenXR runtime** in its settings (it writes
   `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`).
2. `wwhd.exe --vr` (with your game in place, as for the desktop game).
3. The headset shows the game; the window mirrors the left eye.

Check the VR path without the game (no game files needed; the headset shows a colour per eye for about
3 seconds and the controllers vibrate once):

```bash
WWHD_VR=1 ./build/mingw/wwhd.exe --renderer-smoke
```

`[vr smoke] PASS` in the output means the loader, runtime, Vulkan binding, swapchains and frame loop work.

## Troubleshooting (`[vr]` lines in the log)

| Log says | Fix |
|---|---|
| the OpenXR loader (openxr_loader.dll next to the game) was not found | build with `WWHD_VR_BUILD_LOADER=ON`, or copy the Khronos loader next to `wwhd.exe` |
| no OpenXR runtime is installed or active | install one and set it active (SteamVR: Settings > OpenXR > "Set SteamVR as OpenXR runtime") |
| no headset is connected | connect and wake the headset; the runtime must see it |
| the OpenXR runtime does not support Vulkan | the runtime lacks `XR_KHR_vulkan_enable2`; update it |
| the headset is connected to a GPU the game cannot use | laptops: run the game on the GPU the headset is on (Windows graphics settings) |
| the session did not start within 15 s | the headset is asleep, or another VR application has focus |
| The VR session ended | the runtime quit or lost the headset: the game continues on the desktop; restart to return to VR |

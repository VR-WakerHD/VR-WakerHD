# Build VR-WakerHD from source (Windows, Linux, Arch Linux)

How to build **VR-WakerHD**, the VR version of *The Legend of Zelda: The Wind Waker HD* built on
[ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp), from its source code. Most players do not need
this: the [release downloads](https://github.com/VR-WakerHD/VR-WakerHD/releases) set the game up for you (see
[INSTALLATION.md](INSTALLATION.md)). Building from source is for developers and for platforms without a release.

You need your own, legally obtained copy of the Wii U game. Nothing from the game is included in this repository.

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
git clone https://github.com/VR-WakerHD/VR-WakerHD.git
```

```bash
cd VR-WakerHD
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
with **llvm-mingw**, the release toolchain. The steps are from [VR_BUILD_WINDOWS.md](VR_BUILD_WINDOWS.md),
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
[docs/UPSTREAM_README.md](UPSTREAM_README.md#windows). The native LLVM path did not build the unmodified
runtime with Visual Studio 2022's clang 19 at the time of the VR work. MSYS2 should build VR the same way, but has
not been tested.

## Building on Linux

> The Linux VR build is the one the developer plays on (Arch Linux / CachyOS, with SteamVR and Monado). The steps
> follow the original project's Linux build and [VR_BUILD_LINUX.md](VR_BUILD_LINUX.md); player releases are AppImages.

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
git clone https://github.com/VR-WakerHD/VR-WakerHD.git && cd VR-WakerHD
```

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/linux
```

Prepare your game first (see above). The same untested status as the Linux section applies.

See also: [INSTALLATION.md](INSTALLATION.md) · [COMPATIBILITY.md](COMPATIBILITY.md) · [FAQ.md](FAQ.md) ·
[back to the README](../README.md)

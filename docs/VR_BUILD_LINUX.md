# VR mode: building on Linux

VR is part of the normal Vulkan build (`WWHD_VR=ON` by default). Build as in the
[README's Linux section](../README.md); CMake downloads the pinned Khronos OpenXR SDK (release-1.1.63,
Apache-2.0, SHA-256 checked) for its headers. The game opens the distribution's OpenXR loader at run time
(`libopenxr_loader.so.1`, then `libopenxr_loader.so`), found through the normal library search path, so
there is no hard-coded library directory and a build with VR starts without a loader.

Status: the Linux build of VR has **not been compiled or run** in this work (it was built and tested on
Windows). The code is portable (no Windows-only calls outside `#ifdef _WIN32`, `dlopen` for the loader);
the commands below are the standard ones and need checking on a Linux machine.

For Steam Frame - Native (Linux arm64 on SteamOS arm64, with its own loader next to the game) see
[VR_STEAM_FRAME.md](VR_STEAM_FRAME.md).

## Packages

Ubuntu / Debian (22.04+ / 12+), in addition to the README's packages:

```bash
sudo apt install libopenxr-loader1 libopenxr-dev
```

Arch-based (Arch, CachyOS, Manjaro, SteamOS in developer mode):

```bash
sudo pacman -S openxr
```

Fedora:

```bash
sudo dnf install openxr-libs
```

Only the loader is needed at run time (the headers come from the pinned SDK at build time). To build the
loader from the SDK instead of using the distribution's, configure with `-DWWHD_VR_BUILD_LOADER=ON`; it is
placed next to the executable as `libopenxr_loader.so.1` (start the game with `LD_LIBRARY_PATH` pointing
there, or install it).

## Build

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/linux
```

```bash
ctest --test-dir build/linux --output-on-failure
```

The `vr` test needs no headset. Without the game: `python3 tools/recomp/stubgen.py build/gen-stub` and
`-DGEN_DIR=$PWD/build/gen-stub`.

## Runtimes

| Runtime | Notes |
|---|---|
| Monado (open source) | `monado-service`, then start the game; Monado writes the active runtime file |
| WiVRn (Quest and other standalone headsets over Wi-Fi) | built on Monado; start the WiVRn server first |
| SteamVR | set it as the OpenXR runtime in SteamVR's settings; the game does not require SteamVR otherwise |
| Envision | a front end that installs and starts Monado/WiVRn profiles |

The loader finds the runtime through `XR_RUNTIME_JSON`, then `$XDG_CONFIG_HOME/openxr/1/active_runtime.json`,
then `/etc/xdg/openxr/1/active_runtime.json`. Set `XR_RUNTIME_JSON=/path/to/runtime.json` to choose one.

The runtime must support `XR_KHR_vulkan_enable2` (Monado, WiVRn and SteamVR do). The headset must be on a GPU
with Vulkan 1.3 (or 1.1/1.2 with `VK_KHR_dynamic_rendering`), as for the desktop game. The window system does
not matter for VR (the headset images go through Vulkan); the desktop window still uses SDL3 on X11 or
Wayland.

## Running

```bash
./build/linux/wwhd --vr
```

Check VR without the game files (a colour per eye in the headset for about 3 s, one vibration):

```bash
WWHD_VR=1 ./build/linux/wwhd --renderer-smoke
```

## Troubleshooting (`[vr]` lines in the log)

| Log says | Fix |
|---|---|
| the OpenXR loader (libopenxr_loader.so.1 ...) was not found | install the package above (`ldconfig -p \| grep openxr` should list it) |
| no OpenXR runtime is installed or active | start Monado/WiVRn/SteamVR, or set `XR_RUNTIME_JSON` |
| no headset is connected | the runtime does not see the headset (`monado-cli probe`, SteamVR status window) |
| the OpenXR runtime does not support Vulkan | update the runtime |
| the headset is connected to a GPU the game cannot use | hybrid graphics: start the game on the GPU the runtime uses (`DRI_PRIME=1`, `prime-run`) |
| the session did not start within 15 s | the headset is asleep or another client has the session |

The AppImage does not contain an OpenXR loader; it uses the system's.

## Controllers seen twice (fixed)

SDL opens the PS VR2 Sense controllers (Sony 054C:0E45 and 0E46) on Linux as gamepads of their own, so the game read
them through OpenXR and again as GamePad sticks and buttons. One of their axes reaches the game fully deflected: in the
telescope, whose camera the game turns with the sticks, the view spun about once every 2 s on its own (traced with
`WWHD_VR_TRACE_AIM=1`: pitch input -1.0, Link's yaw turning 57 degrees a third of a second). The game no longer opens
them as gamepads on Linux (`input_sdl.cpp` `open_controller`, logged `[input] controller ignored: ... a VR controller,
read through OpenXR`); other controllers are unchanged. The `sdl_input` test, which failed while they were connected,
passes again.

# Wind Waker HD VR compatibility: headsets, OpenXR runtimes and platforms

This page lists the headsets, OpenXR runtimes and platforms that **VR-WakerHD**, the VR version of *The Legend of
Zelda: The Wind Waker HD*, runs on, and how well each has been tested. VR-WakerHD uses **OpenXR** with the **Vulkan**
renderer, so in principle any headset with an OpenXR runtime that supports Vulkan (`XR_KHR_vulkan_enable2`) works.

"Tested" below means played in a headset by the developer or testers. "Supported" means it should work through OpenXR
but has had little or no testing. Reports are welcome in
[Issues](https://github.com/VR-WakerHD/VR-WakerHD/issues).

## Platforms

| Platform | Release file | Status |
|---|---|---|
| Linux PC (x86-64) | AppImage | **Tested**: the developer's main platform (Arch Linux / CachyOS) |
| Windows PC (x86-64) | zip | **Tested** with SteamVR; released since 0.5.0 |
| Meta Quest 3 / 3S, standalone (Android) | APK | **Tested**. The headset's power is the limit: see [QUEST.md](QUEST.md) |
| Steam Frame (SteamOS, arm64) | AppImage | **Built**, with Quest-like tuning; not yet confirmed on the device ([VR_STEAM_FRAME.md](VR_STEAM_FRAME.md)) |
| macOS | — | No VR build: VR needs the Vulkan renderer. The base port runs flat-screen |

## OpenXR runtimes (PC VR)

| Runtime | Status | Notes |
|---|---|---|
| SteamVR (Windows) | **Tested** | Set it as the active OpenXR runtime |
| SteamVR (Linux) | **Tested** | Plays smoothly most of the time; occasional pacing dips from SteamVR on Linux are being worked on |
| Monado (Linux) | **Tested** | With a PS VR2 headset |
| Meta Quest Link (wired) | **Tested** | Quest 3 over a Link cable |
| Virtual Desktop (VDXR) | **Supported, experimental** | Turn on **Streaming optimal**; 90 Hz with Capped 45 fps first; a cable is steadier than Wi-Fi |
| Air Link | **Supported, experimental** | As Virtual Desktop |
| WiVRn (Linux) | Supported | Little testing |
| Windows Mixed Reality | Supported | Little testing |

## Headsets and controllers

| Headset | Status | Controller profile |
|---|---|---|
| PlayStation VR2 (PS VR2 Sense) | **Tested most** (SteamVR, Monado) | Touch-style; the launcher sets the PS VR2 binding in SteamVR |
| Meta Quest 3 / 3S (Touch Plus) | **Tested** (standalone, Link) | Own profiles and calibration for Quest 3 and 3S |
| Valve Index | Supported | The menu is on the trackpad press (the System button is SteamVR's) |
| HTC Vive | Supported | Vive controller profile |
| Windows Mixed Reality headsets | Supported | WMR controller profile |
| Other OpenXR headsets | Should work | Through the Khronos simple controller or Touch profile |

## PC requirements

- A GPU with a Vulkan driver: Vulkan 1.3, or 1.1 / 1.2 with `VK_KHR_dynamic_rendering`.
- PC VR performance depends on the scene and headset resolution. The render thread (CPU) usually sets the limit more
  than the GPU does. See [VR_PERFORMANCE.md](VR_PERFORMANCE.md).

## Game versions

- **USA** (`cking.rpx`, title `0005000010143500`): the version tested most in VR.
- **Europe**: supported by the base port (title `0005000010143600`).

See also: [INSTALLATION.md](INSTALLATION.md) · [QUEST.md](QUEST.md) · [FAQ.md](FAQ.md) ·
[back to the README](../README.md)

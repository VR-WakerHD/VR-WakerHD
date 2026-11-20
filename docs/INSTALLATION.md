# How to install Wind Waker HD VR (VR-WakerHD) on PC VR, Linux and Quest 3

This page explains how to install **VR-WakerHD**, the VR version of *The Legend of Zelda: The Wind Waker HD*, from a
release download on Windows, Linux, SteamOS and Meta Quest 3 / 3S. You need your own copy of the Wii U game. Setup turns
it into a playable VR game on your own device; nothing from the game is in the download.

To build from source instead, see [BUILDING.md](BUILDING.md).

## What you need

- **Your own copy of the Wii U game**, dumped from your own disc and console, in one of these forms:
  - a `.wud` or `.wux` disc image, with its disc key (`.key`) and the Wii U common key;
  - a Cemu `.wua` archive;
  - an already extracted game folder (holding `code`, `content` and `meta`).
- **For PC VR:** a Vulkan-capable GPU and an OpenXR runtime set as the active one (SteamVR, Meta Quest Link, Virtual
  Desktop, Windows Mixed Reality, or Monado / WiVRn on Linux).
- **For Quest standalone:** a Meta Quest 3 or 3S in developer mode, to install an APK.

The USA version of the game is the one tested most in VR; the European version is supported by the base port.

## 1. Download

Get the file for your platform from the [latest release](https://github.com/VR-WakerHD/VR-WakerHD/releases/latest):

| Platform | File |
|---|---|
| Windows PC (x86-64) | `VR-WakerHD-Windows-<version>-x86_64.zip` |
| Linux PC (x86-64, glibc 2.35+: Arch/CachyOS, Fedora 36+, Ubuntu 22.04+, Debian 12+) | `VR-WakerHD-Linux-<version>-x86_64.AppImage` |
| Steam Frame (SteamOS, arm64) | `VR-WakerHD-SteamOS-aarch64-<version>.AppImage` |
| Meta Quest 3 / 3S (standalone) | `VR-WakerHD-Quest3-<version>.apk` |
| What to install on Linux, per distribution | `required_packages.txt` |

## 2. Start it

- **Windows:** unzip it into a folder of its own, for example `C:\Games\VR-WakerHD`, and start
  `wind-waker-hd-vr.exe`.
- **Linux / SteamOS:** make the AppImage executable (`chmod +x VR-WakerHD-*.AppImage`) and run it. If it does not start,
  install the packages listed in `required_packages.txt` for your distribution.
- **Quest 3 / 3S:** install the APK with SideQuest or `adb install`, then open it from your library's *Unknown
  sources*.

## 3. Set up the game (once)

The installer opens on the first start. Choose your game files: the extracted folder or the disc image. Setup extracts
the game if needed and **compiles it for your device**. That takes a few minutes on a PC and longer on a headset. On
Quest, the app offers to extract the game disc when the game files are missing.

What setup makes (the extracted files, the recompiled game code, shader caches) stays on your device. It must not be
shared.

## 4. Play

When setup finishes, the **VR launcher** opens. Every later start goes straight to it.

1. On PC, start your OpenXR runtime first and make it the active one. In SteamVR that's Settings > OpenXR > "Set SteamVR
   as OpenXR runtime".
2. In the launcher, check the headset, refresh rate, controls and graphics.
3. Press **Play VR**, or **Play Flat** to play on the monitor.

In the headset, click the **left stick** to open the settings menu. How to play: [PLAYING.md](PLAYING.md).

## If something goes wrong

See [FAQ.md](FAQ.md#troubleshooting). For a bug report, attach your log file. The
[bug report form](https://github.com/VR-WakerHD/VR-WakerHD/issues/new/choose) shows where to find it on each platform.

See also: [COMPATIBILITY.md](COMPATIBILITY.md) · [QUEST.md](QUEST.md) · [back to the README](../README.md)

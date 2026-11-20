# Wind Waker HD VR FAQ and troubleshooting

Common questions about **VR-WakerHD**, the VR version of *The Legend of Zelda: The Wind Waker HD* built on the
ZeldaWWHDRecomp native port, and fixes for common problems.

## About the project

**What is VR-WakerHD?**
A fan-made VR port of *The Legend of Zelda: The Wind Waker HD*. You play in first person through Link's eyes, with head
tracking, motion controls and a sword you swing yourself, on PC VR and on Meta Quest 3 / 3S. Earlier copies of this
project were called ZeldaWWHD-VR.

**Is it official?**
No. It is not made, endorsed or sponsored by Nintendo, or by the ZeldaWWHDRecomp authors.

**How is it related to ZeldaWWHDRecomp?**
[ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp) is the open-source native port: it recompiles the
Wii U game's code so it runs natively, flat-screen. VR-WakerHD builds on it and adds the VR mode: the OpenXR session,
the first-person body and arms, the motion controls, the VR launcher, the Quest build and the VR rendering work.

**Is it an emulator, or a VR injector like VorpX?**
Neither. The game's code is recompiled to native code, and the VR is built into the renderer and the game's own logic.
That's how it can show Link's arms following your controllers and the HUD on his wrists.

**Does it include the game?**
No. You need your own, legally obtained copy of the Wii U game. Setup builds it on your device from your own files, and
what it makes must not be shared.

**Is it free and open source?**
Yes. The VR mode is GPL-3.0-or-later with a credit term, and the original port is MPL-2.0. See
[LICENSE-VR.md](../LICENSE-VR.md).

## Hardware

**Which headsets work?** Any headset with an OpenXR runtime that supports Vulkan. PS VR2 and Quest 3 / 3S are tested
most. See [COMPATIBILITY.md](COMPATIBILITY.md).

**Does it run on Quest without a PC?** Yes, standalone on Quest 3 and 3S. See [QUEST.md](QUEST.md). Quest 2 and Pro
are not tested.

**Does it work on Linux?** Yes: the Linux AppImage, with SteamVR, Monado or WiVRn.

**Can I play without a headset?** Yes. **Play Flat** in the launcher plays the base port on the monitor.

## Playing

**How do I open the settings in the headset?** Click the left stick. The grips switch tabs, and the left stick moves
between options.

**How do I use the sword?** Reach over your right shoulder, press the grip to draw it, and swing it fully to attack.
See [PLAYING.md](PLAYING.md#the-physical-sword).

**How do I switch between first and third person?** Click the right stick.

**The view is off-centre or faces the wrong way.** Recentre with both stick clicks.

## Troubleshooting

The game writes a log on every start (`wwhd.log`; the previous start's is `wwhd-previous.log`). Where it is:

| Where you play | Log file |
|---|---|
| Quest 3 / 3S | `/sdcard/Android/data/org.wwhdrecomp.wwhd/files/captures/wwhd.log` |
| Linux / SteamOS (AppImage) | `~/.local/share/wwhd/captures/wwhd.log` |
| Windows (release zip) | `data\captures\wwhd.log` in the folder you unzipped |
| Built from source | `captures/wwhd.log` in the folder you start the game from |

| Problem | What to check |
|---|---|
| VR does not start | An OpenXR runtime must be installed and set as the active one. The log's `[vr]` lines say why: no loader, no active runtime, no headset, a runtime without Vulkan, or the headset on another GPU |
| Launcher: "Active runtime: none set" | Install an OpenXR runtime and set it active (SteamVR: Settings > OpenXR) |
| Low frame rate | Lower the headset resolution and field of view. On Quest, see [QUEST.md](QUEST.md). On PC, turn off the GamePad window on the desktop |
| Stutter on the first visit to an area | Shaders are being made; it eases as you play. *Shader compile: Fast* (Graphics) trades it for objects briefly missing |
| Wireless (Virtual Desktop, Air Link) lags or stutters | Turn on **Streaming optimal**, try 90 Hz with Capped 45 fps, or use a cable |
| Hands drift or stop matching the controllers | Recentre (both stick clicks). The log's `[vr] tracking:` line, every 10 s, shows tracking losses |
| `cannot load game/code/cking.rpx` | The game folder must hold `code`, `content` and `meta` |

Still stuck? Open a [bug report](https://github.com/VR-WakerHD/VR-WakerHD/issues/new/choose) and attach your log.

See also: [INSTALLATION.md](INSTALLATION.md) · [COMPATIBILITY.md](COMPATIBILITY.md) · [back to the README](../README.md)

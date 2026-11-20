# Sharing VR-WakerHD: descriptions and announcement text

Ready-to-use text for telling people about **VR-WakerHD**, for the maintainer to post by hand where it fits each
community's rules. Post only where self-promotion of open-source projects is allowed, answer questions, and don't
repost the same text across many places.

## One-line description

> VR-WakerHD: an unofficial, open-source VR version of The Legend of Zelda: The Wind Waker HD. First person at Link's
> eyes, motion controls and a physical sword, on PC VR and standalone Quest 3. Built on ZeldaWWHDRecomp; bring your
> own copy of the game.

## Short description (about 300 characters)

> VR-WakerHD turns The Wind Waker HD into a real VR game. You play through Link's eyes, his arms follow your
> controllers, you swing the sword yourself, and the HUD is on his wrists. It runs through OpenXR on PC VR
> (Windows/Linux) and standalone on Quest 3 / 3S. Free and open source; you need your own copy of the Wii U game.

## Announcement post (Reddit, forums)

**Title:** VR-WakerHD: The Wind Waker HD in VR with full motion controls (PC VR and standalone Quest 3, open source)

> I've been building a VR version of *The Legend of Zelda: The Wind Waker HD* on top of
> [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp), the native PC port of the game.
>
> - First person at Link's eyes, with 6DoF head tracking (third person is a stick click away)
> - Link's arms follow your controllers, and you draw and swing the sword yourself
> - Bow, Skull Hammer and the Wind Waker baton with motion controls; an item wheel at your shoulder; the HUD on
>   Link's wrists
> - OpenXR on Windows and Linux (SteamVR, Quest Link, Virtual Desktop, Monado), and a standalone Quest 3 / 3S build
>
> It's unofficial and includes no game: setup builds the game on your device from your own Wii U copy.
>
> Tutorial and gameplay: https://youtu.be/a98maOC3x24
> Downloads and source: https://github.com/VR-WakerHD/VR-WakerHD
>
> It's experimental, so bug reports (with your log file) are very welcome.

## Where people look for projects like this

| Place | Why it fits | Notes |
|---|---|---|
| r/virtualreality, r/SteamVR | PC VR players | Check the rules on self-promotion and flair |
| r/OculusQuest | Quest players | Main Quest communities only; never piracy communities (the project needs the player's own copy) |
| r/linux_gaming | The Linux and Monado support is uncommon | Mention Monado and the AppImage |
| r/zelda, r/Wiiu | Wind Waker fans | Video posts do better than text |
| r/FlatScreenToVR | Flat games made VR | A natural fit |
| Flat2VR Discord | The main community for flat-to-VR mods | Follow its showcase channel rules |
| The ZeldaWWHDRecomp community | Upstream users | Ask the maintainers first; keep it clearly separate from their project |
| YouTube | Gameplay discovery | Link the repository in each video's description, with "Wind Waker HD VR" in the title |
| GitHub topic pages | `wind-waker`, `openxr`, `virtual-reality` | Comes from the repository's topics |

## References worth asking for

These are real places where people already look for this project. Each text is ready for the maintainer to send.
Nothing here is sent automatically.

### 1. The upstream project (ZeldaWWHDRecomp)

Its README already credits community forks (the Android ones), so a short "VR" line fits there. Ask in an issue or on
its Discord first, rather than opening a pull request unasked:

> **Title:** A link to VR-WakerHD, the VR version built on this port?
>
> Hi! VR-WakerHD (https://github.com/VR-WakerHD/VR-WakerHD) is a VR version of The Wind Waker HD built on this port:
> first person at Link's eyes, motion controls, OpenXR on PC and a standalone Quest 3 build. It keeps this
> repository's history, credits it in its README, and its draw batching was contributed back here (#606). People
> searching for "Wind Waker VR" often end up on this repository. Would you be open to a one-line link in the README's
> community or forks section, for example: "**VR:** [VR-WakerHD](https://github.com/VR-WakerHD/VR-WakerHD), a VR
> version of the port (PC VR and Quest 3)". Happy to open the pull request if that's welcome.

### 2. PCVR-Mods-Installer-Hub, issue #159

[Issue #159, "Mod suggestion: Zelda Windwaker"](https://github.com/Mr-Nlce/PCVR-Mods-Installer-Hub/issues/159) asks
for a Wind Waker VR mod and links only the flat-screen port. A reply:

> There's a VR version of that port now: VR-WakerHD (https://github.com/VR-WakerHD/VR-WakerHD), first person with
> motion controls, through OpenXR (SteamVR, Quest Link, Virtual Desktop). It's open source; the player needs their own
> copy of the Wii U game, and setup builds it on their PC. Releases:
> https://github.com/VR-WakerHD/VR-WakerHD/releases

### 3. VR news sites

Sites that cover fan VR mods and ports (they have covered BetterVR, Mario Kart 64 VR and WinlatorXR): vr.org,
UploadVR, Road to VR, mixed-news.com. The sites that covered the upstream port (DSOGaming, GamingBible) may cover a
VR version too. Send a short tip with the video:

> **Subject:** Tip: The Wind Waker HD playable in VR, open source (PC VR and standalone Quest 3)
>
> VR-WakerHD is an open-source VR version of The Legend of Zelda: The Wind Waker HD, built on the ZeldaWWHDRecomp
> native port. You play in first person through Link's eyes, his arms follow your controllers, and you swing the sword
> yourself. It runs through OpenXR on Windows and Linux, and standalone on Quest 3 / 3S. It's unofficial and includes no
> game: players use their own Wii U copy.
>
> Video: https://youtu.be/a98maOC3x24 · Project: https://github.com/VR-WakerHD/VR-WakerHD

### 4. Directories and communities

- **Flat2VR** (Discord) and **r/FlatScreenToVR**: the main places for flat-to-VR projects.
- **SideQuest**: a listing for the Quest APK can be considered. Check that its rules allow an app that needs the
  player's own game files.

Track what was sent, and any listing that appears, in the outside references table of
[SEARCH-DISCOVERABILITY.md](SEARCH-DISCOVERABILITY.md).

## Video titles and descriptions

Put the name people search for first, for example "The Legend of Zelda: Wind Waker HD VR — <what the video shows>".
Each description should start with one line about the project, followed by the repository link
(https://github.com/VR-WakerHD/VR-WakerHD) and the note that no game is included.

See also: [back to the README](../README.md)

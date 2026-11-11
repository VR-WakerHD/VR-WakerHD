# VR launcher

`wwhd-vr-launcher` (Windows: built as `wwhd-vr-launcher.exe`, described as "Wind Waker HD VR") sets the VR options
before the game starts and starts it, on the desktop or in VR. It is optional: the game starts without it
(`wwhd --vr`, or the release's program), and closing the launcher never ends a game it started.

Settings reference: [VR_SETTINGS.md](VR_SETTINGS.md). Controls: [VR_CONTROL_MAPPING.md](VR_CONTROL_MAPPING.md). Menu
panel: [VR_PAUSE_MENU.md](VR_PAUSE_MENU.md).

## What it does

| Tab | Contents |
|---|---|
| General | Play VR / Play Flat; "Start in VR" (also without the launcher); the active OpenXR runtime (read from its manifest, nothing started); "Check for a headset" (starts the runtime briefly and asks for a headset); the game program and game folder; open the settings folder or this documentation |
| Install game | extract the game from your own disc image (.wud, .wux) or Cemu archive (.wua) to a folder and set that folder as the game's (below) |
| Camera and comfort | the menu panel (when, placement, distance, height, size, curvature); title / file select and cutscene distance; the player camera (view at the start: first or third person, HUD position and HUD size, height offset, view forward / back, world scale (matched to Link's arms or set directly: the slider always shows the scale in use, and moving it sets it), lean limit, Link's head, hearts on the left arm: size, look angle, arm distance); comfort (turning, movement direction and speed, the view turning in spin attacks) |
| Controllers | left-handed, stick dead zone, the button bindings, what is fixed and what is not supported yet, swing thresholds, sword and arm tracking |
| Graphics | headset refresh rate (enabled only when the runtime lets applications choose it), headset image size, frame interpolation for VR, the game's internal resolution, FXAA, anisotropic filtering, the performance overlay, **Write a log file** (`logFile`, on by default, the same setting as F1 > Graphics: the game writes `captures/wwhd.log`; a change applies from its next start) with **Open the log folder** (the game's `captures` folder: its release's data folder when it has one, else the game program's folder), NVIDIA DLSS (mode and DLSS 5 Neural Rendering: only what the game's last check found usable is enabled; **Detect PC** runs that check now, `wwhd --dlss-probe`, with no window. DLSS 5 can be turned on when the driver offers it or when a copy of `nvngx_dlssnr.dll` of the player's own, modified or not, is next to the game: nothing here checks its signature, and the game loads NGX's core from its own folder first. [VR_DLSS.md](VR_DLSS.md)) |

Three states are kept apart: **runtime** (an OpenXR runtime is installed and active), **headset** (the runtime
reports one when asked) and **VR session** (only the game runs one; its `[vr]` log lines and settings overlay say so).

Every section has "Restore this section's defaults"; "Restore all VR defaults..." asks for confirmation. Nothing is
written until Save (or Play, which saves first). Unusable values found in the file are listed and kept at their
defaults. A failed save says why (and nothing is half-written).

## The settings file

The launcher edits the game's own `settings.ini` (locations: [VR_SETTINGS.md](VR_SETTINGS.md)) through
`runtime/src/vr/vr_settings_file.h`, with the same definitions as the game (`vr/vr_config.h`, `vr/vr_bindings.h`). It
changes only `vr.*` keys and the game graphics keys it shows (including `dlss` and `dlss5`); other settings, comments and unknown lines are
kept in place. It does not save while a game it started runs (the game rewrites the file itself); changes apply at
the game's next start.

Its own state (`launcher.gameExe`, `launcher.gameDir`, `launcher.format`) is in `vr-launcher.ini` in the same folder.

## Starting the game

The program started is, in order: the configured "Game program"; next to the launcher, `wwhd(.exe)` (a source build)
or the release's `Wind Waker HD.exe` / `wind-waker-hd` (it starts the prepared game and passes `--vr` / `--no-vr` on).
Arguments go as a list, never through a shell, so paths with spaces are safe; the working folder is the program's
folder; a source build gets `--game <folder>` when a game folder is set. "Play Flat" also passes
`--no-stereo`: the plain desktop picture for that start, not the desktop 3D output (`stereoMode`) a setting may
have saved; the setting is kept (F1 can turn 3D on again). Only one game is started at a time from the
launcher; when it ends with an error right after starting, the launcher says so (see the game's log). No
administrator rights are needed.

## Install game

For a game built from source (`wwhd`), which needs the game's files extracted to a folder. (A release's own program
does this in its first start instead.) The steps:

1. **Game source, Browse...**: a Wii U disc image (`.wud`, `.wux`) or a Cemu archive (`.wua`). The file's content
   decides, not its extension (an archive is recognized by its footer). A disc image also needs two keys from your
   own console: its disc key (by default its own name with `.key`, next to it) and the Wii U common key (by default
   `common.key` next to the image, or `WIIU_COMMON_KEY`). "Disc key file" and "Wii U common key file" choose other
   files. No keys are included, and nothing is downloaded.
2. **Detected game**: the title ID from the source (`wwhd-extract info`; kept as text, leading zeros included), the
   game's name from its own `meta/meta.xml` (`longname_en`; only that file is extracted, to a temporary folder,
   removed after), its size and whether it is a disc image or an archive. A name that cannot be read shows as "Unknown
   title". A title ID that is not The Wind Waker HD's, or the Japanese version (which this port is not built for), is
   pointed out. In an archive, the game itself is chosen (title type 00050000), never its update or DLC; an archive
   with only an update says so.
3. **Extract to, Browse...**: a folder (by default a new one next to the source, "<name> (extracted)"). It says
   whether the folder is new, empty, holds other files (then extracting asks first: files with the game's names are
   replaced, nothing else is deleted) or already holds this game ("Use the game already there" then skips the
   extraction), and the free space, or that it could not be determined. Not the launcher's own folder; a folder that
   cannot be written is refused with that reason.
4. **Configure ZeldaWWHD-VR to use this game** (on by default).
5. **Extract Game**: `wwhd-extract --progress extract` in the background (the window stays usable); the bar shows the
   bytes written of the total (an archive is checked against its SHA-256 first: "Checking the archive"). **Cancel**
   stops it: the result says cancelled, never done, and the settings are not changed. After a cancel or a failure,
   files may remain; "Remove the partly extracted files..." (only for a folder that was empty or new before, and only
   after a confirmation) deletes that folder.

**After the extraction** the tab lists what was checked, and only that:

- *Extraction completed* (the extractor's exit code 0);
- *the game's files are all there*: `code/cking.rpx` (the file the game loads first) not empty, `content`, and
  `meta/meta.xml` with the source's title ID;
- *the game program was built from this same version*: it asks the configured game program (`wwhd --guest-build`,
  which prints `build USA 0005000010143500` and exits). A game folder of another version would be refused at start, so
  the settings are then not changed. When the program cannot say (the release's own program, an older `wwhd`), the
  tab says this was not checked;
- *set as the game's folder*: `launcher.gameDir` in `vr-launcher.ini` (the launcher starts `wwhd --game <folder>`;
  `settings.ini` holds no game folder). The file is copied first to `vr-launcher.ini.bak` (then `.bak2`, `.bak3`...;
  never overwritten), changed in place (every other line kept), written through a temporary file renamed over it,
  and read back. Not done while a game started from the launcher runs.

The game is not started by the tab: "ready to start from the General tab" means the checks above passed. Errors are
in plain words with the extractor's own message as details (a missing or wrong key, a damaged file, a folder that
cannot be written, ...).

The code: `tools/vr_launcher/game_install.h` (the decisions), `extract_runner.h` (wwhd-extract as a process: its
arguments as a list, never a shell; its output read on a worker thread; cancel kills it), the tab in
`vr_launcher.cpp` (`tab_install`). The extractor's command line is unchanged; it is found next to the launcher, else
next to the game program. File and folder pickers are SDL's (Windows: the system dialogs; Linux: the desktop portal,
else zenity or kdialog).

**Tests** (`launcher_install`, `runtime/tools/launcher_install_test.cpp`; no game data): the extractor's own
synthetic `.wud`, `.wux` (made-up keys) and `.wua` (game, update and DLC) in a folder with spaces and non-ASCII
letters, through the same runner: info, the meta-only step, full extractions with progress, a missing key, a damaged
image, an unsupported file, a broken archive, a missing program, a cancel (Linux: a stand-in that never ends); the
destination checks (new, unrelated files, an existing game, not writable, a file, the launcher's folder), the game
check, the settings update (new file, every other line kept, backups, a failed write, an unwritable folder, Windows
drive and UNC paths kept as written).

**Captures** (test aids, no input needed): `WWHD_LAUNCHER_TAB=install` opens on the tab; `WWHD_LAUNCHER_SOURCE` and
`WWHD_LAUNCHER_DEST` preset the file and the folder, `WWHD_LAUNCHER_EXTRACT=1` then extracts; `WWHD_LAUNCHER_SHOT=x.bmp`
saves the window once nothing runs, and quits. With `WWHD_USER_DIR=<folder>` the settings files are that folder's,
not yours. Checked on Linux with them: the synthetic archive in a folder with non-ASCII letters, detected, extracted,
checked against the game program (`--guest-build`), `vr-launcher.ini` updated with its comment and other keys kept,
`.bak` then `.bak2`; and a disc image with no common key: the reason, the key pickers, Extract disabled.

**Windows**: written for it (the same code: SDL's processes and dialogs, `std::filesystem` paths from UTF-8, the
game's `host::replace_file`), but not yet built or run on Windows. To check there: the pickers, a long extraction's
progress and cancel, paths with non-ASCII letters, and that no empty console window stays open while `wwhd-extract`
runs (it closes the console Windows gives it when it is started with its output piped).

## Building

Built with the game when VR is on (`WWHD_VR=ON`, `WWHD_VR_LAUNCHER=ON`, both default) on Windows and Linux; it uses
what the game's build already has (SDL3, Dear ImGui, the OpenXR headers):

```bash
cmake --build build/mingw --target wwhd-vr-launcher
```

On Windows it lands next to `wwhd.exe` with `SDL3.dll` and `openxr_loader.dll`; on Linux next to `wwhd` (SDL3 and the
system's OpenXR loader as for the game). Start it from there; it finds the game in the same folder. In a release
folder, put it next to `Wind Waker HD.exe` (or set the game program in the General tab).

## Running without the launcher

```bash
wwhd --vr --game <game folder>
```

or the release's program with `--vr`. `vr.enabled=1` in `settings.ini` (the launcher's "Start in VR") starts every
game in VR.

## Troubleshooting

| Shown | Meaning |
|---|---|
| Active runtime: none set | install an OpenXR runtime and make it active (SteamVR: Settings > OpenXR) |
| No headset: the runtime sees no headset | connect and wake the headset, then check again |
| No headset: the OpenXR loader was not found | `openxr_loader.dll` must be next to the launcher (Windows); Linux: install the OpenXR loader package |
| The game was not found | set the game program in General > Game |
| The game ended with exit code N right after starting | see the game's log (`wwhd.log` / `data/captures`) |
| The game is running and rewrites its settings file | close the game before saving, or change the settings in the game (F1) |

## Settings from an older build (2026-11-04)

The game keeps every key it reads from `settings.ini` and writes them all back, so a default changed in a newer build
never reaches a player whose file already holds the old value (a player's dialogue issue was reported from settings
kept since an older build). The launcher records the build that last saved the file through it (`settingsBuild`, the
build's version, `build_info.h`). When it opens and the file exists without that key, or with another build's, it asks
once: **Start fresh (recommended)** renames the file to `settings.ini.bak-<date>-<time>` (kept, not deleted) and writes
a new one with every VR setting at this build's default (the game's other settings left to their defaults);
**Keep my settings** records this build in the file, so it is not asked again until the next build. Any save in the
launcher records the build too. Not offered while the game runs (it rewrites the file).

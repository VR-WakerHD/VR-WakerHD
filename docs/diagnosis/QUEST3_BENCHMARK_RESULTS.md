# Quest 3 benchmark results (2026-11-03)

**No Quest 3 benchmark was run for this report** (no headset here; this PC cannot build the Android target). What is
below is (1) the measurements already recorded from earlier Quest 3 runs, (2) the method for the next runs, and (3)
empty result tables to fill in. No improvement is claimed.

## 1. Recorded history (not a controlled baseline)

| Source | Scene | Refresh | Result | Notes |
|---|---|---|---|---|
| Quest 3 log, runtime 209 (early) | unknown | — | ~24 fps, 23.7 logic steps/s; frames 33-65 ms | before the record worker and the window skip |
| v0.1+574, runtime 207 | unknown | 72 / 90 Hz | median 24.6 fps at 72 Hz, 30 at 90; GPU 50-60 % busy | CPU-bound |
| user logs, 2026-10-31 | gameplay | 90 Hz | 22-29 fps; game passes 28-44 ms | game thread CPU |
| Downloads\wwhd.log, 2026-11-02 | gameplay | 72/90/108/144 Hz | ~30 fps everywhere; "released + xrEndFrame" p50 13.4-16.9 ms; atlas lock waits 1.5-3 ms/frame | before the end-frame thread default |

## 2. Method

- Build: the Quest APK of the commit under test (`VR=1 android/build_native.sh`, `assembleVrRelease`); note the commit.
- Settings: as saved, plus one env.txt line per A/B only; log file on (`captures/wwhd.log`); back up logs before each run.
- Each scene 3 minutes after loading; the game's `[vr]` lines every 10 s give: frames/s to the headset, headset Hz,
  xrWaitFrame p50/p99, render thread p50/p99, interval p50/p95/p99/max, missed periods, uneven frames, submission
  times, end-frame waits, memory (new), dynres status.
- Meta tools: OVR Metrics Tool (CPU/GPU levels, utilisation, stale frames, temperature), `adb logcat` (VrApi lines),
  Perfetto for thread scheduling (checks 1.1's effect), Android GPU Inspector where it supports the Quest.
- Scenes: 1 Outset Island (start, house to beach); 2 open ocean sailing at full wind; 3 Windfall plaza (many
  characters); 4 a combat room (Forsaken Fortress or Dragon Roost); 5 particle-heavy fight; 6 water and transparency
  (Outset's shore at sunset); 7 wrist UI looked at; 8 sword swings and items; 9 fast head turns; 10 a loading door.
- Record per run: avg fps, % frames on time, interval p99, missed periods, memory peak, temperature after 3 min.

## 3. Results (to fill in)

| Run | Build / change | Scene | fps avg | % on time | interval p99 | render p99 | memory peak | Notes |
|---|---|---|---|---|---|---|---|---|
| Q0 | baseline (this commit) | 1-10 | | | | | | |
| Q1 | Q0 + `XR_KHR_android_thread_settings` off (to A/B 1.1: needs a switch or an older build) | 1, 3 | | | | | | |
| Q2 | ThinLTO + `-mcpu` | 1, 3 | | | | | | |
| Q3 | frame synthesis off | 1, 2 | | | | | | |

## 4. Validation status of this session's changes

| Change | Quest 3 hardware | Desktop | Compile / static |
|---|---|---|---|
| Thread declaration (`XR_KHR_android_thread_settings`) | no | no-op on Windows | Windows build passes; Android code syntax-checked alone |
| Memory log | no | not compiled on Windows | parsing code compiled alone |
| Earlier this session (shared code): UI layer on re-shown frames, cropped-eye motion scale, standalone dynres cap, faster dynres at half rate | no | builds | unit checks compiled, not run |

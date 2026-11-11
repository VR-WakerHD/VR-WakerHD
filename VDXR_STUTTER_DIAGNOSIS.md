# VDXR stutter diagnosis (2026-11-03)

**Symptom.** Quest 3 players on Virtual Desktop's VDXR over Wi-Fi: "skips frames when looking around, jittery". A
PS VR2 on SteamVR (wired) with the same build: no skipped frames.

**Status.** Two game-side causes found by code analysis and the players' logs, fixed for streamed headsets only
(`7102cf4c`). Not yet validated on a Quest 3: no Quest or Virtual Desktop is on the development machine. The
network's share is not measured: the game cannot read Virtual Desktop's encode, network and decode counters.

## Root cause (game side)

The game read the streamed runtime's network jitter as its own lateness, and reacted in two ways that each add a
visible hitch:
1. **It moved its frame timing** half a refresh whenever a few frames looked late (`vr_vulkan.cpp` `note_display`),
   at most every 5 s.
2. **Auto frame rate kept switching** between full and half rate (90 and 45), trying full rate again every 10 s.

Neither happens on a wired PS VR2, whose display times are steady: that is why the PS VR2 was clean.

## Evidence (player logs, Quest 3 through VDXR)

| Log line | Value | Meaning |
|---|---|---|
| `Quest tracking (10 s): ... display times up to N ms apart` | 24.9 to 155.6 ms | the runtime's predicted display times jump with the network |
| `frames near the headset's deadline: moved half a refresh later` | several in one session | the frame-timing shift fired (a hitch each) |
| `VR frame rate auto: capped at 45 ... / capped at 90 ...` | repeated switches | Auto flapping |
| `Quest tracking: left hand 155 tracked, 310 untracked (the runtime's estimate)` | at times | controller poses estimated by the runtime (tracking or network) |
| `frames shown again while the game had none ready` | 0 to 1 per 10 s | not the game missing frames |
| `head tracking jumped 1.62 m in 47 ms` | once at start | a runtime pose glitch, held by the game's guard |

The game's own frames held their rate in that log (45 fps target, about 43 to 46 drawn), so most of the judder is
not frames the game failed to draw.

## Not the cause (checked in code)

- Pose consistency: each frame goes out with the poses it was drawn with.
- GPU-wide waits in the frame path: none.
- Swapchain hold time, OpenXR call order, the end-frame thread: correct (VR_FRAME_PACING_AUDIT.md).

## Network versus rendering

What the game can tell apart now (from `7102cf4c`):
- `[vr] streamed display times (10 s): N frames, N uneven (%), the farthest N ms off`: how irregular the runtime's
  display times are. High uneven share with the game's own frame times steady (`[vr] frame times`) points at the
  stream; the reverse points at the game.
- `[vr] frame times ...`: interval percentiles, missed display periods, `xrWaitFrame` waits.

What needs Virtual Desktop's overlay (manual, VDXR_BENCHMARK_RESULTS.md): game, encode, network and decode latency,
dropped frames, SSW state.

## Reproduction

1. Quest 3, Virtual Desktop streamer with VDXR selected, Wi-Fi.
2. Start the game with the log on (launcher: Graphics > Write a log file).
3. Outset Island, turn the head left and right steadily for 30 s, then walk while turning.
4. A/B on the same build: `WWHD_VR_STREAMED=0` gives the old behaviour (the shift and the 10 s Auto retries);
   the default is the fix.

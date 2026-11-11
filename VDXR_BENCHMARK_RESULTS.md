# VDXR benchmark results (2026-11-03)

**Hardware validation status: not run.** No Quest 3 or Virtual Desktop is on the development machine. The fix
builds (Windows x64); `interp_pacing_test` builds with the new checks (not run). Everything below is the procedure
and the baseline from player logs; the "after" column needs a Quest player.

## Baseline (player log, Quest 3 + VDXR on Wi-Fi, AMD RX 9060 XT, internal resolution 3x, build `fe5c42b`)

| Measure | Baseline | After (`7102cf4c`) |
|---|---|---|
| Headset refresh rate | 90 Hz (75 Hz on the desktop window) | to measure |
| Game frame rate | 43 to 46 fps (Auto at half rate) | to measure |
| Auto switches between full and half | repeated (several in a session) | expected: at most one every 40 to 160 s |
| Frame-timing shifts ("moved half a refresh later") | several | expected: none (off when streamed) |
| Display times apart (`Quest tracking`) | up to 155.6 ms | network; not changed by the game |
| Streamed display times uneven | not logged then | to measure (`[vr] streamed display times`) |
| Frames shown again by the game | 0 to 1 per 10 s | to measure |

## Test procedure (one player, one build, same scene)

1. Launcher: Graphics > Write a log file on; Save.
2. Virtual Desktop: note the refresh rate, codec, bitrate, and whether SSW is Automatic, Always or Off. Turn on its
   performance overlay.
3. Run A, the fix: start the game normally. Outset Island, 2 minutes: 30 s steady head turns, 30 s walking while
   turning, 30 s sword swings, 30 s looking at the sea.
4. Run B, the old behaviour: same, with `WWHD_VR_STREAMED=0` (environment variable or Steam launch option).
5. After each run, from Virtual Desktop's overlay: game latency, encode latency, network latency, decode latency,
   dropped frames. From the game: `captures/wwhd.log` (launcher: Open the log folder).
6. Optional runs, one change each: Virtual Desktop SSW Off; SSW Always; `vr.frameSynthesis=on` (depth sent).

## What to compare

| Measure | Log line |
|---|---|
| game frame rate and missed refreshes | `[vr] timing`, `[vr] frame times` (missed display periods, interval p95/p99) |
| Auto switches | count of `[interp] VR frame rate auto` lines |
| timing shifts | count of `frames near the headset's deadline` lines (B only) |
| network irregularity | `[vr] streamed display times (10 s)`: uneven %, farthest ms |
| controller tracking | `[vr] Quest tracking (10 s)` hand lines |
| GPU busy (encoder headroom) | run with `WWHD_PROFILE=1`: the `[prof]` GPU timeline |

Success: in A against B, no timing shifts, few Auto switches, the same or better frame times, and the player reports
less judder on head turns. If A and B feel the same and `streamed display times` shows a high uneven share, the
remaining judder is the stream (Wi-Fi, encoder, decoder), and Virtual Desktop's settings are the lever
(docs/VR_STREAMING.md, "Settings a player can try").

## Runtime comparison still to do

| Configuration | Status |
|---|---|
| Virtual Desktop + VDXR, Wi-Fi | procedure above |
| Virtual Desktop + SteamVR | same procedure (streamed detection still yes: a Quest) |
| SteamVR wired (PS VR2) | last run 89.5 fps, 92% of gameplay windows at 89.5+ (`streamed headset: no` expected) |
| Quest standalone | unaffected by `streamed()`; not run |

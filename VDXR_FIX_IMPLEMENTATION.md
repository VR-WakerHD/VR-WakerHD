# VDXR fix implementation (2026-11-03)

All in commit `7102cf4c` (pushed). Detail in `docs/VR_STREAMING.md`.

## Runtime detection, not runtime-specific rendering

`Session::streamed()` (`runtime/src/vr/openxr_session.h/.cpp`), decided once per session after the runtime's name
and the headset are known:
- a PC build (`kPcVrBuild`), and
- a Quest headset on any runtime (Meta's Link and Air Link, SteamVR's Steam Link, VDXR), or a runtime whose name says
  Virtual Desktop or ALVR.
- `WWHD_VR_STREAMED=1` or `0` forces it (A/B tests, other streamers).

Logged as `[vr] streamed headset: yes` or `no`. The renderer itself is the same on every runtime: only two pacing
rules read the flag. Wired headsets (PS VR2, Index on SteamVR), Android (Quest standalone) and the Steam Frame are not
streamed and are unchanged.

## Changes

| File | Change | Why |
|---|---|---|
| `runtime/src/gfx/vulkan/vr_vulkan.cpp` `note_display` | returns at once on a streamed headset | the half-refresh shift fired on network jitter, a hitch each time; the streaming runtime paces what it receives |
| `runtime/src/interp_pacing.h` `auto_rate_step` | on a streamed headset: 40 s at half rate before a full-rate try (not 10 s), doubled up to 160 s (not 40 s), back to 40 s after a try that holds | fewer changes of smoothness; Virtual Desktop's SSW also works at half rate |
| `runtime/src/interp.cpp/.h` | `set_vr_streamed`, read by Auto | carries the flag to the pacer |
| `runtime/src/vr/openxr_session.cpp` `begin_frame` | `[vr] streamed display times (10 s)` | measures the runtime's display-time steadiness without extra calls (one subtraction per `xrWaitFrame`) |
| `runtime/tools/interp_pacing_test.cpp` | the streamed waits checked | |

## Earlier the same day, also affecting Quest on PC

- `11588950`: no depth while dialog or menus show (VDXR hid the text layer behind the scene).
- `bb2fd65f`: Auto frame synthesis sends no depth for a Quest on a PC (`vr.frameSynthesis=on` still does).
- `c178e1ac`, `849bf1f9`: the NVIDIA crash on VDXR (swapchain lock; end-of-rendering guard).

## What was not changed, and why

- No sleeps, delays or frame caps were added; the runtime keeps display synchronization (`xrWaitFrame`).
- No synchronization was removed: none in the frame path was unnecessary.
- Resolution is unchanged: there is no evidence of GPU pressure in the Quest logs (the game held its rate).
- Depth for VDXR stays off under Auto until a Quest A/B shows whether VDXR uses it.

## Compatibility

SteamVR wired, Quest standalone and the Steam Frame take the old paths (`streamed()` false). The flag only removes
a timing shift and lengthens a wait, so a misdetected wired headset would lose the shift's correction near a
deadline, not correctness.

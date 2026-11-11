# Quest 3 VR experience audit (2026-11-03)

Camera, tracking, controllers, UI, comfort and interactions, as the code implements them, with what the standalone
ports do differently. The VR code is shared by PC VR, Quest and Steam Frame (`runtime/src/vr/`); the Quest differs in
frame rate (24-30 fps measured [log]), its Touch Plus controllers and its compositor. Marks as in
[QUEST3_PERFORMANCE_AUDIT.md](QUEST3_PERFORMANCE_AUDIT.md). Headset impressions so far are from PS VR2 on PC.

## 1. Camera and head tracking

| Item | Implementation [code, docs] | At Quest frame rates |
|---|---|---|
| One camera writer | `camera_draw` gets the head's eye/center/up after the interpolation blend; the game's state restored (docs/VR_CAMERA_AUDIT.md) | — |
| Pose timing | views located at the predicted display time + 1 frame on the game thread; each frame carries its views id to xrEndFrame; the runtime corrects only the prediction error | at 30 fps the runtime's ATW/positional reprojection carries the head between frames: rotation stays smooth |
| First-person position | Link's eyes, walk bob filtered, held above water, no bob in conversations | — |
| Horizon | the head's own roll only; the game's camera bank is not applied in first person | — |
| Cutscenes | the game's camera turned by the head (`vr.cutsceneFollow`) | forced camera motion: offer a fixed theatre screen option (P4) |
| Lock-on | the game's; grappling-hook lock-on | — |
| Recentring | stored origin and heading; gesture and menu | — |
| Margin | 5° beyond each eye's field on standalone (`fov_margin`): no black edges when reprojection turns the last frame | needed exactly because Quest reprojects most frames |

## 2. Controllers and interactions

| Item | Ours [code, docs] | Standalone ports | Opportunity |
|---|---|---|---|
| Profiles | Touch, Touch Plus (`XR_META_touch_controller_plus`), hands (`XR_EXT_hand_interaction`) | JKXR: Touch | — |
| Sword | tracked blade sweeps the game's own attack capsules; a swing presses B for two logic steps; hits vibrate once per swing | JKXR: velocity trigger thresholds, saber block debounce, haptic intensity setting | a haptic strength setting; block debounce for the shield |
| Shield | on its handle in the left hand | — | — |
| Bow | pointing, the arrow along the bow, draw by grip, string follows the hand | — | headset checks pending (docs/VR_AIMING.md) |
| Boomerang / hookshot / grapple | pointing with a laser quad; one dot | QuestZDoom: laser sight optional | — |
| Hammer | tracked weapon, the game's attacks | Lambda1VR: physical crowbar | — |
| Telescope / Picto Box | eyepiece crop; Picto Box mode | JKXR: weapon scopes | — |
| Item selection | item wheel quad (CPU picture) | OoT VR: 3D items around the hand, flick to choose; Lambda1VR: reach over the shoulder | a "reach to the hip/shoulder" shortcut for the sword and shield (P4) |
| Input latency | buttons at the next logic step (≤ 33 ms); poses every frame | same class | at Quest's 30 fps the picture of a swing lags a frame more than at 72 |

## 3. VR UI

- **Wrist HUD** matches the intended layout [code, docs/VR_WRIST_HUD.md]: hearts on top of the left wrist (back of
  the hand), item buttons under it (palm side), rupees on top of the right wrist; each panel shows while it faces the
  eyes (45°, staying up to 60°) within 0.9 m; one CPU picture (512x368), one quad layer per panel; no per-eye copies.
- **Game UI** (prompts, dialogue, menus): one head-locked quad layer in per-eye mode, now kept on re-shown frames
  (fixed this session) — important on Quest, where most frames are re-shown by the runtime.
- **Foveation / dynamic resolution**: quads are sampled by the compositor at their own resolution: unaffected.
- **Readability on Quest 3**: about 25 pixels per degree in the headset; the hearts panel is about 30 px/cm at arm's
  length [est]: check with the player.
- **Depth**: the UI quad is at a fixed 3.5 m; depth is not sent while text shows (a runtime compositing by it hid the
  UI). Dialogue at the speaker's distance is a P4 idea.

## 4. Comfort

| Option | State [code] | Note |
|---|---|---|
| Smooth / snap turning | both | — |
| Smooth locomotion | the game's own movement turned to the head | uneven game-time steps at low fps are the main discomfort source on Quest [est] |
| Vignette | not built | JKXR has `vr_comfort_vignette`; QuestZDoom has none |
| Camera shake | not audited here (docs/VR_SCREEN_EFFECTS.md covers the screen effects, not shakes) | check in the headset whether the game's shakes move the VR view |
| Forced motion | cutscene follow; boat | theatre option (P4) |
| Scene changes | the game's fades | OoT VR fades the whole view on scene changes |

## 5. Platform and lifecycle

| Item | State |
|---|---|
| Sleep / wake, system menu, focus | session states handled (stopping -> end session; exiting/loss -> desktop fallback) [code]; on Android the window surface may go: the present path waits for a new surface [code]; **untested on Quest** (docs/VR_ANDROID.md "Still to check") |
| Controller disconnect | tracking gaps handled (docs/VR_CONTROLS.md "Tracking gaps") |
| Runtime restart | the session is lost: back to the flat screen; no re-entry without a restart [code] |
| Thermal | the heat guard reads `AThermal_getThermalHeadroom` (Android 12+, looked up at run time) |
| Memory pressure | handled: Android's trim callbacks go through `MemoryPressure.java` / `MemoryPressurePolicy.java` to `runtime/src/platform/memory_trim.h` [code]; now also logged every 10 s (this session); whether its policy suits a Quest's 5.75 GiB is untested |

## 6. Recommended experience work (P4, each needs the headset)

1. Haptic strength setting and a shield block debounce (JKXR's pattern).
2. Comfort vignette for smooth turning and locomotion (opt-in).
3. Theatre screen for long cutscenes (opt-in).
4. Reach-to-holster shortcuts for the sword and shield.
5. Fade on scene changes (check what the game's own fade looks like in VR first).

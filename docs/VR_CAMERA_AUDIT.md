# VR first-person camera: audit, fixes and headset test plan (2026-10-30)

This was done with the headset away, so nothing here has been checked in a headset. Each finding is marked by how it
was verified:

- **[code]** by reading the code;
- **[test]** by an automated test (`runtime/tools/vr_test.cpp`, `test_camera_audit`);
- **[build]** by a successful build;
- **[desktop]** by a desktop run (none were made, because the user was away);
- **[headset]** not verified yet: it needs the headset.

**Headset (2026-10-30, 14:55, PS VR2, Capped 90):** the user tried this build: "everything felt good". That covers
the overall feel only. The individual checklist items in section 18 were not reported one by one, and the run's
frame-rate log was not found.

Short version: the pipeline already had one camera writer and one head pose a frame. The audit found no second writer
and no double-applied offset. It did find four timing and pivot problems: parts of the view or of Link's body moved on
the 30 Hz logic grid, or about a different point than the view. One stereo snapshot inconsistency was also found. All
five are fixed. Two open design questions need the headset; they are isolated in section 17.

---

## 1. Camera ownership map

### Inputs (who provides what)

| Input | Source | Rate | Thread | Where |
|---|---|---|---|---|
| Head pose (6DoF) and both eyes | `xrLocateViews` + `xrLocateSpace(view_, space_)`, one call pair, at one predicted time | every camera pass | game thread | `game_views(time, v, fresh=true)` in `CameraOverride` → `Session::locate_views` |
| Predicted display time | render thread's last `xrWaitFrame` plus the frames still queued × refreshes per frame | every camera pass | game (reads an atomic set by render) | `display_time()` |
| Locomotion (Link's position, facing) | the game's own logic (`daPy_lk_c::execute`) | 30 Hz | game thread | `link_executed` → `g_link.pos/prevPos/heading` |
| Eye point on Link (offset, height) | Link's head joint + `eyePos`, held against animation sway (`Settled`) | 30 Hz, blended per frame | game thread | `link_executed` → `g_link.eyeOffset/eyeHeight` (+prev) |
| Tracking origin (reference frame) | recentre (`g_room.base`) + room-scale moves taken (`g_room.prev/cur`) | 30 Hz, blended per frame | game thread | `room_origin(t)` |
| View heading (`WorldMapping::heading`) | recentre; smooth turn (per frame); snap turn (pad read); ladder/ledge/lock-on rig turn (**now per frame**) | see column | game thread, `g_mutex` | `CameraOverride`, `filter_pad_controllers`, `recenter` |
| Walls | guest `cBgS::LineCross` from the eyes | 30 Hz | game thread | `wall_lean_update`, `room_scale_move` → `g_wallLean` |
| Eye offsets (IPD) for drawing | render thread's `xrLocateViews` at `xrWaitFrame` time | per swap | render thread | `frame_begin` → `stereo::set_xr` → latched after the swap |

### Writers of the drawn camera (eye / centre / up / bank of the game camera)

There is exactly one, `vr::game::CameraOverride` (constructed in `interp.cpp camera_draw_orig`). **[code]**

1. The game's camera logic (`dCamera_c`, 30 Hz) writes eye/centre/up, plus its shake, in the camera store (024F8430).
2. Frame interpolation (`interp.cpp camera_draw_body`) blends that between steps (`write_cam`) **before** step 3.
3. `CameraOverride` saves eye/centre/up/bank and writes the head's look-at (`look_at(head)`; bank 0). It runs last,
   immediately before the game's own `camera_draw` (`f_024FFC40_orig`) builds the view and projection matrices.
4. The game's `camera_draw` builds the view matrix from what `CameraOverride` wrote.
5. `~CameraOverride` restores the game's own values, so the camera logic never continues from the head.

Grep check: `st_vec(cam + kCamEye|kCamCenter|kCamUp)` appears only in `CameraOverride` and its destructor.

### What each mode uses

| Mode | Position | Rotation | Original camera |
|---|---|---|---|
| First-person play | `anchor(t)` (Link's eyes, blended) + (head − origin(t)) × scale, lean cap, walls, lean-back limit | heading × head orientation (full 3DoF incl. roll) | logic runs (the left stick's frame and lock-on still need it); its result is replaced in the draw |
| Third person | game camera eye | game camera × head turned relative to its start | used |
| Events, cutscenes | game camera eye (menu zoom-out for title/file select) | game camera × head relative | used (`vr.cutsceneFollow`) |
| Telescope, Picto Box | subject camera | head | used, zoom shown as magnification |
| Menus / panel | quad layers in tracking space | — | not involved |
| Not in VR | — | — | unchanged (`running()` false: `CameraOverride` returns at once) |

### Readers

- The game's `camera_draw` (view and projection matrices, culling) reads the override.
- The per-eye passes read the view-space eye shift `stereo::frame.eyeOffset` (`project_eye`).
- The compositor reads the views stored with the frame's swap (`store_views` → `OP_SWAP` → `frame_end`).
- The arms read `g_passHead` (wrist HUD) and the head-relative controller offsets (`frame_correct`).
- Room scale, walls and the stick direction read the head pose of the same display time (`game_views`, cached).

### Other things that move Link or the reference frame

| Writer | What | When | Notes |
|---|---|---|---|
| `room_scale_move` | Link's x/z (`set_link_xz`: `current.pos` and `l_debug_keep_pos`) by the head's walk; origin by the same | before his logic, standing in first person | applied once: the two moves cancel in the view **[test]** |
| `room_scale_move` (about eyes) | Link's x/z so his eyes stay put when the body turns to the head | same | view and origin untouched |
| `link_executing` | Link's facing = heading + body yaw | before his logic, standing | |
| `frame_correct` | drawn joints turned by the stick's turn since the step | every pass | **pivot fixed** (section 3) |
| `recenter` | origin, room base, heading, body yaw | recentre gesture, runtime recentre, back from an event/third person | the camera cuts there anyway |

### Update order of one frame (game thread)

1. **Pad read** (30 Hz, `filter_pad_controllers`): controllers, snap turn, left stick remap, rig-turn target.
2. **Link's logic** (30 Hz): `link_executing` (facing, room-scale walk, walls), then the game, then `link_executed`
   (position, eye point, previous values for blending).
3. **Camera pass** (every frame, `CameraOverride`), in this order:
   1. locate the views for the display time;
   2. advance the smooth turn by this frame's display-time interval;
   3. advance the **rig turn** (new) by the same interval;
   4. recentre if asked;
   5. `head_in_game` builds the one head pose;
   6. look-at written; game builds matrices;
   7. `store_views` → travels with this frame's swap.
4. **Draw**: Link's model (`frame_correct` turns it by the same pass's turn about the same pivot), the eye passes.
5. **Render thread** at the swap: `xrWaitFrame` / `xrBeginFrame` (`frame_begin`), eye copies, release, then
   `xrEndFrame` with this frame's views (`frame_end`).

## 2. Competing controllers and transform writers found

- **Two writers of the drawn camera in one frame:** none. The interpolation blend is overwritten by the override,
  which is final. **[code]**
- **Game camera logic overwriting the VR pose:** no. The logic runs on its own state, which is restored after every
  draw. Its shake, FOV shake and bank are not applied in first person: the override writes bank 0 and the headset's
  FOV. **[code]**
- **Tracking offsets applied twice:** no. Room scale moves Link and the origin by the same amount; the view takes
  (head − origin) once. **[code] [test]**
- **Mixed coordinate spaces:** none found. The tracking space is metres (−Z forward) and the game world is units
  (heading 0 = +Z). The single conversion is a yaw of heading + π plus scale (`vr_math.h`). Lean, walls and room
  scale all convert through `to_game_dir`. **[code] [test]**
- **Euler round trips:** none on the head path. Quaternion → (forward, up) → the game's look-at keeps the roll
  through `up`. s16 angles are used only for Link's facing, never for the head. **[code] [test]**
- **Stale samples:** the camera always locates fresh (`fresh=true`). Other game-thread users reuse that sample for
  the same display time. **[code]**
- **Update after matrices are built:** none. Every view input is final before `look_at` is written. **[code]**
- **Different rates:** found (section 3). The ladder/ledge/lock-on view turn ran at the pad's 30 Hz, the body's
  turn pivot differed from the view's, and the lean-back plane turned at 30 Hz.
- **Threads:** the game thread owns all camera state under `g_mutex`; `frame_correct` (draw) takes the same mutex.
  The render thread receives the views by id through the swap command. **[code]**
- **Eyes from different snapshots:** both eyes always come from one head pose (`as_rendered`). The *submitted* eye
  offsets, however, came from the game thread's locate, while the *drawn* offsets came from the render thread's.
  This is fixed (section 10). **[code]**

## 3. Root causes of jitter, stutter and counteracting movement

| # | Cause | Effect in the headset | Status |
|---|---|---|---|
| A | Ladder/ledge and lock-on view turns eased at the 30 Hz pad read (`turn_towards` at pad dt) | the world turned in 30 Hz steps while frames came at 90: up to ~10 degrees a step at a lock-on's start, a visible judder; on ladders, the turn round to the wall | **fixed**: per frame (`RigTurn`) **[code] [test]** |
| B | Stick turn between steps: the view turns about Link's eyes (the tracking origin's game point), but `frame_correct` turned his drawn body about his feet | his body and shoulders slid sideways against the view by eye distance × turn (about 1.3 units a step at 120 °/s, 9 units for a 30° snap), then snapped back at the next step: a 15 Hz sawtooth under the view during smooth turns | **fixed**: body turned about the eye point (`turn_pivot`) **[code] [test]** |
| C | Lean-back limit plane oriented by the step's facing, not the drawn one | while leaning back past 8 cm and turning, the view was pushed sideways in 30 Hz steps | **fixed**: blended facing (`blend_heading`) **[code] [test]** |
| D | Smooth turn's interval re-read `display_time()` after the views were located | the turn's per-frame share could follow a different interval than the head's pose (the render thread predicting anew between the two reads): an uneven turn | **fixed**: uses `v.time`, the views' own time **[code]** |
| E | Submitted eye positions from a different locate than the drawn ones | on a runtime whose eye positions move between locates (eye-tracked IPD), the compositor reprojected from eyes the picture was not drawn from: small stereo error | **fixed** (section 10) **[code]** |
| F | PS VR2 tracking rate dropping (SteamVR `trk:` 30/16) and frame dips in grass (earlier sessions) | side-to-side jitter | outside the camera code: see `docs/VR_PERFORMANCE.md`, and the grass batching work |

Already correct (kept): the eye point blended between steps; hold-and-glide on game turns; the turn about the eyes when
the body follows the head; the jump and gap holds on the head's tracking; the pose predicted per refreshes per frame;
the virtual vsync locked to the headset's clock.

## 4. The original camera update architecture

`dCamera_c` logic at 30 Hz → interpolation blend per pass → `camera_draw` builds matrices. In VR the head-tracked
override goes in between (section 1). The game camera is never disabled, because the stick direction, lock-on,
events, the telescope and third person need it.

## 5. The authoritative VR camera pipeline

(Since 2026-10-30 this pipeline is one class, `HeadController` in `runtime/src/vr/vr_head.h`: docs/VR_HEAD_CONTROLLER.md.)

One function builds the first-person head, once per camera pass (`CameraOverride` → `head_in_game` →
`gameplay_head`):

```
view position  = anchor(t) + R(heading + π) · limit((head − origin(t)), cap, walls) · scale,  then the lean-back hold
view rotation  = R(heading + π) · head orientation
eye e          = view ⊕ (eyeX[e], 0, 0) in view space        (both eyes, one head pose)
```

- `head` comes from OpenXR. `anchor(t)` is the locomotion (Link's eyes, blended like his model). `origin(t)` is the
  reference frame (blended the same way). `heading` holds the turns.
- Everything else only feeds inputs: walls feed the lean cap, room scale moves Link and the origin, and turns change
  `heading`.
- This session made no architectural change. The audit confirmed this pipeline is already the single authority.
  What changed are the inputs that were updated on the wrong grid or about the wrong pivot.

## 6. Files and functions changed

| File | Function / symbol | Change |
|---|---|---|
| `runtime/src/vr/vr_game.cpp` | `RigTurn g_rigTurn` (new) | the ladder/ledge/lock-on turn target |
| | `filter_pad_controllers` | sets `g_rigTurn` instead of turning `g_map.heading` at the pad read; cleared when neither applies |
| | `CameraOverride::CameraOverride` | smooth-turn interval from `v.time`; applies `g_rigTurn` per frame (and the lock-on body yaw) |
| | `frame_correct` | the stick-turn pivot is the blended eye point (`turn_pivot`) |
| | `LinkStep::prevHeading`, `headingSeeded`; `link_executed` | the previous step's facing kept |
| | `head_in_game` | lean-back limit through `hold_lean_back` with the blended facing |
| `runtime/src/vr/vr_camera.h` | `hold_lean_back`, `turn_pivot` (new) | the two helpers above, testable |
| `runtime/src/vr/vr_ik.h` | `RigTurn` (new) | per-frame rig turn (next to `turn_towards`) |
| `runtime/src/vr/vr_math.h` | `blend_heading`, `eye_pose` (new) | heading blend the short way; an eye from the head pose |
| `runtime/src/vr/vr.cpp` | `as_rendered` | uses `eye_pose` (same result) |
| `runtime/src/gfx/vulkan/vr_vulkan.cpp` | `frame_end` | submitted eye poses rebuilt from the drawn eye offsets |
| `runtime/tools/vr_test.cpp` | `test_camera_audit` (new) | section 13 |

## 7. OpenXR timing and tracking

Lifecycle, as audited **[code]**:

- The render thread runs `xrWaitFrame` → `xrBeginFrame` in `frame_begin` at each swap, then acquires the eye images
  and waits on them (100 ms timeout, so a stopped compositor cannot hang the game). It copies the eyes, releases the
  images under the graphics-queue lock, and calls `xrEndFrame` with `displayTime` = that frame's
  `predictedDisplayTime` in `frame_end`.
- The game thread locates the head for its own prediction: the render thread's last time plus the frames queued ×
  refreshes per drawn frame (`display_time`). That is ahead of the render thread's frame, because the game builds the
  next frame while the previous one presents.
- The submitted projection views carry the pose the frame was drawn with. The compositor reprojects from that pose to
  the real one at `displayTime`, so ATW/ASW stays intact and nothing is re-located at submit time.
- No blocking was added. `game_views` takes a short local mutex; the render thread does not wait on the game thread.

Changed: the smooth turn's per-frame interval now comes from the views' own display time (cause D). Unchanged: the
prediction itself, `xrWaitFrame` placement, reprojection, and the tracking-gap and jump holds.

## 8. Transform and coordinate-space corrections

None were needed in the conversion itself; it is pinned by `test_conversion`, `test_gameplay_camera_regression` and
the new test. Two pivot and frame fixes were made: the body turn's pivot (B) and the lean-back plane's facing (C).

## 9. Smoothing and interpolation

| Filter | Applied to | Verdict |
|---|---|---|
| None | raw head pose (position and orientation) | none exists, and none was added **[code]** |
| Blend between steps (`pass_t`) | Link's position, eye point, origin, facing (new, for the lean-back plane only), body turn | render interpolation of 30 Hz locomotion: kept |
| `Settled` dead zones (eye height 6 u, offset 4 u, eye forward 2 u) | Link's animated eye point (idle sway) | animation filtering, not the head: kept |
| Hold and glide on a game turn (> 20° in a step, feet still) | eye point only | kept (fixes the 37-unit swings when the game turns him) |
| `turn_towards` easing | ladder/ledge/lock-on view base | kept; now per frame |
| `PoseHold` / gap blend 0.1 s | controllers after a tracking gap | kept (hands, not the view) |
| Session `hold` (last views turned with the head) | head during a tracking loss (≤ hold s) or a > jump-limit teleport | kept: a safety fallback, not smoothing |
| Frame blur (`interp_fx.cpp`, Swift Sail, some attacks) | game's own screen-space blur | unchanged; see 17 |

## 10. Stereo rendering

- Both eyes share one head pose: orientation equal, offsets along its right, separation = the runtime's IPD
  **[code] [test]**.
- The eye view matrix equals the head camera's view shifted by the eye offset (`project_eye`) **[test]**.
- One projection (the symmetric union of both eyes' FOV) for both eyes. On Linux the submitted part is the eye's own
  FOV (`fov_crop`, unchanged).
- **Changed (E):** `frame_end` now submits each eye at the head pose of the frame's views plus the offset that
  frame's eye passes used (`stereo::frame.eyeOffset`, latched at the swap before). Previously the submitted offsets
  came from the game thread's locate, while the drawn ones came from the render thread's.
  - On a headset with a fixed IPD (PS VR2, Quest 3 with its wheel at rest), the two are equal to float precision.
    Nothing changes there.
  - Guarded: only in XR mode, only with a left offset below 0 and a right offset above 0, only with a positive
    world scale.
  - Remaining approximation: the world scale is read at submit. If `vr.unitsPerMetre` or the arm-matched scale
    changes, one frame can be off.
- Multiview, the eye rows and batching are untouched.

## 11. 6DoF regression check

`test_camera_audit` **[test]**: lean on all three axes maps 1:1 times the scale, with no zoom. Yaw, pitch and roll
pass unchanged, and roll survives the look-at. The 1 m lean cap applies horizontally only; crouching is never held.
The walk is applied once (room scale) and half a walk once (walls). Recentring puts the head at Link's eyes facing his
heading. The existing `test_gameplay_camera_regression` still passes, unchanged. `kLeanBackMetres` (0.08),
`kWallMargin` (6), `vr.maxHeadOffset` (1) and `vr.roomScale` are unchanged.

## 12. Player locomotion

- The left stick is remapped to the head's heading **[test]**.
- Room scale, collision and gravity are the game's own; that code path is unchanged.
- Smooth turn: same speed and gating; only its interval source changed (D).
- Snap turn: unchanged (pad read). Its body now turns about the eye point between steps, so there is no 9-unit
  sideways jump of the body for one step.
- Ladder/ledge/lock-on reach the same heading over the same time, in 90 smaller steps a second instead of 30 **[test]**.

## 13. Automated tests

All in `build/game` (Release, llvm-mingw clang): `vr_test`, `interp_pacing_test`, `interp_camera_test`,
`stereo_test`, `stereo_pointer_test`.

- **Before the changes:** all pass.
- **After:** all pass.
- **New, `test_camera_audit`:**
  - 6DoF composition and quaternion order; roll through the look-at;
  - lean cap;
  - turning + lean about the eyes;
  - body pivot = view pivot;
  - room-scale cancellation (whole and partial walk);
  - stick + head heading;
  - recentre;
  - both eyes from one snapshot and the per-eye view shift;
  - lean-back hold and blended facing across the ±π wrap;
  - per-frame rig turn against the pad-rate result.

## 14. Builds

- **Windows VR (`build/game`):** every changed file compiled without new warnings, and `vr_test` was built and run.
  See the end of this section for `wwhd.exe` and the launcher.
- **Not available on this machine:** Linux (the WSL Fedora image has no compiler), Steam Frame Lepton (no Android
  NDK installed) and Steam Frame Native (no arm64 toolchain).
  - The changes use no platform API: plain C++ in the headers, `vr_game.cpp` and `vr_vulkan.cpp`. A build there
    still needs to be run.
  - The one platform-sensitive thing found was the `<windows.h>` `near`/`far` macros. That is why `RigTurn` lives in
    `vr_ik.h`, which `vr_vulkan.cpp` does not include.
- **The non-VR build is not affected:** the changes are in VR-only files, and `vr_vulkan.cpp`'s non-VR stubs are
  unchanged.

The first relink of `wwhd.exe` was refused because a game process (PID 29948, not started by this session) was
running from `build/game`. It went through once that process had exited:

- `wwhd.exe` was rebuilt at 13:56 and contains these changes.
- The batching session relinked it again at 13:57 from the same sources and is running it. My last edit after that
  was a comment, so nothing changes in the code; that relink was refused while the game was open, and I did not
  retry it.
- It also contains the batching session's uncommitted work, which shares this checkout.
- `wwhd-vr-launcher.exe` was already up to date: no launcher source changed.

## 15. Performance

There are no reliable measurements without the headset, and no desktop runs were made. By inspection **[code]**:

- **Per frame:** one `turn_towards` (a few flops, only on a ladder or ledge or during a lock-on), one heading blend,
  one vector lerp in `frame_correct`, and two `eye_pose` calls in `frame_end`.
- **Not added:** locks, heap allocations, extra locates, buffering or render passes.
- **Removed:** one `display_time()` call a frame (an atomic read and a few multiplies).

Latency is unchanged: the head pose is the same sample, applied the same way.

## 16. Remaining limitations

- The world-scale read at submit (section 10).
- The prediction offset still assumes the render thread's next frames follow its last `xrWaitFrame` cadence. A
  missed deadline makes one frame's pose an interval off, and the compositor's reprojection covers it, as before.
- Link's position and facing stay 30 Hz logic. They are blended per frame for drawing (rule: 30 Hz logic kept).

## 17. Needs the headset (isolated, not guessed)

1. **The smooth/snap turn pivot while leaning without room scale.** Seated, crouching, climbing, locked on, or against
   a wall, the lean stays an offset from Link's eyes, and a stick turn swings the view around his eyes, like a swivel
   chair, rather than around the head.
   - Turning about the head is the usual VR choice, but here it would take the view off Link's body, or move Link,
     against the walls and body-under-head rules.
   - Standing with room scale on, the offset is the head's movement within one step (a few cm), so the two pivots
     are equal there.
   - Decide in the headset whether the swing is felt (test L3). If it is, the change belongs in `CameraOverride`
     (rotate `g_room.base` about the head) together with Link's position.
2. **The game's frame blur in VR** (Swift Sail, some attacks): a screen-space overlay of the last frame. In a headset
   it may read as ghosting when the head moves. Turning it off loses fidelity (user rule), so it is unchanged. Check
   test V7.

## 18. Headset test checklist

Setup for every test:

- `WWHD_VR_FRAME_RATE=full` (Capped 90). PS VR2 first.
- Restart SteamVR before A/B runs.
- Logs: the game's log (`[vr]` lines), plus SteamVR's `vrserver.txt` (`trk:` rate) and `vrcompositor.txt`.
- Note the average fps and the % of frames at 90. A run averaging below 35 fps is a fail.
- Useful traces: `WWHD_VR_TRACE_LAG=1` (hands), `WWHD_VR_TRACE_EYES=1` (eye height), `WWHD_VR_TRACE_LEDGE=1`
  (ladders and ledges).

### Head movement (stand on Outset, near the house and the fence)

| # | Do | Expect | Failure looks like | Logs |
|---|---|---|---|---|
| H1 | lean forward and back slowly, 30 cm | near objects grow and shrink by perspective; view goes back about 8 cm, then holds smoothly | zoom feel, a step at the 8 cm limit | `views held at his back N` |
| H2 | lean back past 8 cm and turn the head and body left and right | the held view turns smoothly with Link | sideways steps 30 times a second (cause C) | same |
| H3 | move the head left/right 30 cm, look past the fence post | parallax reveals what is behind | the view sticks, slides, or the body lags | `head at most ... m from the view's anchor` |
| H4 | crouch fully and stand | view drops and rises 1:1; Link's height is the game's | jumps; Link moves | — |
| H5 | head circles and diagonals combined with turning the head | world fixed, no swim | swimming, wobble | `xrWaitFrame` p50 in the frame log |
| H6 | lean around a wall corner up to it | the view stops about 6 units short; no clipping | seeing through the wall; jitter at the wall | `body under the head: ... not all taken` |

### Locomotion

| # | Do | Expect | Failure looks like | Logs |
|---|---|---|---|---|
| L1 | walk with the left stick while looking and leaning sideways | moves where the head points; view steady | lurches, drift | — |
| L2 | smooth-turn at full stick while turning the head the other way | even turn, the body and shoulders stay under the view | body sliding under the view (cause B), uneven steps | `smooth turn: N frames ... degrees a frame` (most ≈ average) |
| L3 | **seated** (or crouching), lean 30 cm sideways, smooth-turn | the view swings around Link's eyes like a swivel chair (current design) | **report whether this swing is uncomfortable**: section 17.1 | — |
| L4 | snap turn standing, then while leaning | instant turn, body under the view at once | a body jump for one step | — |
| L5 | Z-target an enemy or person (lock-on) from 90° away | the world turns smoothly to the target | 30 Hz stepping (cause A) | `lock-on: the view turns ...` |
| L6 | climb a ladder, step onto a ledge, sidle along a wall | the view turns smoothly to face the wall/ladder | stepping | `ladder/ledge: ...`, `WWHD_VR_TRACE_LEDGE=1` |
| L7 | walk into walls and objects, standing and walking | stops at the wall; no sliding of the world | the world slides with the head | `not all taken` |

### Visual stability

| # | Do | Expect | Failure looks like | Logs |
|---|---|---|---|---|
| V1 | stare at a sign 1 m away and move the head | it stays fixed in the world | micro-jitter | frame timing log: missed periods |
| V2 | look at distant sea and islands while leaning | rock steady | shimmer, swim | — |
| V3 | close one eye at a time on a near object | both eyes consistent, no vertical mismatch | double vision, eye strain (cause E) | `[vr] left/right eye: ... mm` (once) |
| V4 | stand still 30 s | no motion at all | bob or creep | `camera base moved ... standing still` = 0.00 |
| V5 | grass field at full speed | stable 90 | dips → judder (cause F) | fps, `trk:` rate |
| V6 | cutscene, title, file select, a pause menu | game camera + head rotation; panel steady | snapping, the panel drifting | `first person again: ...` |
| V7 | Swift Sail or an attack with a blur | acceptable | ghosting when the head moves (section 17.2) | `[interp-fx] frame blur` |

### Runtimes and headsets

- **PS VR2 (SteamVR, Windows):** all of the above. Also check `vrserver.txt` `trk:` stays at its full rate, and
  that the repeat count in the log is 0 in normal play.
- **Quest 3 (Quest Link / Virtual Desktop / SteamVR):** H1–H6, L2, L5, V1, V3. The Quest-only code (views reuse on
  loss, rendering defaults) is unchanged. With an IPD change during play, V3 should stay right.
- **Linux (SteamVR / Monado):** H1, V3. The submitted FOV crop is unchanged; check there is no double vision.
- **Steam Frame (Lepton / Native):** build first; then H1, L2, V1.

Do not count any of these as passed until they have been run in the headset.

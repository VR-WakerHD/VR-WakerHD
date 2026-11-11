# VR first-person head: one controller (2026-10-30)

This change follows the camera audit (`docs/VR_CAMERA_AUDIT.md`) and its fixes. The user tried those in the headset
("everything felt good").

This pass is a refactor that keeps the behaviour the same. The audit found exactly one writer of the drawn camera.
This pass also gives the first-person head **one owner in the source**: `HeadController` in
`runtime/src/vr/vr_head.h`.

The labels below say how each statement was checked:

- **[code]** checked by reading the code;
- **[test]** checked by `vr_test`;
- **[build]** checked by compiling;
- **[headset]** not yet checked: it needs the headset.

## 1. Where to look

| Question | One place |
|---|---|
| How the first-person head transform is calculated | `vr_head.h` `HeadController::first_person` (and `step_head` for the arms' targets during Link's logic) |
| How the tracking space is placed in the world (origin, heading, scale) | `HeadController`: `recenter`, `turn`, `advance_rig_turn`, the room-scale methods |
| Where the final camera state for rendering is produced | `vr_game.cpp` `CameraOverride::CameraOverride`: it asks the controller for the head, writes the game camera's look-at once, and hands the frame's views to its swap (`store_views` → `set_frame_views`) |
| How both eyes get their poses | `vr.cpp` `as_rendered` (`eye_pose` of one head pose) for the views the frame is drawn with; `vr_vulkan.cpp` `frame_end` submits the eyes at the drawn offsets |

## 2. The controller

`HeadController` is header-only and does not depend on OpenXR, so it is tested without a headset. It owns:

| State | Before | Now |
|---|---|---|
| Mapping tracking space → game (origin, heading, scale, anchor) | `WorldMapping g_map`, written in 6 places | `HeadController::map_`, read through a `const` alias; written only by its methods |
| Room-scale origin (base, two steps' cumulative walk, pending walk, about-eyes turn) | `RoomScale g_room`, written in 12 places | `HeadController::room_`, `const` alias; methods `step_new_link`, `step`, `step_again`, `begin_move`, `walk`, `turned_about_eyes`, `take_turned_about_eyes`, `set_catching_up` |
| Calibration (recentred, for which Link, a recentre asked for) | `g_calibrated`, `g_recenter`, `g_calibratedLink` | `recenter`, `request_recenter`, `calibrated_for` |
| Stick turn total (Link's body turns by it between steps) | `g_turnTotal` | `turn`, `turn_total` |
| Ladder/ledge/lock-on turn | `g_rigTurn` | `set_rig_turn`, `stop_rig_turn`, `advance_rig_turn` |
| Walls' lean limit | `g_wallLean` | `set_wall_lean`, `wall_lean` |
| Game-camera mode (third person, events, menus) and its head yaw at the start | `g_eventActive`, `g_eventYaw` | `begin_game_camera`, `game_camera_view`, `end_game_camera` |
| Views drawn by display time (screen blur) | `g_viewHist` | `begin_frame`, `drawn`, `blur_reprojection` |
| Diagnostics (walks taken, views held at his back) | 5 globals | `take_stats` |

**Enforced by the compiler [build]:**
- `vr_game.cpp` keeps `const WorldMapping& g_map` and `const RoomScale& g_room` for its ~70 readers (hands, arms,
  sword, wheel, laser). A write through them doesn't compile.
- The tree compiles, so no direct writer is left.
- `vr_test` checks with `static_assert` that the controller's accessors are read-only.

**The data flow per frame:**

```
OpenXR (xrLocateViews + xrLocateSpace, one predicted time)      sticks (pad read, 30 Hz; the smooth stick each frame)
            |                                                              |
            v                                                              v
   CameraOverride (game thread, each camera pass, g_mutex) --- turn / advance_rig_turn --> HeadController
            |   mode: first person / third person / event / menu                              |
            |   first person: first_person(head, Link's eyes, facing, t, limits) <------------+
            |   game camera:  game_camera_view(eye, center, up, head)
            v
   look_at(head) -> the game camera's eye/centre/up (restored after the draw)    as_rendered(views) -> both eyes
            |                                                                             |
            v                                                                             v
   the game builds the view matrix; stereo shifts it per eye            store_views -> with the swap -> xrEndFrame
```

**Inputs and what they give:**

| Input | Gives | Writes |
|---|---|---|
| OpenXR (`game_views`, `Session::locate_views`) | head pose and eyes for one display time | nothing of the camera |
| Link's logic (`link_executed`) | eye point, facing, position (blended per frame) | `g_link` (his state, not the camera's); `step*` on the controller |
| `room_scale_move` (before his logic) | the head's walk into his position, the about-eyes turn | Link's position through the game (`set_link_xz`); `walk`, `turned_about_eyes` |
| `wall_lean_update` | the free lean in front (guest `cBgS::LineCross`) | `set_wall_lean` |
| `filter_pad_controllers` | snap turns, rig-turn targets, recentre gesture | `turn`, `set_rig_turn`/`stop_rig_turn`, `request_recenter` |
| `CameraOverride` | smooth turn each frame, the mode, the frame's head | `turn`, `advance_rig_turn`, `recenter`, `begin/end_game_camera`, `first_person`, `begin_frame`/`drawn` |

## 3. Ownership map

| System / function | Reads camera | Writes position | Writes rotation | Frequency | VR role |
|---|---|---|---|---|---|
| `HeadController::first_person` | — | **final first-person head position** | **final orientation** (mapping × head) | each camera pass | **authority** |
| `HeadController::game_camera_view` | game camera eye/centre/up | game's eye | game camera × head turn | each pass outside first person | authority for those modes |
| `HeadController::recenter` / `turn` / `advance_rig_turn` | — | mapping origin | mapping heading | recentre, frame, pad | owner of the reference frame |
| `CameraOverride` ctor / dtor | game camera | guest camera eye/centre/up (the head's look-at) | (look-at up, bank 0) | each pass | the one writer to the game; restores the game's own after the draw |
| Game camera logic (`dCamera_c`) | its own | its own state | its own state | 30 Hz | input in first person (stick frame, lock-on); the view in third person, events, menus |
| Frame interpolation (`interp.cpp camera_draw_body`) | game camera | blended game camera | blended | each pass | overwritten by `CameraOverride` in VR (it runs after) |
| `room_scale_move` | mapping (const), head | Link's position | Link's facing (`set_link_facing`, `link_executing`) | 30 Hz | contributor (locomotion), never the camera |
| `link_executed` | — | `g_link` eye point, blends | — | 30 Hz | contributor (anchor) |
| `frame_correct` | mapping (const), turn total | Link's drawn joints | drawn joints | each pass | contributor (body under the view) |
| `as_rendered` / `frame_end` | views | eye poses | eye orientation = head | each frame | stereo from the one head pose |
| `display_time` | render thread's prediction | — | — | each pass | timing input |
| `vr_vulkan.cpp frame_begin` | runtime | eye offsets for drawing | — | each swap | IPD only |

There are no competing controllers. In first person, the only camera writer is `CameraOverride`, and the only head
pose it writes is the controller's. **[code] [build]**

## 4. What moved, stayed or went

| Function | Change |
|---|---|
| `recenter` (vr_game.cpp) | the mapping and room part → `HeadController::recenter`; the body's yaw stays (input) |
| `head_in_game` | → `HeadController::first_person` (the lean-back limit and walls inside); the test-pitch aid kept |
| `room_taken`, `room_origin`, `room_origin_now` | → `HeadController::taken` (private), `origin`, `origin_now` |
| `link_executed` room bookkeeping | → `step_new_link`, `step`, `step_again` |
| `room_scale_move` / `wall_lean_update` writes | → `begin_move`, `turned_about_eyes`, `walk`, `set_catching_up`, `set_wall_lean` |
| `link_arms` local `WorldMapping stepMap` + `gameplay_head` | → `HeadController::step_head` (the same head, at the step) |
| `CameraOverride` third person / events / menus | → `begin_game_camera`, `game_camera_view`, `end_game_camera` (explicit mode transitions: first person again recentres, as before) |
| smooth, snap and rig turns | → `turn`, `set_rig_turn`, `advance_rig_turn` |
| `ViewHistory g_viewHist`, `blur_head_turn` | → `begin_frame`, `drawn`, `blur_reprojection` |
| tracking summary counters | → `HeadStats` |
| removed globals | `g_map` (now an alias), `g_room` (alias), `g_calibrated`, `g_recenter`, `g_calibratedLink`, `g_turnTotal`, `g_rigTurn`, `g_eventActive`, `g_eventYaw`, `g_wallLean`, `g_backHeld`, `g_roomWalks/Short/Wanted/Taken`, `g_viewHist` |
| kept (not the head) | sword fallback mapping (a controller point into the game: a reader), `g_link`, `g_body`, hands, arms |

Disabled or removed execution paths: none were needed. No second head calculation was running; the old functions are
now the controller's methods.

## 5. Behaviour

The refactor keeps the maths exactly. `test_head_controller` pins the controller against `gameplay_head`, the
transform the headset approved: equal position and orientation for a lean with roll **[test]**.

One deliberate difference: with `WWHD_VR_TEST_PITCH` (a desk test aid only), the wall limit now applies too.

Nothing changes in OpenXR timing, stereo, frame pacing or locomotion beyond the camera audit's fixes, already in `main`.

## 6. Checks

**[test] `vr_test` `test_head_controller`:**
- read-only ownership (`static_assert`);
- recentre from any room position and yaw;
- equivalence with `gameplay_head` (lean with roll, height offset, lean cap);
- recentre requests and calibration per Link;
- turns and their total, wrapped;
- the rig turn only while set, never counted as a stick turn;
- the room-scale walk applied exactly once, also half way between steps;
- a wall stopping half a walk (the origin takes half, the view stays at the head; stats);
- true 60's same-step move and a new Link;
- the about-eyes turn reported once;
- the catch-up log edge;
- the walls' lean limit and its reset;
- the lean-back hold and its count;
- `step_head` equal to the step's mapping without changing it;
- game-camera mode: the head's turn added since its start, position the game's, transitions;
- view history by display time (no frame before, a second pass of a frame, a frame not drawn from the head).

The earlier camera, stereo and blur tests all still pass (`test_camera_audit`, `test_gameplay_camera_regression`,
`test_conversion`, `test_screen_blur`).

**[build]:**
- `build/game` (Windows VR, Release) builds `wwhd.exe` and `wwhd-vr-launcher.exe` with no new warnings.
- The full test suite was run; see section 8.
- Linux, Steam Frame Lepton and Native can't be built on this machine (no Linux compiler, Android NDK or arm64
  toolchain). The change is portable C++ in the same files.

**Threads [code]:** every controller call is made with `g_mutex` held, as the globals it replaces were. No lock was
added.

**Performance [code]:** no heap allocations or locks were added. The same arithmetic runs; methods are inline in the
header. No reliable measurement without the headset; expected change: none.

## 7. Headset test plan

There's nothing new to feel: the behaviour is meant to be identical to the build tested at 15:44 to 15:48.

Run the camera audit's checklist (`docs/VR_CAMERA_AUDIT.md`, section 18) once, at Capped 90
(`WWHD_VR_FRAME_RATE=full`), and look for any difference from before. The ones that exercise every moved path:

| # | Do | Expect | Failure looks like | Logs |
|---|---|---|---|---|
| R1 | recentre (both stick clicks), then look and lean around | head at Link's eyes, facing his way | view off his body, wrong facing | `[vr] recentred: heading ...` |
| R2 | walk in the room 1 m, then lean into a wall | Link follows under you; the view stops short of the wall | the world sliding, seeing through the wall | `body under the head: ... not all taken` |
| R3 | smooth-turn and snap-turn while leaning | as before, the body under the view | the body sliding, jumps | `smooth turn: ... a frame` |
| R4 | lock onto something; climb a ladder | the view turns smoothly to the target or wall | stepping, no turn | `lock-on: ...`, `ladder/ledge: ...` |
| R5 | right-stick click to third person and back; a cutscene; the title | game camera with head turns; first person re-anchors at your head | wrong facing after returning | `first person again: ...` |
| R6 | lean back far | held about 8 cm behind his eyes | seeing his headless body | `views held at his back N` |
| R7 | Swift Sail blur with Disable screen blur unchecked | trail with the world | doubling with head turns | `screen blur: the last frame moved ...` |
| R8 | both eyes on a near object | steady stereo | double vision | — |

Not checked without the headset: everything in section 18 of the audit, and that it feels the same as the tested
build.

## 8. Results

- **Builds [build]:**
  - `vr_test`, `wwhd.exe` and `wwhd-vr-launcher.exe` were built in `build/game` (Windows VR, Release).
  - `vr_test` passes, including `test_head_controller`.
  - The full test suite was started but stopped before it ran, at the user's request (headset feedback first).
- **Headset, run 1 (2026-10-30 16:16) [headset]:** ghosting and blur when moving or turning the head. The cause was
  not the refactor:
  - SteamVR had been switched to 120 Hz, and the test launch's `WWHD_VR_FRAME_RATE=full` capped the frame rate at
    120 fps, which the game can't hold (79.5 fps average).
  - Each missed refresh (250 to 340 every 10 s) showed an old frame again, moved to the head's pose.
- **Headset, run 2 (16:23, SteamVR back at 90 Hz, `vr.refreshRate=90`) [headset]:**
  - 87.7 fps average, 81% of the time at 90 or above;
  - 0 to 14 missed refreshes every 10 s;
  - no crash. The user: "Good".

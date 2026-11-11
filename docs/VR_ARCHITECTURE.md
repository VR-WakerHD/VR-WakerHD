# VR mode: architecture

VR mode plays the recompiled game in a PC VR headset through OpenXR: a head-tracked first-person camera,
stereo rendering of the real game world from both eyes, motion controllers as the GamePad, and the sword
swung with the controller and hitting through the game's own attack collision. It is optional twice: the
build option `WWHD_VR` (on by default for Vulkan builds on Windows and Linux) and the setting `vr.enabled`
(off by default; `--vr` / `--no-vr` / `WWHD_VR=1|0` override it for one start). Without a loader, runtime or
headset the game starts on the desktop exactly as before and logs why.

Build, controls and test procedures: [VR_BUILD_WINDOWS.md](VR_BUILD_WINDOWS.md),
[VR_BUILD_LINUX.md](VR_BUILD_LINUX.md), [VR_CONTROLS.md](VR_CONTROLS.md), [VR_TESTING.md](VR_TESTING.md).

## 1. What the audit found

| Area | In this repository | Consequence for VR |
|---|---|---|
| Game code | PowerPC code statically recompiled to C on the player's machine (`tools/recomp`, `build/gen`, never in the repository); runtime hooks replace functions (`tools/recomp/hooks*.txt`: `hook_ADDR` / `f_ADDR_orig`) or run before instructions (`@ADDR` → `site_ADDR`) | VR never edits generated code. It uses existing hooks and adds one (`hooks_vr.txt`), which the setup compiles into the player's game like every other hook |
| Structures | The decompilation (`decomp/` submodule, ZeldaWWHDDecomp/wwhd) and the guest SDK headers (`runtime/guest/include/wwhd`) give HD offsets | Every offset VR uses is listed at the top of `runtime/src/vr/vr_game.cpp` with its source |
| Graphics | GX2 implemented natively on **Vulkan** (all platforms) and Metal (macOS); the Vulkan renderer loads `vulkan-1` at run time (`gfx/vulkan/loader.h`) | OpenXR binds to the existing Vulkan renderer (`XR_KHR_vulkan_enable2`); no new renderer |
| Stereo | An existing stereo-3D mode (`gfx/stereo.h`, `gfx/vulkan/stereo.cpp`) draws the **whole frame twice**, eye 1 replaying eye 0's command stream with its own render targets, shifting only perspective projections in uniforms (`stereo_projection.h`), with fixes for the sun-shadow reconstruction and shared light atlas | VR is a new stereo mode `stereo::XR`: true per-eye rendering of the actual world, not a duplicated image |
| Camera | `camera_draw` (024FFC40) builds the view from eye/center/up at camera+0xDC/0xE8/0xF4; `interp.cpp` already wraps it to blend 30 Hz cameras for 60/120/240 fps | VR replaces eye/center/up only around the original call (after the frame blending), then restores the game's state, so the game's camera logic never sees the head |
| Projection / culling | `aspect.cpp` hooks every projection and the culling frustum (`C_MTXPerspective`, `sead::PerspectiveProjection`, `mDoLib_clipper::setup`) and reshapes screen-sized render targets | The headset's field of view goes through the same hooks: picture, culling and render targets stay consistent |
| Input | Host input is a virtual Wii U GamePad (`input::PadState`); mods filter it at the end of `input::read()` (`mods::filter_pad`) | Controllers become GamePad buttons and sticks there; the game's movement code moves Link |
| Link | `daPy_lk_c` (Link) executes in `daPy_Execute` (0240EBB0, already hooked by the camera mod); its sword attack is three capsules `mAtCps[3]` that `setSwordAtCollision` spans between the blade's previous and current positions; `dCcS::Move` checks all colliders in the scene's draw, after every actor executed | The tracked blade replaces the capsules' geometry while the game's attack is active: damage, invulnerability, reactions, sounds and cooldowns stay the game's |
| Link's model | `daPy_lk_c::draw` hides face, hair and hat and keeps arms and legs when the camera attention status `0x20` is set (the game's own "camera very close" presentation) | First person uses that presentation; no new body model |

## 2. Modules

```text
runtime/src/vr/
  vr_math.h          vectors, quaternions, poses; OpenXR space <-> game world; projections; filters
  vr_config.h        vr.* settings: parse, clamp, validate, defaults
  vr_bindings.h      OpenXR actions -> GamePad buttons (vr.bind.*), recentre gesture
  vr_locomotion.h    stick remapping to the head's heading, snap / smooth turning, head offset limit
  vr_ik.h            two-bone arm IK with reach limits, shoulder and elbow-pole placement, body yaw follow
  vr_swing.h         blade from grip pose, swing detection (velocity, phases, classification),
                     swept capsules, per-swing hit ledger
  openxr_session.*   loader (opened at run time), instance, system, Vulkan binding, spaces, swapchains,
                     action sets and profiles, haptics, session states (RAII)
  vr.h / vr.cpp      shared state: settings, status, the views ring, eye offsets, the field of view
  vr_game.*          game thread: camera override, Link anchor, input, sword adapter, first-person draw
runtime/src/gfx/vulkan/vr_vulkan.*   render thread: instance/device through OpenXR, frame loop, eye copies
runtime/src/mods/vr_hooks.cpp        hook_023DBBE4 (daPy_Draw), defined in every build
tools/recomp/hooks_vr.txt            the hook list entry for daPy_Draw
runtime/tools/vr_test.cpp            headset-free unit tests (ctest: vr)
```

The headers (`vr_math`, `vr_config`, `vr_bindings`, `vr_locomotion`, `vr_ik`, `vr_swing`) have no
dependency on OpenXR, Vulkan or the game; all VR calculations are unit-tested without a headset.

## 3. Frame flow

```text
game thread                                   render thread
-----------                                   -------------
input::read -> vr::game::filter_pad           swap() (OP_SWAP of frame N-1)
  xrSyncActions, controller poses               vrvk::frame_begin: xrPollEvent, xrWaitFrame, xrBeginFrame,
  swing detection -> B for 2 logic steps          locate views -> headset FOV + eye offsets (vr.h)
  left stick turned to the head's heading         stereo::set_xr(eye offsets)
  right stick -> snap/smooth turn               present(TV):
daPy_Execute (logic step)                         window picture (left eye)
  Link's position / eye height sampled            vrvk::record_eyes: TV scan -> eye 0 image,
  attack active: mAtCps <- tracked sweep            stereoRight scan -> eye 1 image (same command buffer)
camera_draw (every frame, after interp blend)    submit (semaphores: the batch is on the queue)
  xrLocateViews(predicted time + 1 frame)       vrvk::frame_end: release images, xrEndFrame with the
  eye/center/up <- head in the game world          views the frame was drawn with (by id)
  views stored -> id for this frame             stereo_latch: eye offsets for the next frame
daPy_Draw: head hidden, sword at the hand
GX2SwapScanBuffers: OP_SWAP {..., views id}
```

- **Exact poses.** Each frame is submitted with the views it was rendered with: the id of its views travels
  as the 7th word of its `OP_SWAP` (`gx2_core.cpp`). The runtime's reprojection then corrects only the
  latency between the predicted and the actual pose. Frames drawn without the game camera (title, menus,
  loading) are submitted at the head's predicted pose (head-locked).
- **Parallel eyes.** `stereo::XR` renders both eyes with the head's orientation, the smallest symmetric field
  of view containing both eyes (`symmetric_union`) and each eye moved sideways by its measured offset
  (`as_rendered`). Rendering a symmetric frustum keeps every shader the existing stereo mode already fixes
  (sun shadows, depth reconstruction) correct, at the cost of some pixels outside each eye's own field of
  view. Canted displays (a few wide-FOV headsets) are drawn parallel and reprojected by the runtime.
- **Game frame vs. headset rate.** The game keeps its own timing (30 Hz logic; 40/60/120/240 fps output with
  the existing frame interpolation). The compositor reprojects when the game delivers fewer frames than the
  headset shows; choose the interpolation rate at or near the headset's refresh rate (Graphics menu).

## 4. Coordinate spaces

| Space | Units | Axes | Source |
|---|---|---|---|
| OpenXR tracking (STAGE, else LOCAL) | metres | +Y up, -Z forward | runtime |
| game world | game units (`vr.unitsPerMetre`, default 100: about 1 cm) | +Y up; heading 0 faces +Z, heading a faces (sin a, 0, cos a) | game |
| game view | game units | as OpenXR (+X right, -Z forward) | `camera_draw` |

`WorldMapping` (vr_math.h): game = anchor + Ry(heading + pi) * (xr - origin) * unitsPerMetre. The anchor is
Link's position (blended between logic steps like his drawn model) plus his eye height, filtered to remove
the bob of his walk animation while still following crawling, swimming and climbing. Recentring stores the
current head position as `origin` and turns `heading` so the head looks where Link faces. Snap / smooth turns
change `heading`. Leaning or walking physically moves the view up to `vr.maxHeadOffset` from Link; beyond
that the view is held at the limit (the body never drifts away from Link's collision cylinder).

## 5. Camera modes

1. **VR first person** (default with VR): during normal play the camera is the head at Link's eyes.
2. **Desktop third person**: the original camera, untouched, whenever VR is off.
3. **Events / title / file select** (`aspect::gameplay_camera()` false): the game's camera position and
   direction, turned by the head relative to where it looked when the event began (`vr.cutsceneFollow`,
   default on); off: the game's camera as on a flat screen (head-locked). Camera cuts stay cuts.

## 6. Sword and combat

- **Blade.** The sword hand's grip pose plus `vr.gripOffset*`, `vr.gripPitchDegrees`; the blade's length in
  the game is the game's own (distance between its sword root and tip, so the Master Sword's reach and the
  game's balance are kept).
- **Swing detection** in tracking space (artificial locomotion and turns never count): a swing starts when
  the tip's speed stays above `vr.swingStartSpeed` for `vr.swingMinTime`, ends below `vr.swingEndSpeed`
  (hysteresis), then `vr.swingCooldown` must pass; frame steps longer than 0.1 s are not measured (hitches,
  loads, a teleported blade).
- **Attack adapter.** A swing presses B for two logic steps: the game starts the cut its state machine
  allows (combos, cooldowns, recovery, animations, sounds, Link's state checks: nothing is bypassed).
- **Collision.** While the game has the sword attack set (bit 0 of `mAtCps[0]`'s attack parameters), after
  Link's execute the three capsules are moved to the tracked sweep: [0] the blade now, [1] the tip's path,
  [2] root to previous tip — the arrangement `setSwordAtCollision` uses, covering the swept area so fast
  swings cannot pass through a target between frames. Their radius, attack type, power, hit groups and the
  game's hit validation (`dCcS`: groups, masks, invulnerability, shields, each target's own damage logic)
  are unchanged. The sword tip/root fields effects and sounds use follow too.
- **Hits.** Each target that reports a hit vibrates the sword hand once per swing (`HitLedger`).
- **Spin attack, other weapons.** The spin attack uses the game's cylinder collider (not moved). Picked-up
  weapons (Bokoblin sticks, Moblin spears) use the same capsules, so they follow the controller too. The Skull
  Hammer is held and swung as a tracked weapon, its attacks the game's own (docs/VR_SKULL_HAMMER.md).

## 7. First-person body and arms

- Link's draw runs with the game's close-camera presentation: face, hair and hat hidden; arms and legs
  drawn (`vr.showBody` draws everything). Reflections, shadows and cutscenes keep the full model (the flag
  is only set around his draw during normal play).
- The sword model is drawn at the tracked hand (`vr.trackedSword`): its joint matrices are moved rigidly from
  where the animation put them (no game function is called).
- **Arms (not done).** `vr_ik.h` has a tested two-bone solver with reach clamping, shoulder placement and
  elbow poles, but it is not applied to Link's skinned arms yet: his arms still follow his animation.
  Writing the solved shoulder/elbow/wrist into his joint matrices also needs the skin's envelope matrices
  recalculated (J3D weight envelopes are computed in `J3DModel::calc`), which must be measured in the game.

## 8. Threads and lifetime

- The render thread owns the OpenXR session: creation after the Vulkan device, frame loop, swapchains,
  events, teardown (`vrvk::stop`: end the frame, idle the device, destroy swapchains, spaces, session,
  instance; the loader stays loaded). Session loss, the runtime exiting or the headset going away switch the
  renderer back to mono (`stereo::set_xr(false)`) and the game continues on the desktop.
- The game thread only locates views, syncs actions and applies haptics (`Session::trackingMutex_` keeps
  them away from teardown).
- The OpenXR runtime uses the graphics queue in `xrReleaseSwapchainImage` / `xrEndFrame`: those run under
  the renderer's `graphics_queue_mutex()`.

## 9. Plan and status

| Phase | Content | Status |
|---|---|---|
| 1 Audit | this document | done |
| 2 Baseline | desktop build and tests with the release toolchain (llvm-mingw) | done (Windows); see VR_TESTING.md |
| 3 OpenXR foundation | loader, instance, Vulkan binding, spaces, swapchains, frame loop, fallback | implemented; frame loop **verified on a headset** (SteamVR, PS VR2) by the VR smoke test |
| 4 Head tracking | camera override, recentring, turning, event mode | implemented; needs the game (not verified) |
| 5 Controllers | actions, profiles, bindings, haptics | implemented; controller tracking through the Touch profile **verified on a headset** (PS VR2 Sense), game mapping not verified |
| 6 Arms / IK | solver and tests | solver done; not applied to Link's skin (see 7) |
| 7 Sword / combat | swing detection, sweep, attack adapter, tracked sword model | implemented; needs the game (not verified) |
| 8 Compatibility | events, aiming items, menus | first pass (aiming passes the right stick); needs play-testing |
| 9 Performance | — | not measured |
| 10 Release | docs, Linux build | Windows built here; Linux documented, not built here |

## 10. Known limitations

- Not yet played: everything that needs the game (camera, movement, sword, body) is untested; the person
  building it needs their own dump. Feature flags allow turning parts off (`vr.trackedSword`, `vr.showBody`,
  `vr.cutsceneFollow`, `vr.enabled`).
- Arms follow Link's animation, not the controllers (see 7).
- Sword hits are not blocked by walls beyond what the game's own sword does (the original game has no line
  of sight test for the sword either).
- The HUD is drawn into both eyes at zero disparity across the field of view (the layouts are head-locked);
  a quad layer for the HUD and menus would read better.
- Items aimed in first person (bow, hookshot, boomerang, telescope) still aim with the right stick.
- Only the main camera is replaced; picture-in-picture views of the game (GamePad screen) stay flat.
- Interaction profiles without a menu button (Valve Index) need a binding in the runtime's binding UI for
  the pause menu.

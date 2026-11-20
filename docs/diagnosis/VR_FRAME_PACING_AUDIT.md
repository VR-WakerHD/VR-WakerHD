# VR frame pacing audit (2026-11-03)

Code analysis of the OpenXR frame loop, Vulkan synchronization and threads, at commit `8458abfe`. Nothing here was
measured in a headset for this audit; the measurements quoted come from player logs read earlier the same day.

## The frame loop, as the code runs it

| Step | Thread | Where | What happens |
|---|---|---|---|
| 1 | render | `vr_vulkan.cpp` `frame_begin` | waits for the end-frame thread's last job (`end_thread::wait`), so each `xrEndFrame` comes before the next `xrWaitFrame` (OpenXR's order) |
| 2 | render | `openxr_session.cpp` `begin_frame` | `xrWaitFrame` (the runtime's pacing), then `xrBeginFrame`; keeps `predictedDisplayTime` and `predictedDisplayPeriod` |
| 3 | render | `frame_begin` | `xrLocateViews` at that predicted time: the eyes' field of view, frustums and offsets for the frames drawn from now on |
| 4 | game | `vr_game.cpp` `CameraOverride` | the camera of each drawn frame (logic step or in-between pass) from the head located for its display time, predicted one frame ahead (`[vr] camera pose predicted ahead`); stored under a views id |
| 5 | render, record worker | `draw.cpp`, `pod_commands.cpp` | the frame's draws recorded and replayed into command buffers; submissions under `graphics_queue_mutex` |
| 6 | render | `vrvk::record_eyes` | eye images acquired (`xrAcquireSwapchainImage`, `xrWaitSwapchainImage` with a 100 ms timeout), the eye pictures copied or upscaled into them |
| 7 | render | `frame_end` | the views **the frame was drawn with** (`vr::views(g_frameViews)`) packed into a job with the quads |
| 8 | end-frame | `end_thread::end` | under `graphics_queue_mutex`: images released, `xrEndFrame` with `displayTime` = the frame's predicted time |
| - | render | `headset_frame` | only while the game has no frame for the longer of 3 periods and twice its recent frame interval: the last frame shown again (same images, same poses) |

## Findings

**Correct, kept as is**
- Each frame is submitted with the exact poses it was drawn with (step 7), so the runtime reprojects a consistent
  image; a late or jumpy prediction costs latency, not jitter.
- `xrEndFrame` on its own thread keeps OpenXR's order (step 1) and holds the queue lock the runtime needs.
- No queue-wide or device-wide GPU wait in the frame path: desktop presentation is asynchronous by default
  (`WWHD_VK_ASYNC_PRESENT`); `vkDeviceWaitIdle` and `vkQueueWaitIdle` remain only at session stop, swapchain
  resize/recreation and capture.
- Swapchain images are held only from acquire (step 6, late in the frame) to release (step 8).
- Overlay swapchains made mid-session take the queue lock (`c178e1ac`).
- Unmatched ends of rendering are dropped on every platform (`849bf1f9`).

**Problems found (game side), both fixed in `7102cf4c` for streamed headsets only**
1. **The frame-timing shift** (`note_display`): after three frames whose display times look a refresh late within
   2 s, the game moved its frames half a refresh later, at most every 5 s, and each move is a hitch. A streamed
   runtime's predicted display times move with the network (a Quest 3 through VDXR: up to 155.6 ms apart), so the
   rule fired on network jitter, not on the game. The code's own comment records the hitch.
2. **Auto frame rate flapping** (`interp_pacing.h` `auto_rate_step`): after 10 s at half rate it tries full rate
   again (doubling to 40 s on failure). In the Quest log it went between 90 and 45 again and again, a change of
   smoothness each time, against Virtual Desktop's own half-rate smoothing (SSW).

**Not problems, measured**
- Frames shown again by the game (`headset_frame`): 0 to 1 per 10 s in the Quest VDXR log.
- Render thread waiting for the end-frame thread: 0 frames in 300 to 600 (`[vr] end-frame thread` lines).

**Left open (needs hardware)**
- Whether VDXR uses submitted depth for its reprojection: depth is off for a Quest on a PC under Auto since
  `bb2fd65f` (and `vr.frameSynthesis=on` sends it). An A/B in a Quest run decides it.
- The game's logic runs at 30 Hz; at 45 fps a step is drawn 1.5 frames on average (1 and 2 in turn), so animation
  (not the head) moves unevenly at 45. At 60 on 120 Hz it is even. This is inherent to the 30 Hz rule
  (never changed) and the 45 fps choice.

## Threads

| Thread | Priority | Waits on |
|---|---|---|
| game (guest) | game | the render thread at DrawDone (skipped when nothing it protects is queued) |
| render | boosted | `xrWaitFrame`; the end-frame thread at `frame_begin`; the queue lock |
| record worker | boosted | the render thread's recorded commands; the queue lock to submit |
| end-frame | boosted | the queue lock; the runtime in release and `xrEndFrame` |
| scene threads (S4, S6) | boosted | joins with the render thread |

The end-frame thread holds the queue lock for milliseconds a frame, so no other thread takes it every frame (the
overlay swapchain lock is taken only while a swapchain is made).

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The Vulkan renderer's side of VR mode (vr/vr.h, docs/VR_ARCHITECTURE.md).
//
// With VR wanted, the OpenXR runtime makes the Vulkan instance and device (XR_KHR_vulkan_enable2: it adds
// the extensions it needs and names the GPU the headset is on), the session runs on the renderer's graphics
// queue, and each frame both eyes' finished pictures (the TV scan buffer for the left eye, stereoRight's for
// the right) are copied into the headset's swapchain images inside the TV window's present, after the
// window's own picture: the same command buffer, so the copies follow the frame's rendering on the queue.
// Without VR (not built, not wanted, no runtime or headset) every function falls through to the renderer's
// normal behaviour.
#pragma once
#include <vector>
#include "backend.h"

namespace gfxvk::vrvk {
// render thread, in the renderer's start-up order
VkResult create_instance(const VkInstanceCreateInfo& ci, VkInstance* out);
// the index in `devices` of the GPU the headset needs (-1: no preference)
int preferred_device(const std::vector<VkPhysicalDevice>& devices);
VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& ci, VkDevice* out);
void device_ready();  // R.device and R.queue exist: the session starts
// every frame (render thread)
void frame_begin();                    // swap(), before the TV present
void record_eyes(VkCommandBuffer cmd); // the TV present, after the window's picture
void frame_end();                      // swap(), after the TV present (its work is submitted)
// a VR frame begun whose eyes the TV present did not record: Android in a headset has no window surface to present
// (docs/VR_ANDROID.md); swap() then records and submits them itself before frame_end
bool eyes_pending();
bool skip_desktop_gamepad();           // swap(): a VR session runs and the GamePad window is not wanted beside it
bool skip_window_tv();                 // the TV present (Android): a VR session runs; the app's window is not seen
// render thread, between frames: the headset's last frame shown again while the game has no new one ready (a shader
// or pipeline compiled the first time it is needed, a stage loading). When the next one is due (steady clock ns, 0:
// not needed: no session, a frame under way), and the frame itself (nothing when not due or the queue is busy)
int64_t headset_frame_due();
void headset_frame();
void shutdown();                       // renderer teardown (device idle)
// --renderer-smoke with VR wanted (WWHD_VR=1 or --vr): `frames` frames through the whole OpenXR frame loop on
// the headset (wait/begin, acquire, a GPU clear of each eye's image, release, a projection layer), controller
// tracking and a short vibration. 0 = passed or VR not running (nothing to test), 1 = failed.
int smoke(int frames);
}  // namespace gfxvk::vrvk

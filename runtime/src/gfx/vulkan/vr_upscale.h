// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The SGSR1 pass (docs/VR_UPSCALING.md, vr/vr_upscale.h): an eye's picture upscaled into the headset's swapchain image
// by Qualcomm's Snapdragon Game Super Resolution 1, in place of the linear blit. Render thread only.
#pragma once
#include "backend.h"

namespace gfxvk::vr_upscale {

// whether the pass can be drawn into images of this format here (dynamic rendering, the source sampleable)
bool available(const Surface& source, VkFormat target);
// `source`'s region `shown` (from its top left) drawn over the whole of `target` (a swapchain image in
// COLOR_ATTACHMENT_OPTIMAL, left so); false: nothing recorded (the caller blits)
bool draw(VkCommandBuffer cmd, Surface& source, VkExtent2D shown, VkImage target, VkFormat format, VkExtent2D extent,
          float sharpness);
// the swapchain images' views, before the session's images go (with the panel's)
void forget_views();
// (wwhd --renderer-smoke) the pass drawn into small UNORM and sRGB images and read back: a flat picture comes out
// unchanged (its pixels within 2/255), at any scale; raises on a mismatch
void self_test();

}  // namespace gfxvk::vr_upscale

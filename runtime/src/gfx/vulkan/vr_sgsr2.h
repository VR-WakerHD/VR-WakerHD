// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// SGSR2 (docs/VR_UPSCALING.md, vr/vr_upscale.h): an eye's picture upscaled into the headset's swapchain image by
// Qualcomm's temporal Snapdragon Game Super Resolution 2 (its 2-pass fragment variant), with the eye's own history and
// the motion pass's motion vectors and scene depth. Render thread only.
#pragma once
#include "backend.h"

namespace gfxvk::vr_sgsr2 {

// the eye's picture (its region `shown`) into `target` (a swapchain image in COLOR_ATTACHMENT_OPTIMAL, left so); false:
// nothing recorded (no motion this frame for the eye, sizes that do not match, a failure): the caller uses SGSR1 or the
// blit, and the eye's history starts again next time
bool draw(VkCommandBuffer cmd, int eye, Surface& picture, VkExtent2D shown, VkImage target, VkFormat format,
          VkExtent2D extent);
// the eyes' histories and images, before the session's images go
void forget();

}  // namespace gfxvk::vr_sgsr2

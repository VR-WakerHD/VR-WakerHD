// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// FSR 1 (docs/VR_UPSCALING.md, vr/vr_upscale.h): an eye's picture upscaled into the headset's swapchain image by AMD
// FidelityFX Super Resolution 1: EASU (edge-adaptive spatial upsampling) into an intermediate image of the output's
// size, then RCAS (robust contrast-adaptive sharpening) into the swapchain image. Upscaling only: FSR has no frame
// generation here. Render thread only.
#pragma once
#include "backend.h"

namespace gfxvk::vr_fsr1 {

// `picture`'s region `shown` (from its top left) over the whole of `target` (a swapchain image in
// COLOR_ATTACHMENT_OPTIMAL, left so). sharpnessStops: RCAS's reduction of sharpness in stops (0 the sharpest, AMD's
// samples 0.2). False: nothing recorded (the caller uses SGSR1 or the blit)
bool draw(VkCommandBuffer cmd, int eye, Surface& picture, VkExtent2D shown, VkImage target, VkFormat format,
          VkExtent2D extent, float sharpnessStops);
// the eyes' intermediate images and the swapchain images' views, before the session's images go
void forget();

}  // namespace gfxvk::vr_fsr1

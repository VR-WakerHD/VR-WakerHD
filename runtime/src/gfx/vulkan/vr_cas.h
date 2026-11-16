// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The CAS pass (docs/VR_UPSCALING.md, vr/vr_upscale.h Mode::Cas): an eye's picture upscaled into the headset's
// swapchain image with contrast adaptive sharpening (after AMD FidelityFX CAS), once a game frame, in place of the
// compositor's Super Resolution at every refresh: the Quest's default (2026-11-11). Render thread only.
#pragma once
#include "backend.h"

namespace gfxvk::vr_cas {

// whether the pass can be drawn into images of this format here (dynamic rendering, the source sampleable)
bool available(const Surface& source, VkFormat target);
// `source`'s region `shown` (from its top left) drawn over the whole of `target` (a swapchain image in
// COLOR_ATTACHMENT_OPTIMAL, left so), sharpened by `sharpness` 0..1, its edges faded to black over `edge` of the
// picture (a narrowed field of view; 0: none), its edges smoothed first with `fxaa` (FXAA in the same pass, the
// sharpening kept off the edges it smoothed); false: nothing recorded (the caller blits)
bool draw(VkCommandBuffer cmd, Surface& source, VkExtent2D shown, VkImage target, VkFormat format, VkExtent2D extent,
          float sharpness, float edge = 0.0f, bool fxaa = false);
// the swapchain images' views, before the session's images go (with the panel's)
void forget_views();

}  // namespace gfxvk::vr_cas

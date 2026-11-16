// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
//
// Fixed foveated rendering on the eye-sized two-eye (multiview) passes: a shading rate map (foveation.cpp,
// docs/VR_FOVEATION.md).
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace gfxvk {
// the level in use: 0 off, 1 low, 2 medium, 3 high (vr.foveation, or WWHD_VK_FOVEATION_LEVEL); 0 without the device's
// support (R.fsr) or outside a VR session
int foveation_level();
const char* foveation_level_name(int level);
// the shading rate map for a two-eye pass of this render size, or null: off, not the eye picture's size, or not ready
// yet (made on the render thread, used from the next frame on)
VkImageView foveation_map(uint32_t width, uint32_t height);
}  // namespace gfxvk

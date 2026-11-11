// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Frame synthesis and depth for the OpenXR runtime (docs/VR_FRAME_SYNTHESIS.md, vr/vr_framesynth.h): each eye's
// motion vectors (from the motion pass) and depth (the scene's) written into the session's motion vector and depth
// swapchain images at the eye copy, and the frame's near/far planes. Render thread, except note_camera (draw threads).
#pragma once
#include <cstddef>
#include <cstdint>

#include "backend.h"

namespace gfxvk::vr_framesynth {

// the session has frame synthesis or depth swapchains this run (set when the VR session's swapchains exist)
void set_active(bool on);
bool active();
// a scene draw's vertex block with a camera projection (draw.cpp bind_stage, stereo): its planes and the depth target
// it draws into, for the eye (any drawing thread)
void note_camera(int eye, const uint32_t* regs, const unsigned char* block, size_t size);
// at the eye copy (vr_vulkan.cpp record_eyes): both eyes' images written where their data exists; the session told
// what the next xrEndFrame carries. Releases are the caller's (release_images, before the frame ends)
void record(VkCommandBuffer cmd);
// the session's swapchain images acquired by record, released (vr_vulkan.cpp frame_end, before xrEndFrame)
void release_images();
// the swapchain images' views, before the session's images go
void forget_views();
// (the settings' status line, the log) what the last frames carried
const char* status();
// the last frame carried both eyes' motion vectors (the runtime synthesizes; the game draws at half the refresh rate)
bool carrying();

}  // namespace gfxvk::vr_framesynth

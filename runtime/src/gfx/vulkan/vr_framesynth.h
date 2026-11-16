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
// (frame generation, vr_genframe) an eye's scene depth target (layer `slice`) and the camera projection's p[10], p[11]
// (projection units) and depth range, as the last record() found them; false: none this frame
bool last_depth(int eye, Surface*& depth, uint32_t& slice, float& projA, float& projB, bool& halfZ);
// (frame generation) the tangents of the field of view the eye's picture was drawn with, from the game's projection (left,
// right negative/positive, up positive, down negative); false: not known this frame
bool last_tangents(int eye, float& left, float& right, float& up, float& down);
// the session's swapchain images acquired by record, released (vr_vulkan.cpp frame_end, before xrEndFrame)
void release_images();
// the swapchain images' views, before the session's images go
void forget_views();
// (the settings' status line, the log) what the last frames carried
const char* status();
// the last frame carried both eyes' motion vectors (the runtime synthesizes; the game draws at half the refresh rate)
bool carrying();
// (2026-11-13, asked for: "make sure that frame synthesis doesn't raise it") frame synthesis held: the player's frame
// rate is under half the refresh rate (Capped 30 at 90 or 120 Hz). The runtime only synthesizes one frame between two
// (the game at exactly half: 36 at 72 Hz, 45 at 90), so using it raised a chosen 30 to 45 (a Quest 3 player's log: 45
// forced in every scene, about 24 fps drawn, the GPU full). Held, the frames carry nothing for it, as with it off
// (2026-11-14) also held in Auto Mode while the game runs under half the refresh rate (vr_vulkan.cpp
// update_auto_synth_hold): refresh filling fills the refreshes instead. `why`: the log's reason (on a change)
void set_held(bool held, const char* why = nullptr);
bool held();

}  // namespace gfxvk::vr_framesynth

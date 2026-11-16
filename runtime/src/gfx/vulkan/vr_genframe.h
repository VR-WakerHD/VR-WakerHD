// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR frame generation (docs/FRAME_INTERPOLATION_IMPLEMENTATION.md, "Generated frames"): an eye's in-between picture
// made on the GPU from the last real one, reprojected by its scene depth to the camera of the in-between refresh (the
// head where it will be, the view's own turn and movement since). Render thread only.
#pragma once
#include <vector>
#include <cstdint>

#include "backend.h"

namespace gfxvk {
struct Surface;
namespace vr_genframe {

// the warp of one eye: everything in the real eye camera's space (metres, -Z forward, +Y up)
struct Warp {
    float axisX[3], axisY[3], axisZ[3];  // the in-between camera's axes in the real camera's space
    float origin[3];                     // the in-between camera's position in the real camera's space
    float tanLeft, tanRight, tanUp, tanDown;  // the eye's field of view (the same for both cameras)
    float projA, projB;                       // the camera projection's p[10], p[11] (projB already in metres)
    bool halfZ = false;                       // depth 0..1 (else -1..1 stored as 0..1)
    // (the near field, 2026-11-12) what lies within nearMetres of the eye (the hands, the sword and shield, held items)
    // moves with you, not with the world: it is drawn by the head's own move since the real frame only (nearRotation,
    // a quaternion x y z w, and nearOrigin: that camera in the real camera's space), not the stick's turn or the walk;
    // the world it uncovered filled from the nearest world beside it. 0: none (every pixel warped as the world)
    float nearRotation[4] = {0, 0, 0, 1};
    float nearOrigin[3] = {0, 0, 0};
    float nearMetres = 0;
};

// the eye's in-between picture into the acquired swapchain image `target` (whole image): `color` the real eye picture
// (its top-left `shownWidth` x `shownHeight` part), `depth` its scene depth (layer `slice`). False: not drawn (a
// pipeline or view could not be made); the caller then shows the real frame again instead
bool draw(VkCommandBuffer cmd, Surface& color, uint32_t shownWidth, uint32_t shownHeight, Surface& depth, uint32_t slice,
          const Warp& warp, VkImage target, VkFormat format, uint32_t width, uint32_t height);
// (diagnostics, WWHD_VR_GENFRAME_CAPTURE=1) an acquired swapchain image read back as RGBA8, as the headset gets it
// (display-encoded bytes); waits for the GPU (a hitch: diagnostics only). False: not read
bool read_target(VkImage target, VkFormat format, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba);
// (diagnostics, the refresh sequence capture) rows [y0, y0 + rows) of an acquired swapchain image (COLOR_ATTACHMENT_OPTIMAL,
// and again after) copied into `buffer` (a readback buffer of width * rows * 4 bytes) in `cmd`, no wait
void copy_rows(VkCommandBuffer cmd, VkImage target, uint32_t width, uint32_t y0, uint32_t rows, VkBuffer buffer);
// (docs/VR_HANDS_LAYER_PLAN.md) the hands image `src` into the acquired swapchain image `target` (the same size, whole
// image): its covered pixels as they are, the transparent ones within 3 pixels of a covered one given that pixel's colour
// (still transparent), so a compositor filtering the layer's edge blends in the hands' colour, not black. False: not drawn
// linkDepth (his draws' depth: the eye's depth with him) and worldDepth (the eye's depth at the frame's end, without
// him): his pixels behind something the world drew after him are left out (both null: none left out)
// (docs/VR_HANDS_LAYER_PLAN.md, 2026-11-14: the Master Sword's white glow a thick black outline with steady hands on)
// Link's hands image keeps the game's own alpha (its draws blend as the game made them: some use the picture's alpha
// as a mask); where he is comes from depth in dilate instead (his depth nearer than the world's at the frame's end),
// and the layer is sent premultiplied: his covered pixels opaque, the rest of his image added onto the world (his
// glows over the background). WWHD_VR_HANDS_COVERAGE=0: alpha as coverage, unpremultiplied, as before
bool hands_coverage_from_depth();
// dstX: the part of the target written starts there (a shared image holding both eyes side by side)
bool dilate(VkCommandBuffer cmd, Surface& src, VkImage target, VkFormat format, uint32_t width, uint32_t height,
            Surface* linkDepth = nullptr, Surface* worldDepth = nullptr, uint32_t dstX = 0);
// its pipeline for swapchain images of `format` made (before images are acquired for it); false: generation cannot run
bool prepare(VkFormat format);
// the swapchain images' views made for it: dropped with the swapchains (vr_vulkan.cpp ensure_swapchain)
void forget_views();

}  // namespace vr_genframe
}  // namespace gfxvk

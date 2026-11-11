// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A shader's motion variant (docs/VR_DLSS.md, stage D2b): its translated GLSL, rewritten so a draw also writes each
// pixel's motion since the previous frame. Like the multiview variant (multiview_glsl.h) it leaves the translator's
// code as it is and changes declarations, plus a new main around the translator's:
//   vertex - every uniform block (the guest uniform buffers and the support block) becomes an array of the current
//     frame's data and the previous frame's (`#define uf_blockVS1 uf_blockVS1_blk[wwhd_mframe].uf_blockVS1_mo`), and
//     every vertex input gets a second input at location + 16 for the previous frame's vertex buffer
//     (`#define attrDataSem0 (wwhd_mframe == 1 ? attrDataSem0_prev : attrDataSem0_cur)`). The translator's main runs
//     twice: on the previous frame's data (its position kept), then on the current frame's, which leaves every output
//     as the original writes it. Both clip positions go to the pixel shader (locations 30 and 31).
//     With `views` 2 (a multiview draw) the arrays hold four: [frame * 2 + gl_ViewIndex]; eye-specific textures are
//     the multiview variant's business (rewrite that first with blocks off).
//   pixel - the two positions in, and `out vec2 wwhd_motion` at the given location: previous minus current position
//     in normalized device coordinates, halved (a fraction of the picture, + right and down as Vulkan's y). The
//     translator's main runs first; a discarded pixel writes no motion.
// In Vulkan each block array is one binding with two (or four) descriptors.
#pragma once
#include <cstdint>
#include <string>

namespace gfxvk::vk::motion {

constexpr uint32_t kPrevAttributeOffset = 16;          // previous frame's vertex inputs: location + 16
constexpr uint32_t kPrevClipLocation = 30, kCurClipLocation = 31;  // vertex to pixel

struct Variant {
    bool vertex = false;
    uint32_t views = 1;          // 2: a multiview draw (the arrays also per view)
    uint32_t blocks = 0;         // vertex: bit i, guest uniform buffer i (its data per frame)
    bool supportBlock = false;   // vertex: the support block (ufBlock)
    uint32_t motionLocation = 0; // pixel: the colour output location of the motion target
};
// the variant's GLSL; false (with why) if the source is not laid out as the translator writes it
bool rewrite(const std::string& glsl, const Variant& variant, std::string& out, std::string* why = nullptr);
// the motion pass's own pixel shader (docs/VR_DLSS.md, stage D2b: the motion pass): the two clip positions in, the
// motion out at location 0, as the pixel variant writes it. The game's pixel shader does not run in the motion pass:
// its depth test (equal to the scene's depth) keeps only the visible surface, cut-out pixels included
// Motion past kMaxMotion (a share of the picture in one frame), from a previous position behind the camera or not a
// number is written as zero: unknown, which DLSS handles better than a wrong vector
constexpr float kMaxMotion = 0.25f;
std::string motion_only_pixel();

}  // namespace gfxvk::vk::motion

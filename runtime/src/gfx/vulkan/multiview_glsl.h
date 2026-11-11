// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A shader's multiview variant (docs/VR_MULTIVIEW.md, stage M3): its translated GLSL, rewritten so that one draw serves
// both eyes. The translator's output is left as it is; the variant changes declarations only:
//   - an eye-dependent uniform block (a guest uniform buffer, `uniformBlockVS<i> { vec4 uf_blockVS<i>[n]; }`, or the
//     support block `ufBlock` of remapped registers) becomes a two-element block array, view 0's data and view 1's, and
//     its names become macros indexing it by gl_ViewIndex (`#define uf_blockVS0 uf_blockVS0_blk[gl_ViewIndex].uf_blockVS0_mv`);
//   - an eye-specific texture (an eye's own render target) becomes a two-element sampler array, each eye's image, its
//     name a macro the same way.
// Every use in the code stays as the translator wrote it. In Vulkan each array is one binding with two descriptors:
// the descriptor set writes view 0's and view 1's (M4). Indexing them by gl_ViewIndex needs the device's dynamic
// indexing of uniform buffer and sampled image arrays.
#pragma once
#include <cstdint>
#include <string>

namespace gfxvk::vk::multiview {

struct Variant {
    bool vertex = false;
    uint32_t blocks = 0;    // bit i: guest uniform buffer i is per view
    bool supportBlock = false;  // the support block (ufBlock) is per view
    uint32_t textures = 0;  // bit u: texture unit u is per view
};
// the variant's GLSL; false (with why) if the source is not laid out as the translator writes it
bool rewrite(const std::string& glsl, const Variant& variant, std::string& out, std::string* why = nullptr);

}  // namespace gfxvk::vk::multiview

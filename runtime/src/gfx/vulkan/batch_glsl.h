// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// A vertex shader's batched variant (issue #1, docs/VR_PERFORMANCE.md "Batched same-state runs"): its translated GLSL,
// rewritten so that one instanced draw draws a run of the guest's draws that differ only in the addresses of some of
// its uniform blocks (the grass: one draw per tuft, each with its own matrix block). Like the multiview variant
// (multiview_glsl.h) it changes declarations only:
//   - each batched block `uniformBlockVS<i> { vec4 uf_blockVS<i>[n]; }` becomes an array of the run's pieces, one per
//     instance, and its name a macro indexing it by gl_InstanceIndex
//     (`#define uf_blockVS1 uf_blockVS1_in[gl_InstanceIndex].v`);
//   - the support block `ufBlock` (registers and the guest blocks' entries the translator remapped into it) likewise,
//     when its entries come from a batched block: its members a struct, the block an array of it, each member's name
//     a macro (`#define uf_remappedVS ufBlock_in[gl_InstanceIndex].uf_remappedVS`);
//   - the guest's instance ID reads 0, as it did in each of the run's draws (one instance each).
// Every use in the code stays as the translator wrote it, so each instance computes exactly what its draw did.
#pragma once
#include <cstdint>
#include <string>

namespace gfxvk::vk::batch {

// pieces (instances) a draw of the variant may have at most, for any block
constexpr uint32_t kMaxPieces = 256;
struct Variant {
    uint32_t blocks = 0;               // bit i: guest uniform buffer i is per piece
    bool support = false;              // the support block is per piece
    uint32_t supportBytes = 0;         // its size (std140, a multiple of 16: a piece's stride)
    uint32_t pieces = 0;               // (out) the array size every batched block was given
    uint32_t vec4s[16] = {};           // (out) each batched block's size in the shader, in vec4s (a piece's stride)
};
// the variant's GLSL; false (with why) if the source is not laid out as the translator writes it.
// maxRange: the device's largest uniform buffer range (each batched block's pieces fit in one binding)
bool rewrite(const std::string& glsl, Variant& variant, uint32_t maxRange, std::string& out, std::string* why = nullptr);

}  // namespace gfxvk::vk::batch

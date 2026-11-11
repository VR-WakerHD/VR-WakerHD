// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// textureNeedsGuestLevels: which texture units of a pixel shader must keep the game's own texture when a
// replacement 4x larger is offered. A replacement is only right for a unit read with normalized coordinates and
// the hardware's level choice; texel reads, gathers, size/LOD queries and explicit-level samples see the guest's
// size or levels. Synthetic Latte programs only (no game data): one TEX clause with one instruction on unit U, then
// EXPORT_DONE of R0 to colour output 0.
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

std::unique_ptr<Renderer> g_renderer = std::make_unique<VulkanRenderer>();
void cemu_shim_log(const std::string& msg) { std::fprintf(stderr, "[decompiler] %s\n", msg.c_str()); }

namespace {
// one TEX instruction `opcode` on texture `unit` (sampler 0, dest/source R0), coordinates normalized or not
std::vector<uint32_t> program(uint32_t opcode, uint32_t unit, bool unnormalized) {
    std::vector<uint32_t> w;
    w.push_back(2);  // CF 0: TEX clause at 64-bit word 2, one instruction
    w.push_back((GPU7_CF_INST_TEX << 23) | ((1 - 1) << 10));
    w.push_back((0u << 15) | (0u << 13) | 0u);  // CF 1: EXPORT_DONE pixel output 0 from R0.xyzw
    w.push_back((GPU7_CF_INST_EXPORT_DONE << 23) | (1u << 21) | (0u << 0) | (1u << 3) | (2u << 6) | (3u << 9));
    w.push_back(opcode | (unit << 8) | (0u << 16));
    w.push_back(0u | (0u << 9) | (1u << 12) | (2u << 15) | (3u << 18) | (unnormalized ? 0u : (0xFu << 28)));
    w.push_back((0u << 20) | (1u << 23) | (2u << 26) | (3u << 29));
    w.push_back(0);
    return w;
}

// the flags of the translated shader's units, one bit per unit
uint32_t needs(uint32_t opcode, uint32_t unit, bool unnormalized) {
    std::vector<uint32_t> regs(LATTE_MAX_REGISTER, 0);
    regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + unit * 7] = uint32_t(Latte::E_DIM::DIM_2D);
    regs[mmCB_SHADER_MASK] = 0xF;
    regs[mmCB_SHADER_CONTROL] = 1;
    auto code = program(opcode, unit, unnormalized);
    LatteShader_UpdatePSInputs(regs.data());
    LatteDecompilerOptions options;
    LatteDecompilerOutput_t output{};
    LatteDecompiler_DecompilePixelShader(0x1234, regs.data(), reinterpret_cast<uint8*>(code.data()),
                                         uint32(code.size() * 4), options, &output);
    assert(output.shader && !output.shader->hasError);
    uint32_t bits = 0;
    for (uint32_t u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++) bits |= uint32_t(output.shader->textureNeedsGuestLevels[u]) << u;
    return bits;
}
}  // namespace

int main() {
    // units that keep the original
    assert(needs(GPU7_TEX_INST_SAMPLE, 0, true) == 1u);               // SAMPLE, texel coordinates
    assert(needs(GPU7_TEX_INST_LD, 0, false) == 1u);                  // texel read
    assert(needs(GPU7_TEX_INST_SAMPLE_L, 0, false) == 1u);            // explicit level
    assert(needs(GPU7_TEX_INST_GET_TEXTURE_RESINFO, 0, false) == 1u); // size query
    assert(needs(GPU7_TEX_INST_GET_COMP_TEX_LOD, 0, false) == 1u);    // level query
    assert(needs(GPU7_TEX_INST_FETCH4, 0, false) == 1u);              // gather: the four texels of the guest's grid
    // units a larger texture serves
    assert(needs(GPU7_TEX_INST_SAMPLE, 0, false) == 0u);              // SAMPLE, normalized
    assert(needs(GPU7_TEX_INST_SAMPLE_LZ, 0, false) == 0u);           // level 0 of the larger texture
    // the flag lands on the unit the instruction names
    assert(needs(GPU7_TEX_INST_LD, 3, false) == (1u << 3));
    assert(needs(GPU7_TEX_INST_SAMPLE, 3, false) == 0u);
    std::puts("guest_levels_shader: ok");
    return 0;
}

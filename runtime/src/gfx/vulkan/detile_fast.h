// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The guest offsets of one 8x8 micro tile's texels in a TM_2D_TILED_THIN1 surface with one sample, from one address
// computation per tile (Cemu's AddrLibFastDecode.h): the tile's pipe, bank and macro tile are the same for its 64
// texels, so each texel adds only its place in the micro tile (the address library's own table). Issue #1: decoding a
// new texture texel by texel (ComputeSurfaceAddrFromCoordMacroTiledCached for each) was most of the first bomb
// explosion's 67 ms hitch in the headset. The offsets equal the per-texel computation exactly (detile_fast_test).
#pragma once
#include <cstdint>

#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

namespace gfxvk {

// ci: SetupCachedSurfaceAddrInfo for the slice (tile mode 4, one sample, not depth); (xt, yt): the tile's first texel
// (multiples of 8); bytesPerElement: the element's bytes (ci.bpp / 8)
inline void micro_tile_offsets_tm04(LatteAddrLib::CachedSurfaceAddrInfo& ci, uint32_t xt, uint32_t yt,
                                    uint32_t bytesPerElement, uint32_t out[64]) {
    const uint16_t* table = ci.microTilePixelIndexTable + ((ci.slice & 7) << 6);
    // the tile's first texel's address, less its own place in the micro tile
    const uint32_t first = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(xt, yt, &ci);
    auto place = [&](uint32_t pixelIndex) {
        const uint32_t e = pixelIndex * bytesPerElement;
        // (past the 256-byte pipe interleave the address moves its high part over the pipe and bank bits)
        return bytesPerElement * 64 > 256 ? (e & 0xFFu) | ((e & ~0xFFu) << 3) : e;
    };
    const uint32_t base = first - place(table[0]);
    for (uint32_t i = 0; i < 64; ++i) out[i] = base + place(table[i]);
}

}  // namespace gfxvk

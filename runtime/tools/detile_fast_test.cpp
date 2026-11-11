// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// gfx/vulkan/detile_fast.h: the micro tile offsets of the fast texture decode equal the address library's per-texel
// ComputeSurfaceAddrFromCoordMacroTiledCached for every texel of TM_2D_TILED_THIN1 surfaces with one sample, across
// element sizes, pitches, heights, slices and pipe/bank swizzles.
#include <cassert>
#include <cstdio>

#include "gfx/vulkan/detile_fast.h"

int main() {
    uint64_t texels = 0, surfaces = 0;
    const uint32_t bpps[] = {8, 16, 32, 64, 128};
    const uint32_t pitches[] = {32, 64, 96, 128, 256, 384, 512, 1024};
    const uint32_t heights[] = {32, 64, 128, 256};
    for (uint32_t bpp : bpps)
        for (uint32_t pitch : pitches)
            for (uint32_t height : heights)
                for (uint32_t slices : {1u, 3u})
                    for (uint32_t pipeSwizzle = 0; pipeSwizzle < 2; ++pipeSwizzle)
                        for (uint32_t bankSwizzle = 0; bankSwizzle < 4; ++bankSwizzle)
                            for (uint32_t z = 0; z < slices; ++z) {
                                LatteAddrLib::CachedSurfaceAddrInfo fast{}, slow{};
                                LatteAddrLib::SetupCachedSurfaceAddrInfo(&fast, z, 0, bpp, pitch, height, slices, 1,
                                                                         Latte::E_HWTILEMODE::TM_2D_TILED_THIN1, false,
                                                                         pipeSwizzle, bankSwizzle);
                                slow = fast;
                                ++surfaces;
                                for (uint32_t yt = 0; yt < height; yt += 8)
                                    for (uint32_t xt = 0; xt < pitch; xt += 8) {
                                        uint32_t offsets[64];
                                        gfxvk::micro_tile_offsets_tm04(fast, xt, yt, bpp / 8, offsets);
                                        for (uint32_t i = 0; i < 64; ++i) {
                                            const uint32_t x = xt + (i & 7), y = yt + (i >> 3);
                                            const uint32_t want =
                                                LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &slow);
                                            if (offsets[i] != want) {
                                                printf("mismatch: bpp %u pitch %u height %u slice %u/%u swizzle %u/%u "
                                                       "texel %u,%u: fast %08X, per texel %08X\n",
                                                       bpp, pitch, height, z, slices, pipeSwizzle, bankSwizzle, x, y,
                                                       offsets[i], want);
                                                return 1;
                                            }
                                            ++texels;
                                        }
                                    }
                            }
    printf("detile fast path: %llu surfaces, %llu texels, all offsets equal\n", (unsigned long long)surfaces,
           (unsigned long long)texels);
    return 0;
}

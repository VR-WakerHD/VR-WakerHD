// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Batched same-state runs (issue #1, docs/VR_PERFORMANCE.md "Batched same-state runs"): what gx2 hands the renderer
// for a run of draws that differ only in the addresses of some vertex-shader uniform blocks (the grass draws one tuft
// a draw, each with its own matrix block). gx2 holds such a run (gx2_core.cpp, held_*) and gives it to the renderer as
// one draw with `instances` = its pieces and draw_batch::current() set; the renderer draws it as one instanced draw
// (gfx/vulkan/draw.cpp, batch_*) and sets consumed(), or leaves consumed() false without drawing anything, and gx2
// then draws the pieces one by one as before. Render thread only.
#pragma once
#include <cstdint>

namespace draw_batch {

constexpr uint32_t kMaxPieces = 1024;  // a held run is given to the renderer at this many (it draws chunks)
struct Batch {
    uint32_t count = 0;         // pieces (the run's draws)
    uint32_t blocks = 0;        // bit i: vertex-shader uniform block i's address differs between the pieces
    uint32_t addrs[kMaxPieces][16];  // each piece's vertex-shader uniform block addresses (register word 0 of each)
};
// the run the renderer's draw is for; null for an ordinary draw
inline const Batch*& current() {
    static thread_local const Batch* batch = nullptr;
    return batch;
}
// the renderer drew current() (set by the renderer; cleared by gx2 before each run)
inline bool& consumed() {
    static thread_local bool done = false;
    return done;
}
// the renderer: may a draw with these registers start a held run (its pieces are drawn on this thread, in order,
// from these registers)? Null: no renderer batches
using CanHold = bool (*)(const uint32_t* regs);
inline CanHold& can_hold() {
    static CanHold f = nullptr;
    return f;
}

}  // namespace draw_batch

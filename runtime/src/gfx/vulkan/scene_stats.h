// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The scene's shape, for splitting it between threads (docs/VR_SCENE_THREADS.md, stage S0; diagnostics only).
//
// WWHD_VK_SCENE_STATS=1 (off by default) counts, per frame, on the render thread: the render passes the draws begin,
// the draws in each, the earlier passes whose targets each pass samples, the layout changes and eye 1's flushes. Every
// 600 frames it logs the averages and, for the split between two passes that halves the scene's draws, how many
// surfaces the second part samples or lays out that the first part wrote. Once every 1,800 frames it also logs one
// frame's passes. Off, each hook is one test of a constant.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gfxvk {
struct Surface;
namespace scene_stats {
inline const bool kOn = [] { const char* e = std::getenv("WWHD_VK_SCENE_STATS"); return e && !std::strcmp(e, "1"); }();
// a draw begins a render pass (draw.cpp): its colour targets (8, may be null) and depth target
void pass_begin(Surface* const* colors, Surface* depth, uint32_t width, uint32_t height, bool multiview);
void draw();                    // a draw is recorded in the current pass
void sampled(Surface* s);       // a draw samples s (resolve_texture)
void layout_change(Surface* s); // s changes layout (transition_image)
void eye1_flush();              // eye 1's held commands are emitted (stereo_flush)
void frame_end();               // the swap
}  // namespace scene_stats
}  // namespace gfxvk

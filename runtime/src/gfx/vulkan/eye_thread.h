// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Eye 1's draws on a thread of their own (docs/VR_EYE_THREAD.md, stages E4 onwards; WWHD_VK_EYE_THREAD=1).
//
// Stereo draws each scene draw twice: eye 0 on the render thread resolves it and hands its result over (StereoShaders,
// draw.cpp), eye 1's draw replays it. With this thread, the render thread queues eye 1's draw (with a slot of eye 0's
// hand-over, eye1_handoff) instead of drawing it. The thread draws it from its replay register machine into eye 1's
// held segment (stereo.cpp, the eyes' held segments: on whenever this thread is), with its own pass, upload arena,
// descriptor pool and counters, and stereoEye 1. A cut (an in-draw flush) is queued too: the thread's pass ends at
// the end of its segment and it goes on in the next. stereo_flush joins it (finish) before it emits the segments.
//
// Stage E5: the render thread goes on at once. While the thread is behind, every change eye 0 makes to a texture eye 1
// may share cuts (eye 1's draws so far go before it on the GPU) and is marked with eye 0's draw. The thread using a
// texture eye 0 changed during a later draw switches the thread off for the session, logged (eye0_change, check).
#pragma once
#include <cstdint>

#include "stereo_switches.h"

namespace gfxvk {
class DeferredCommands;
struct Surface;
}

namespace gfxvk::eye_thread {

inline bool g_off = false;      // (eye_thread.cpp: switched off for the session)
inline bool g_running = false;  // (eye_thread.cpp)
// WWHD_VK_EYE_THREAD=1 (stereo; turns the eyes' held segments on)
inline bool enabled() { return switches::kEyeThread && !g_off; }
inline bool running() { return g_running; }  // eye 1's draws go to the thread now
bool on_eye_thread();
// (render thread) eye 1's draws from now on recorded into stream (eye 1's current held segment)
void begin(DeferredCommands* stream);
// (render thread) eye 1's draw of the draw eye 0 just drew, with eye 0's hand-over (null: none, eye 1 resolves it)
void draw(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances, const void* handoff);
// (render thread) a cut: the thread's pass ends at the end of its segment; it goes on in next
void cut(DeferredCommands* next);
// (render thread) the invalidate's part for eye 1's surfaces, in order with eye 1's draws
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
// (render thread) wait until the thread is done, its pass ended, its counters added
void finish();
// (render thread) the next slot of eye 0's hand-over, free for eye 0's draw to fill
void* handoff_slot(uint64_t& seq);
// (render thread) a surface's state is about to change during eye 0's draw (atlas_overlap.h before_change)
void eye0_change(Surface* s);
// (the thread) it uses s for its current draw (a texture eye 0 shares with it)
void check(const Surface* s);
void note_draw(bool replayed);  // (the thread, diagnostics)
// off for the rest of the session (logged once); a running thread finishes at the next join
void disable(const char* why);
// (diagnostics) since the last call
struct Stats {
    uint64_t draws = 0, cuts = 0, changeCuts = 0, invalidates = 0, joins = 0, ringWaits = 0;
    uint64_t replayed = 0, full = 0;  // (the thread) its draws replayed from eye 0's hand-over, or resolved in full
    double joinMs = 0;  // the render thread's waits at the joins
};
Stats take_stats();

// (draw.cpp) eye 1's draw on the thread: stereoShadersIn = handoff, then draw
void eye1_draw(const uint32_t* regs, const uint32_t* args, const void* handoff);
// (draw.cpp) the hand-over ring: slot i of n
void* handoff_ring(uint64_t i);
uint64_t handoff_ring_size();

}  // namespace gfxvk::eye_thread

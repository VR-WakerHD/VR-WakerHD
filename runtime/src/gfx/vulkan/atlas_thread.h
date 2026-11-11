// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The sun atlas's draws on a thread of their own (docs/VR_ATLAS_THREAD.md, stage 3; WWHD_VK_ATLAS_THREAD=1).
//
// Each frame's atlas comes as a block at its start. At the block's first draw the render thread starts an overlap
// (begin): the textures the atlas sampled last time are made ready first, on the render thread, so before anything of
// the atlas's on the GPU. The atlas's thread gets a replay register machine loaded from the render thread's
// registers, and from then on every register change the render thread makes is queued to it (gx2 RegisterObserver),
// in order with the block's draws and depth clears. They are done there, in that order, into the atlas's held
// stream, with the atlas's own pass, upload arena, descriptor pool and counters. (The block's invalidates are applied
// at once on the render thread, as before the thread: the scene that follows must see them.)
//
// The render thread does not wait for them. At the block's end (the next other draw) it goes on with the scene, whose
// commands are held in a stream of their own (stereo.cpp), while the atlas's thread catches up. The threads join
// (finish) where the order of the two matters: a scene draw that may sample the atlas, a submission, an in-order
// operation (a clear or copy), an invalidate, the swap, the render thread running out of commands. The atlas's stream
// is emitted, then the scene's: the GPU gets them in the guest's order.
//
// Stage 3.1 waited for each draw; stage 3.2 overlaps.
#pragma once
#include <cstdint>

namespace draw_batch { struct Batch; }

namespace gfxvk {
struct Surface;
}

namespace gfxvk::atlas_thread {

bool enabled();  // WWHD_VK_ATLAS_THREAD=1 (implies the atlas's held stream)
inline bool g_running = false;              // (atlas_thread.cpp)
inline bool running() { return g_running; }  // an overlap runs: the atlas's work goes to its thread
// (render thread) the block's first draw: start an overlap
void begin();
// (render thread) an atlas draw, recorded on the atlas's thread from the registers the render thread has now (r).
// proven: the registers alone show it writes no colour, so it is view-independent and the render thread goes on at
// once (true). Otherwise the render thread waits for it: true if it was view-independent after all, false if eye 1
// must draw it too.
bool draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances, bool proven);
// (render thread) a held run of atlas draws that write no colour (gfx/draw_batch.h; issue #1: the grass's shadow,
// three cascades of up to 982 tufts a frame, kept the render thread waiting for this thread): queued as one job, drawn
// there from its registers as instanced draws (draw.cpp batch_draw_run), else draw by draw
void draw_run(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
              const draw_batch::Batch& run);
// (render thread, while the block holds) a depth clear of the atlas itself, done on the atlas's thread in order: true
// if it was queued
bool clear_depth(uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
// (render thread) a guest flush while an overlap runs waits until the threads join (take_deferred_flush)
void defer_flush();
bool take_deferred_flush();
// (render thread, a scene draw while an overlap runs) whether it may sample the atlas: the threads join first
bool may_sample_atlas(const uint32_t* r);
// (the atlas's thread) a texture its draws sampled, made ready at the next overlap's start
void note_texture(const uint32_t* words, bool isDepthSampler, uint32_t maxLevels, Surface* s);
// (render thread) wait until the atlas's thread is done, its pass ended, its counters added: the overlap ends
void finish();
// (diagnostics, logged with the renderer's frame statistics) since the last call
struct Stats {
    uint64_t overlaps = 0, draws = 0, waited = 0, clears = 0, flushes = 0, sceneDraws = 0;
    uint64_t prepared = 0;  // textures made ready at the overlaps' starts
    double joinMs = 0;      // the render thread's waits at the joins
};
Stats take_stats();
void scene_draw();  // (render thread, diagnostics) a scene draw while the overlap runs

}  // namespace gfxvk::atlas_thread

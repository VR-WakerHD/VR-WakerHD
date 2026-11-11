// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The sun atlas recorded on a thread of its own while the render thread goes on with the scene: an overlap
// (docs/VR_ATLAS_THREAD.md, stages 2.6b onwards).
//
// The two threads share surfaces: the atlas's draws sample 15 to 16 textures the scene samples too. Their commands reach
// the GPU in an order the CPU did not record them in: all of the atlas's stream first, then the render thread's. So a
// change to a surface's state (an upload, a layout, its contents, its image) is in the right place only if:
//   - the atlas's thread made it (its stream comes first), or
//   - the render thread made it after every use of that surface by the atlas's thread.
// The rules:
//   - the atlas's thread marks every surface it uses (atlasUse = the overlap);
//   - the render thread never waits inside these hooks: they run inside the surface functions' locks, and waiting
//     there for the atlas's thread, which may want the same lock, could never end. It marks what it changes
//     (sceneChange = the overlap). It joins before a draw that samples the atlas (outside every lock), and the
//     textures the atlas used last frame are made ready at the overlap's start, before the atlas's thread runs, so
//     neither thread changes those meanwhile;
//   - the atlas's thread finding a surface the render thread has changed in this overlap is the one order that cannot
//     be put right: overlaps are switched off for the session, with a log line.
// Outside an overlap (id() == 0, always so before stage 3) every hook returns at once.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

#include "draw_threads.h"
#include "eye_thread.h"
#include "per_thread.h"
#include "stereo.h"

namespace gfxvk::overlap {

// the current overlap's number, 0 while there is none
inline std::atomic<uint64_t> g_current{0};
inline std::atomic<uint64_t>& current() { return g_current; }
inline uint64_t id() { return g_current.load(std::memory_order_acquire); }
// the calling thread is the atlas's
constinit inline thread_local bool atlasThreadTls = false;
inline bool on_atlas_thread() { return per_thread::possible() && atlasThreadTls; }
void set_atlas_thread();  // (the atlas's thread, at its start)
// (render thread) wait until the atlas's thread is done with this overlap's work
void join(const char* why);
// overlaps off for the rest of the session (logged once)
void disable(const char* why);
bool disabled();
void conflict(const Surface* s);  // (the atlas's thread) disable, the surface logged

// a surface the atlas's thread uses (a target, a texture)
inline void used(Surface* s) {
    if (switches::kSceneFront && s) [[unlikely]] scene_thread::front_used(s);  // (docs/VR_SCENE_THREADS.md, S7)
    const uint64_t o = id();
    if (!o || !s || !on_atlas_thread()) return;
    if (s->sceneChange == o) conflict(s);
    s->atlasUse = o;
}
// a surface's state is about to change (upload, layout, contents, image)
inline void before_change(Surface* s, const char* what) {
    if (!per_thread::possible()) return;  // (no draw thread this session: nothing to mark)
    // (docs/VR_SCENE_THREADS.md, stage S7) the second drawing thread before the render thread's part: marked and checked
    // as the atlas's thread and the scene are
    if (switches::kSceneFront) [[unlikely]] {
        if (s) scene_thread::front_change(s, what);
        if (scene_thread::on_scene_b()) return;
    }
    // (docs/VR_SCENE_THREADS.md, stage S6) the second drawing thread: its commands go after the scene's thread's span,
    // so it changes nothing that span may still use (rendering into the main pass's targets aside: both only draw)
    if (scene_thread::on_scene_b()) [[unlikely]] {
        if (s) scene_thread::b_before_change(s, what);
        return;
    }
    // (stage E5) eye 1's thread: its own changes are checked, never marked as the scene's; the render thread's, while
    // that thread runs, are eye 0's (docs/VR_EYE_THREAD.md)
    if (s && eye_thread::running()) {
        if (eye_thread::on_eye_thread()) {
            eye_thread::check(s);
            return;
        }
        // (a change recorded before the atlas's commands precedes both eyes' scene commands: no cut, no mark)
        if (!draw_threads::on_draw_thread() && !stereo_before_atlas(s)) eye_thread::eye0_change(s);
    }
    const uint64_t o = id();
    if (!o || !s) return;
    if (on_atlas_thread()) {
        used(s);
        return;
    }
    // (a game texture the atlas's thread has not used yet: changed before the atlas's commands, stereo_before_atlas)
    if (stereo_before_atlas(s)) return;
    s->sceneChange = o;
    s->sceneChangeWhat = what;
}

}  // namespace gfxvk::overlap

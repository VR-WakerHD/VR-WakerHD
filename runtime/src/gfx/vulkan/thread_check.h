// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// (diagnostics) Which thread uses each shared part of the renderer (WWHD_VK_THREAD_CHECK=1;
// docs/VR_ATLAS_THREAD.md). Recording the sun atlas on a thread of its own (docs/VR_PERFORMANCE.md, roadmap #1) makes
// two threads prepare draws at once. Each part below is made safe for that in a stage of its own; until then the
// first thread to use a part owns it. A use from any other thread is logged once per part, with both threads, so a
// part that is not yet safe shows up the first time it is reached from the wrong thread.
//
// Off, a note costs one predictable branch. On, a relaxed atomic load and a comparison of thread ids.
#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

namespace gfxvk::thread_check {

enum class Area : int {
    Uploads,      // the upload arena (snapshot, the vertex and index copies)
    Shaders,      // shader translation and its caches (vk::translate, vk::get_fetch_shader)
    Pipelines,    // the pipeline cache (pipeline, resolve_pending_pipeline)
    Descriptors,  // the descriptor pool and set caches (stage_descriptor_set)
    Surfaces,     // the surface maps and texture uploads (find_or_create_surface, upload_surface)
    Layouts,      // image layouts and their barriers (transition_image)
    BufferCache,  // the guest buffer cache (cached_guest_range)
    Passes,       // the render pass state and framebuffer caches (begin_pass)
    Draws,        // a draw's own preparation: registers, the continued fast path, statistics (draw)
    Count
};
inline const char* area_name(Area a) {
    switch (a) {
    case Area::Uploads: return "uploads";
    case Area::Shaders: return "shaders";
    case Area::Pipelines: return "pipelines";
    case Area::Descriptors: return "descriptors";
    case Area::Surfaces: return "surfaces";
    case Area::Layouts: return "image layouts";
    case Area::BufferCache: return "buffer cache";
    case Area::Passes: return "render passes";
    case Area::Draws: return "draw preparation";
    case Area::Count: break;
    }
    return "?";
}

// (a namespace-scope constant: tested several times a draw)
inline const bool kEnabled = [] { const char* e = std::getenv("WWHD_VK_THREAD_CHECK"); return e && !std::strcmp(e, "1"); }();
inline bool enabled() { return kEnabled; }
// the slow path (thread_check.cpp): takes ownership, or logs a use from another thread
void note_slow(Area a);
inline void note(Area a) {
    if (enabled()) note_slow(a);
}
// a part's owner and the uses from other threads seen so far (tests, the log at exit)
size_t foreign_uses(Area a);
// hashed thread id (as std::hash<std::thread::id>), never 0
inline size_t this_thread() {
    const size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return h ? h : 1;
}

}  // namespace gfxvk::thread_check

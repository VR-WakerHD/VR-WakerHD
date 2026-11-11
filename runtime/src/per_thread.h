// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Per-thread state of the renderer's draw threads (docs/VR_ATLAS_THREAD.md, docs/VR_EYE_THREAD.md): the selected eye,
// the held stream being recorded into, the pass state, counters, upload arena, descriptor pool, the register machine's
// counters and the profiler's per-draw marks.
//
// A thread_local read costs several dependent loads on Windows (and a wrapper call where it is defined in another
// file), and these are read many times a draw. With no draw thread possible this session (the atlas thread off:
// WWHD_VK_ATLAS_THREAD=0, or Android without =1; and no WWHD_VK_EYE_THREAD=1 or WWHD_VK_SCENE_THREAD=1) the render thread is the only one that
// uses them, so each lives in a plain global as well, and the accessors pick it: one predictable branch on a constant.
// PER_THREAD(type, name, init) declares both and the accessor name(), a reference to the calling thread's.
#pragma once
#include <cstdlib>
#include <cstring>
#include "gfx/vulkan/stereo_switches.h"

namespace per_thread {
inline const bool kPossible = [] {
    const auto on = [](const char* name) {
        const char* e = std::getenv(name);
        return e && !std::strcmp(e, "1");
    };
    // (the atlas thread is on by default, on Android only on a Quest: gfx/vulkan/stereo_switches.h atlas_default_on)
    const bool atlasThread = gfxvk::switches::atlas_default_on("WWHD_VK_ATLAS_THREAD");
    // (and the scene's parts' thread: docs/VR_SCENE_THREADS.md, stage S3)
    // (the scene thread on by default too, as gfxvk::switches::kSceneThread)
    return atlasThread || on("WWHD_VK_EYE_THREAD") || gfxvk::switches::scene_default_on("WWHD_VK_SCENE_THREAD") ||
           on("WWHD_VK_SCENE_FRONT");
}();
inline bool possible() { return kPossible; }
}  // namespace per_thread

// A per-thread object (a cache, a memo, scratch space) the same way: per_thread::get<Tag, T>() is the calling thread's
// T, default-constructed; Tag is a type naming the variable (PER_THREAD_STATIC declares one). The thread-local copy is
// never touched while no draw thread is possible, so neither its lookup nor its first-use guard costs anything then.
namespace per_thread {
template <class Tag, class T> struct Slot {
    static inline T global{};
    static inline thread_local T local{};
};
template <class Tag, class T> inline T& get() { return possible() ? Slot<Tag, T>::local : Slot<Tag, T>::global; }
}  // namespace per_thread
// in place of `static thread_local T name;` in a function: PER_THREAD_STATIC(name, T)
#define PER_THREAD_STATIC(name, ...) \
    struct name##_per_thread_tag;    \
    __VA_ARGS__& name = per_thread::get<name##_per_thread_tag, __VA_ARGS__>()

#define PER_THREAD(type, name, init)                                                  \
    constinit inline thread_local type name##_thread = init;                         \
    inline type name##_global = init;                                                \
    inline type& name() { return per_thread::possible() ? name##_thread : name##_global; }

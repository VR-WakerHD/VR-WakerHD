// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// (diagnostics) thread_check.h: the slow path.
#include "thread_check.h"

#include "runtime.h"

namespace gfxvk::thread_check {
namespace {
std::atomic<size_t> g_owner[int(Area::Count)];
std::atomic<size_t> g_foreign[int(Area::Count)];
std::atomic<bool> g_logged[int(Area::Count)];
}  // namespace

void note_slow(Area a) {
    const int i = int(a);
    const size_t me = this_thread();
    size_t owner = g_owner[i].load(std::memory_order_relaxed);
    if (owner == me) return;
    if (!owner) {
        if (g_owner[i].compare_exchange_strong(owner, me)) {
            LOG("[vulkan thread check] %s: owned by thread %zx", area_name(a), me);
            return;
        }
        if (owner == me) return;
    }
    g_foreign[i].fetch_add(1, std::memory_order_relaxed);
    if (!g_logged[i].exchange(true))
        LOG("[vulkan thread check] %s: used from thread %zx as well (first used by %zx; made safe for two threads by the "
            "atlas thread stages, docs/VR_ATLAS_THREAD.md)",
            area_name(a), me, owner);
}
size_t foreign_uses(Area a) { return g_foreign[int(a)].load(std::memory_order_relaxed); }

}  // namespace gfxvk::thread_check

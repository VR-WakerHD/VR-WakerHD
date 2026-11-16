// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
//
// Crash tracking for the record worker's replay (2026-11-13, docs/VR_ANDROID.md "Crash tracking"): a Quest's Adreno
// driver crashed again and again in a replayed vkCmdDrawIndexed (signal 11 at 0xf8). What the crash log then says, read
// by the crash handler without locks or allocation (plain atomics only, fixed-size tables):
// - the handles destroyed lately (pipelines, buffers, image views) and the render frame each went in: a draw whose
//   pipeline or buffer was destroyed (a use after free) shows as such;
// - each two-eye pipeline as it is made, and whether it carries the shading rate flag: a pipeline that does not suit
//   the pass shows as such;
// - the last 16 commands the replay handed to the driver, with their key handles.
// Header-only: loader.cpp, built into tools without the renderer, records the destroys too.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace gfxvk::crash_track {

// (the user's choice, 2026-11-13: off until the player's logs from the build without it come back) on only with
// WWHD_VK_CRASH_TRACK=1; off, nothing is recorded and the crash log has no such section
inline bool enabled() {
    static const bool on = [] { const char* e = std::getenv("WWHD_VK_CRASH_TRACK"); return e && e[0] == '1'; }();
    return on;
}

enum Kind : uint32_t { kPipeline = 1, kBuffer = 2, kImageView = 3 };
enum PipelineFlags : uint32_t { kRegistered = 1, kMultiview = 2, kShadingRate = 4 };

inline std::atomic<uint64_t> g_frame{0};  // the render frame now (backend.cpp, each frame's start)

struct Destroyed {
    std::atomic<uint64_t> handle{0}, frame{0};
    std::atomic<uint32_t> kind{0};
};
constexpr uint32_t kDestroyed = 1024;
inline Destroyed g_destroyed[kDestroyed];
inline std::atomic<uint32_t> g_destroyedAt{0};
inline void note_destroyed(Kind kind, uint64_t handle) {
    if (!handle || !enabled()) return;
    Destroyed& d = g_destroyed[g_destroyedAt.fetch_add(1, std::memory_order_relaxed) % kDestroyed];
    d.handle.store(0, std::memory_order_relaxed);  // (a half-written entry never matches)
    d.kind.store(kind, std::memory_order_relaxed);
    d.frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    d.handle.store(handle, std::memory_order_release);
}
// the frame `handle` was last destroyed in, or ~0 when it is not among the last kDestroyed destroys
inline uint64_t destroyed_frame(uint64_t handle, uint32_t* kind = nullptr) {
    if (!handle) return ~uint64_t{0};
    uint64_t best = ~uint64_t{0};
    for (const Destroyed& d : g_destroyed)
        if (d.handle.load(std::memory_order_acquire) == handle) {
            const uint64_t f = d.frame.load(std::memory_order_relaxed);
            if (best == ~uint64_t{0} || f > best) {
                best = f;
                if (kind) *kind = d.kind.load(std::memory_order_relaxed);
            }
        }
    return best;
}

struct Pipeline {
    std::atomic<uint64_t> handle{0};
    std::atomic<uint32_t> flags{0};
};
constexpr uint32_t kPipelines = 8192;
inline Pipeline g_pipelines[kPipelines];
inline uint32_t pipeline_slot(uint64_t h) { return uint32_t((h ^ (h >> 17) ^ (h >> 33)) * 2654435761u) % kPipelines; }
inline void note_pipeline(uint64_t handle, uint32_t flags) {
    if (!handle || !enabled()) return;
    for (uint32_t i = 0, s = pipeline_slot(handle); i < 32; ++i, s = (s + 1) % kPipelines) {
        uint64_t seen = g_pipelines[s].handle.load(std::memory_order_acquire);
        if (seen != handle && seen == 0 &&
            !g_pipelines[s].handle.compare_exchange_strong(seen, handle, std::memory_order_acq_rel))
            ;  // (taken meanwhile: seen holds the winner)
        if (g_pipelines[s].handle.load(std::memory_order_acquire) == handle) {
            g_pipelines[s].flags.store(flags | kRegistered, std::memory_order_release);
            return;
        }
    }
}
inline uint32_t pipeline_flags(uint64_t handle) {
    if (!handle) return 0;
    for (uint32_t i = 0, s = pipeline_slot(handle); i < 32; ++i, s = (s + 1) % kPipelines) {
        const uint64_t seen = g_pipelines[s].handle.load(std::memory_order_acquire);
        if (seen == handle) return g_pipelines[s].flags.load(std::memory_order_acquire);
        if (!seen) return 0;
    }
    return 0;
}

struct Command {
    std::atomic<uint32_t> opcode{0};
    std::atomic<uint64_t> cmd{0}, a{0}, b{0};
};
constexpr uint32_t kCommands = 16;
inline Command g_commands[kCommands];
inline std::atomic<uint32_t> g_commandAt{0};
inline void note_command(uint32_t opcode, uint64_t cmd, uint64_t a, uint64_t b) {
    if (!enabled()) return;
    Command& c = g_commands[g_commandAt.fetch_add(1, std::memory_order_relaxed) % kCommands];
    c.cmd.store(cmd, std::memory_order_relaxed);
    c.a.store(a, std::memory_order_relaxed);
    c.b.store(b, std::memory_order_relaxed);
    c.opcode.store(opcode, std::memory_order_release);
}

}  // namespace gfxvk::crash_track

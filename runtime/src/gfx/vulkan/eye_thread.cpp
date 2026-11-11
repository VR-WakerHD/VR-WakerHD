// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Eye 1's draws on a thread of their own: eye_thread.h.
#include "eye_thread.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../stereo.h"
#include "backend.h"
#include "deferred_commands.h"
#include "draw_thread.h"
#include "runtime.h"
#include "stereo.h"
#include "platform/thread_sampler.h"

namespace gfxvk::eye_thread {
namespace {

thread_local bool eyeThread = false;
thread_local uint64_t eyeDraw = 0;  // (the thread) the sequence number of the draw it does now
enum : uint32_t { kDraw = DrawThread::kUser, kCut, kInvalidate };

class Worker : public DrawThread {
public:
    Worker() { start(); }
    std::atomic<uint64_t> drawn{0};  // eye 1's draws done (the hand-over slots before it are free)

private:
    void setup() override {
        DrawThread::setup();
        thread_sampler::add_current_thread("eye1");  // (WWHD_SAMPLE_THREADS profiles)
        eyeThread = true;
        stereoEye() = 1;
        set_thread_upload_blocks(&R.eyeUploadBlocks);
        set_thread_descriptor_pool(&R.eyeDescriptorPool);
    }
    void execute(const Job& j, const uint32_t* words) override {
        switch (j.kind) {
        case kDraw:
            eyeDraw = j.u;
            eye1_draw(regs(), j.a, j.ptr);
            drawn.fetch_add(1, std::memory_order_release);
            break;
        case kCut:
            end_encoder();  // the pass ends at the end of the segment
            eye_deferred_commands() = static_cast<DeferredCommands*>(j.ptr);
            break;
        case kInvalidate:
            invalidate_surface_cache(surfaces_of(1), j.a[0], j.a[1], j.a[2]);
            break;
        }
    }
};
Worker* g_worker = nullptr;  // made at the first begin (never while the thread is off)
Worker& worker() {
    if (!g_worker) g_worker = new Worker;  // (lives for the session, as the atlas thread's)
    return *g_worker;
}
uint64_t g_queued = 0;  // eye 1's draws queued (the hand-over's sequence; the next one's number)
Stats g_stats;
std::atomic<uint64_t> g_replayed{0}, g_full{0};

}  // namespace

bool on_eye_thread() { return eyeThread; }

void disable(const char* why) {
    if (!g_off) LOG("[vulkan stereo] eye 1's thread: off for this session (%s)", why);
    g_off = true;
}

void begin(DeferredCommands* stream) {
    static bool logged = false;
    if (!logged) { logged = true; LOG("[vulkan stereo] eye 1's thread: on (stage E5: overlapping eye 0)"); }
    worker().begin(stream);
    g_running = true;
}

void draw(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances, const void* handoff) {
    auto& w = worker();
    DrawThread::Job j{kDraw};
    j.a[0] = prim; j.a[1] = count; j.a[2] = indexType; j.a[3] = indexAddr; j.a[4] = baseVertex; j.a[5] = instances;
    j.ptr = const_cast<void*>(handoff);
    j.u = g_queued;
    w.add(j);
    ++g_queued;
    ++g_stats.draws;
    // (diagnostics) WWHD_VK_EYE_SYNC=1: wait for each draw, as stage E4 did
    static const bool sync = switches::env_on("WWHD_VK_EYE_SYNC");
    // The render thread goes on at once. Its draws are handed over 16 at a time: the thread's replays are quicker than
    // eye 0's draws, so it is mostly asleep, and waking it for every draw cost the render thread a system call each
    // (about 2,600 a frame). A join hands over the rest (wait)
    static uint32_t unhanded = 0;
    if (sync) w.wait();
    else if (++unhanded >= 16) {
        unhanded = 0;
        w.hand();
    }
}

void cut(DeferredCommands* next) {
    DrawThread::Job j{kCut};
    j.ptr = next;
    worker().add(j);
    ++g_stats.cuts;
}

void invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    DrawThread::Job j{kInvalidate};
    j.a[0] = flags; j.a[1] = addr; j.a[2] = size;
    worker().add(j);
    ++g_stats.invalidates;
}

void eye0_change(Surface* s) {
    // (the render thread, during eye 0's draw number g_queued: its eye 1 draw gets that number)
    s->eye0ChangeSeq = g_queued;
    // eye 1's earlier draws may still sample it: a cut puts them before the change on the GPU, without a wait. Only a
    // texture eye 1 may share needs it: eye 1 has its own copy of every other (stereo_eye0_texture)
    const bool shareable = !s->gpuWritten || stereo::layoutPhotograph;
    if (shareable && worker().drawn.load(std::memory_order_acquire) < g_queued) {
        stereo_cut();
        ++g_stats.changeCuts;
    }
}
void check(const Surface* s) {
    // (the thread) a texture eye 0 changed during a later draw than this one: its state is one eye 1 must not see
    if (s->eye0ChangeSeq > eyeDraw) disable("eye 1's thread used a texture eye 0 changed during a later draw");
}

void finish() {
    if (!g_running) return;
    auto& w = worker();
    const auto t0 = std::chrono::steady_clock::now();
    g_running = false;
    w.finish();
    g_stats.joinMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++g_stats.joins;
}

void* handoff_slot(uint64_t& seq) {
    seq = g_queued;
    // a slot is free once the thread is past the draw that used it last
    if (g_running && seq >= handoff_ring_size()) {
        auto& w = worker();
        if (w.drawn.load(std::memory_order_acquire) <= seq - handoff_ring_size()) {
            ++g_stats.ringWaits;
            w.hand();
            while (w.drawn.load(std::memory_order_acquire) <= seq - handoff_ring_size()) std::this_thread::yield();
        }
    }
    return handoff_ring(seq % handoff_ring_size());
}

void note_draw(bool replayed) { (replayed ? g_replayed : g_full).fetch_add(1, std::memory_order_relaxed); }
Stats take_stats() {
    g_stats.replayed = g_replayed.exchange(0);
    g_stats.full = g_full.exchange(0);
    const Stats s = g_stats;
    g_stats = {};
    return s;
}

}  // namespace gfxvk::eye_thread

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A thread that records part of the renderer's draws (docs/VR_EYE_THREAD.md, stage E3: the sun atlas's thread and eye
// 1's share it).
//
// The render thread queues jobs (add) and hands them over in batches (hand): register writes, forwarded from the GX2
// machine as it applies them (gx2::RegisterObserver), and the thread's own jobs. The thread applies the writes to a
// replay register machine of its own, so its draws read the registers as they were at that point of the guest's
// order. It has its own pass state, upload arena, descriptor pool and draw counters (set by the derived class's setup,
// on the thread). It records into the held stream begin names, until finish. An error thrown there is thrown again on
// the render thread at its next wait.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "backend.h"
#include "gx2/replay_machine.h"

namespace gfxvk {
class DeferredCommands;

class DrawThread : public gx2::RegisterObserver {
public:
    // (diagnostics) every draw thread's register jobs: whole-file loads, writes and their words
    static inline std::atomic<uint64_t> g_loads{0}, g_writes{0}, g_writeWords{0};
    struct Job {
        uint32_t kind;
        uint32_t a[7] = {};
        float f = 0;
        uint32_t off = 0, n = 0;  // its words in the batch's
        void* ptr = nullptr;
        uint64_t u = 0;  // (a derived class's: eye 1's draw's sequence number)
    };
    // the base's own kinds; a derived class's start at kUser
    enum : uint32_t { kBegin, kRegs, kLoadRegs, kFull, kFinish, kLoadBlocks, kVerifyRegs, kUser = 16 };
    // (diagnostics) WWHD_VK_REG_BLOCKS_VERIFY=1: after each load by blocks the whole file too, compared with the
    // thread's (g_verifyMismatches: registers that differed), then loaded
    static inline std::atomic<uint64_t> g_verified{0}, g_verifyMismatches{0};

    ~DrawThread() override;
    // ---- render thread
    void add(Job job, const uint32_t* words = nullptr, uint32_t n = 0);
    void hand();  // the queued jobs to the thread
    void wait();  // until every queued job is done; the thread's error, if one was thrown, again here
    // an overlap: the machine loaded from the render thread's registers, register writes forwarded, recording into
    // stream (a held one) from the next job on
    void begin(DeferredCommands* stream);
    // its pass ended at the end of the stream, forwarding stopped, every job done, its counters added to the
    // renderer's
    void finish();
    // (docs/VR_SCENE_THREADS.md, stage S6) finish's two halves: forwarding stopped and its pass ended at the end of the
    // stream, then `last` (a derived class's job), all handed over without waiting; and later every job done, its
    // counters added to the renderer's
    void finish_async(Job last);
    void complete();
    bool running() const { return running_; }
    gx2::ReplayMachine& machine() { return machine_; }
    // gx2::RegisterObserver (render thread: queued in order with the jobs)
    void apply(uint32_t first, const uint32_t* values, uint32_t count) override;
    void load_regs(const uint32_t* regs) override;
    void load_blocks(const uint32_t* regs, const uint32_t* blocks, uint32_t count, uint32_t blockWords) override;
    void note_full_state_change() override;
    void resync(const uint32_t* regs) override;

protected:
    DrawThread() = default;
    void start();  // (the derived class's constructor, last)
    // ---- the thread
    virtual void setup();  // its per-thread state (the base's: counters, pass state, replay machine)
    virtual void execute(const Job& j, const uint32_t* words) = 0;  // a job of kUser or later
    uint32_t* regs() { return machine_.regs(); }

private:
    struct Batch {
        std::vector<Job> jobs;
        std::vector<uint32_t> words;
    };
    void run();
    void execute_base(const Job& j, const uint32_t* words);

    std::mutex mutex_;
    std::condition_variable changed_, done_;
    std::deque<Batch> ready_;
    std::vector<Batch> free_;
    Batch open_;  // (render thread)
    bool stopping_ = false, idle_ = false, busy_ = false, waiting_ = false;
    std::exception_ptr error_;
    gx2::ReplayMachine machine_;
    DrawCounters counters_;
    PassState pass_;
    bool running_ = false;  // (render thread)
    std::thread thread_;
};

// The register writes go to every draw thread running (gx2 has one forward: this fans out)
namespace draw_threads {
void forward_to(DrawThread* t);     // (render thread) from now on
void stop_forwarding(DrawThread* t);
// how many draw threads run now: the cache locks are on while one does
int running_threads();
}  // namespace draw_threads

}  // namespace gfxvk

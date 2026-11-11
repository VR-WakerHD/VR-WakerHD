// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// A thread that records part of the renderer's draws: draw_thread.h.
#include "draw_thread.h"

#include <algorithm>
#include <stdexcept>

#include "deferred_commands.h"
#include "draw_threads.h"
#include "gx2/gx2_regs.h"
#include "render_prof.h"

namespace gfxvk {

DrawThread::~DrawThread() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    thread_.join();
}
void DrawThread::start() {
    // its per-thread state must be its own: per_thread.h keeps it in plain globals unless a draw thread's switch is on
    if (!per_thread::possible()) throw std::logic_error("a draw thread started while per_thread::possible() is false");
    thread_ = std::thread([this] { run(); });
}

void DrawThread::add(Job job, const uint32_t* words, uint32_t n) {
    job.off = uint32_t(open_.words.size());
    job.n = n;
    if (n) open_.words.insert(open_.words.end(), words, words + n);
    open_.jobs.push_back(job);
}
void DrawThread::hand() {
    if (open_.jobs.empty()) return;
    bool wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_.push_back(std::move(open_));
        if (!free_.empty()) {
            open_ = std::move(free_.back());
            free_.pop_back();
        } else
            open_ = Batch{};
        wake = idle_;
    }
    if (wake) changed_.notify_one();
}
void DrawThread::wait() {
    hand();
    std::unique_lock<std::mutex> lock(mutex_);
    waiting_ = true;
    done_.wait(lock, [&] { return ready_.empty() && !busy_; });
    waiting_ = false;
    if (error_) {
        auto e = error_;
        error_ = nullptr;
        std::rethrow_exception(e);
    }
}

void DrawThread::begin(DeferredCommands* stream) {
    // the submission's command buffer is begun here, on the render thread: a draw thread finding none would begin it
    // itself, in the renderer's own stream, while the render thread records (right after a submission joined it)
    command_buffer();
    machine_.load();  // (the thread is idle: no overlap of its runs)
    if (draw_threads::running_threads() == 0) draw_threads::set_second_thread_active(true);
    running_ = true;
    draw_threads::forward_to(this);
    Job j{kBegin};
    j.ptr = stream;
    add(j);
}
void DrawThread::finish() {
    if (!running_) return;
    draw_threads::stop_forwarding(this);
    add(Job{kFinish});
    struct End {
        DrawThread* t;
        ~End() {
            t->running_ = false;
            if (draw_threads::running_threads() == 0) draw_threads::set_second_thread_active(false);
        }
    } end{this};
    wait();
    R.drawCounters.add(counters_);
    counters_ = {};
}

void DrawThread::finish_async(Job last) {
    if (!running_) return;
    draw_threads::stop_forwarding(this);
    add(Job{kFinish});
    add(last);
    hand();
}
void DrawThread::complete() {
    if (!running_) return;
    struct End {
        DrawThread* t;
        ~End() {
            t->running_ = false;
            if (draw_threads::running_threads() == 0) draw_threads::set_second_thread_active(false);
        }
    } end{this};
    wait();
    R.drawCounters.add(counters_);
    counters_ = {};
}

void DrawThread::apply(uint32_t first, const uint32_t* values, uint32_t count) {
    Job j{kRegs};
    j.a[0] = first;
    add(j, values, count);
}
void DrawThread::load_regs(const uint32_t* regs) { add(Job{kLoadRegs}, regs, gx2::kNumRegs); }
// (docs/VR_SCENE_THREADS.md, "register loads by block") each block's index, then its words: a few kilobytes, where the
// whole file is 256 KiB, copied into the batch and again into the thread's machine, about a dozen times a frame
void DrawThread::load_blocks(const uint32_t* regs, const uint32_t* blocks, uint32_t count, uint32_t blockWords) {
    Job j{kLoadBlocks};
    j.a[0] = count;
    j.a[1] = blockWords;
    j.off = uint32_t(open_.words.size());
    for (uint32_t b = 0; b < count; ++b) {
        open_.words.push_back(blocks[b]);
        const uint32_t first = blocks[b] * blockWords;
        const uint32_t n = std::min(blockWords, gx2::kNumRegs - first);
        open_.words.insert(open_.words.end(), regs + first, regs + first + n);
        open_.words.resize(open_.words.size() + (blockWords - n));  // (a short last block: padded)
    }
    j.n = uint32_t(open_.words.size()) - j.off;
    open_.jobs.push_back(j);
    static const bool verify = [] { const char* e = std::getenv("WWHD_VK_REG_BLOCKS_VERIFY"); return e && !std::strcmp(e, "1"); }();
    if (verify) add(Job{kVerifyRegs}, regs, gx2::kNumRegs);
}
void DrawThread::note_full_state_change() { add(Job{kFull}); }
void DrawThread::resync(const uint32_t* regs) {
    load_regs(regs);
    note_full_state_change();
}

void DrawThread::setup() {
    draw_threads::onDrawThreadTls = true;
    rprof::set_thread_uncounted();
    set_thread_draw_counters(&counters_);
    set_thread_pass_state(&pass_);
    machine_.attach();
}
void DrawThread::execute_base(const Job& j, const uint32_t* words) {
    switch (j.kind) {
    case kBegin: eye_deferred_commands() = static_cast<DeferredCommands*>(j.ptr); break;
    case kRegs:
        g_writes.fetch_add(1, std::memory_order_relaxed);
        g_writeWords.fetch_add(j.n, std::memory_order_relaxed);
        machine_.apply(j.a[0], words, j.n);
        break;
    case kLoadRegs: g_loads.fetch_add(1, std::memory_order_relaxed); machine_.load_regs(words); break;
    case kLoadBlocks: machine_.load_blocks(words, j.a[0], j.a[1]); break;
    case kVerifyRegs: {
        const uint32_t* mine = machine_.regs();
        uint64_t differ = 0;
        for (uint32_t i = 0; i < gx2::kNumRegs; ++i) differ += mine[i] != words[i];
        g_verified.fetch_add(1, std::memory_order_relaxed);
        g_verifyMismatches.fetch_add(differ, std::memory_order_relaxed);
        if (differ) machine_.load_regs(words);
        break;
    }
    case kFull: machine_.note_full_state_change(); break;
    case kFinish:
        end_encoder();  // its pass ends at the end of its held commands
        eye_deferred_commands() = nullptr;
        break;
    default: execute(j, words); break;
    }
}
void DrawThread::run() {
    setup();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        idle_ = true;
        changed_.wait(lock, [&] { return stopping_ || !ready_.empty(); });
        idle_ = false;
        if (ready_.empty() && stopping_) return;
        Batch b = std::move(ready_.front());
        ready_.pop_front();
        busy_ = true;
        lock.unlock();
        // after an error nothing more is done until the render thread has seen it (wait)
        if (!error_) {
            try {
                for (auto& j : b.jobs) execute_base(j, b.words.data() + j.off);
            } catch (...) {
                error_ = std::current_exception();
                eye_deferred_commands() = nullptr;
            }
        }
        b.jobs.clear();
        b.words.clear();
        lock.lock();
        free_.push_back(std::move(b));
        busy_ = false;
        if (ready_.empty() && waiting_) done_.notify_all();
    }
}

namespace draw_threads {
namespace {
// the GX2 machine's one forward, passed on to each running thread in turn
struct Fanout : gx2::RegisterObserver {
    DrawThread* to[2] = {};
    int n = 0;
    void apply(uint32_t first, const uint32_t* values, uint32_t count) override {
        for (int i = 0; i < n; ++i) to[i]->apply(first, values, count);
    }
    void load_regs(const uint32_t* regs) override {
        for (int i = 0; i < n; ++i) to[i]->load_regs(regs);
    }
    void load_blocks(const uint32_t* regs, const uint32_t* blocks, uint32_t count, uint32_t blockWords) override {
        for (int i = 0; i < n; ++i) to[i]->load_blocks(regs, blocks, count, blockWords);
    }
    void note_full_state_change() override {
        for (int i = 0; i < n; ++i) to[i]->note_full_state_change();
    }
    void resync(const uint32_t* regs) override {
        for (int i = 0; i < n; ++i) to[i]->resync(regs);
    }
} g_fanout;
}  // namespace
void forward_to(DrawThread* t) {
    if (std::find(g_fanout.to, g_fanout.to + g_fanout.n, t) != g_fanout.to + g_fanout.n) return;
    if (g_fanout.n == 2) throw std::logic_error("more than two draw threads");
    g_fanout.to[g_fanout.n++] = t;
    gx2::set_register_forward(&g_fanout);
}
void stop_forwarding(DrawThread* t) {
    auto* end = std::remove(g_fanout.to, g_fanout.to + g_fanout.n, t);
    g_fanout.n = int(end - g_fanout.to);
    if (!g_fanout.n) gx2::set_register_forward(nullptr);
}
int running_threads() { return g_fanout.n; }
}  // namespace draw_threads

}  // namespace gfxvk

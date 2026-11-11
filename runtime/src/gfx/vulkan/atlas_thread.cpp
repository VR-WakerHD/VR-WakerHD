// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The sun atlas's draws on a thread of their own: atlas_thread.h.
#include "atlas_thread.h"
#include "../draw_batch.h"
#include "stereo_switches.h"
#include "platform/core_groups.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "api.h"
#include "atlas_overlap.h"
#include "backend.h"
#include "deferred_commands.h"
#include "draw_thread.h"
#include "draw_threads.h"
#include "gx2/gx2_regs.h"
#include "gx2/replay_machine.h"
#include "render_prof.h"
#include "runtime.h"
#include "stereo.h"
#include "platform/thread_sampler.h"

namespace gfxvk {
void batch_draw_run(uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
                    uint32_t baseVertex, const draw_batch::Batch& run, bool* shared);  // (draw.cpp)
}
namespace gfxvk::atlas_thread {
namespace {

struct Texture {
    uint32_t words[7];
    bool depth;
    uint32_t maxLevels;
    Surface* surface;
};
enum : uint32_t { kDraw = DrawThread::kUser, kClearDepth, kDrawRun };

// (stage E3: on the draw thread both threads share, draw_thread.h)
class Worker : public DrawThread {
public:
    Worker() { start(); }
    bool last_shared() const { return lastShared_; }
    std::vector<Texture>& textures() { return textures_; }  // (while the atlas's thread is idle)
    // ---- the atlas's thread
    void note(const uint32_t* words, bool depth, uint32_t maxLevels, Surface* s) {
        for (auto& t : textures_)
            if (t.surface == s && t.depth == depth && !std::memcmp(t.words, words, sizeof t.words)) return;
        Texture t{};
        std::memcpy(t.words, words, sizeof t.words);
        t.depth = depth;
        t.maxLevels = maxLevels;
        t.surface = s;
        textures_.push_back(t);
    }

private:
    void setup() override {
        DrawThread::setup();
        thread_sampler::add_current_thread("atlas");  // (WWHD_SAMPLE_THREADS profiles)
        core_groups::place_current_thread(core_groups::Role::Other, "atlas");  // (WWHD_VK_PLACE_THREADS=0: not placed)
        overlap::set_atlas_thread();
        set_thread_upload_blocks(&R.atlasUploadBlocks);
        set_thread_descriptor_pool(&R.atlasDescriptorPool);
    }
    void execute(const Job& j, const uint32_t* words) override {
        switch (j.kind) {
        case kDraw: {
            bool shared = false;
            gfxvk::draw(regs(), j.a[0], j.a[1], j.a[2], j.a[3], j.a[4], j.a[5], &shared);
            lastShared_ = shared;
            if (j.a[6] && !shared)
                throw std::logic_error("a sun atlas draw whose registers showed no colour output drew colour");
            break;
        }
        case kDrawRun: {
            // words: the pieces, the batched blocks, then each piece's 16 block addresses
            static thread_local draw_batch::Batch run;
            run.count = words[0];
            run.blocks = words[1];
            std::memcpy(run.addrs, words + 2, size_t(run.count) * 16 * 4);
            bool shared = false;
            batch_draw_run(regs(), j.a[0], j.a[1], j.a[2], j.a[3], j.a[4], run, &shared);
            lastShared_ = shared;
            if (!shared) throw std::logic_error("a sun atlas run whose registers showed no colour output drew colour");
            break;
        }
        case kClearDepth:
            // as stereo_pair: eye 0, then eye 1 (whose atlas is eye 0's: stereo_surface_cache), in the atlas's stream
            clear_depth_stencil_copy(words, j.f, j.a[0], j.a[1]);
            stereoEye() = 1;
            try {
                clear_depth_stencil_copy(words, j.f, j.a[0], j.a[1]);
            } catch (...) {
                stereoEye() = 0;
                throw;
            }
            stereoEye() = 0;
            break;
        }
    }

    bool lastShared_ = false;
    std::vector<Texture> textures_;  // (the atlas's thread while it runs)
};

Worker& worker() {
    static Worker w;
    return w;
}
bool g_flushDeferred = false;
uint64_t g_overlaps = 0;
uint32_t g_atlasImage = 0;            // the atlas's image address (its depth buffer's), this overlap
std::vector<Texture> g_lastTextures;  // the textures the last overlap's draws sampled
Stats g_stats;

bool same_image(uint32_t a, uint32_t b) { return a && ((a ^ b) & ~0x7FFu) == 0; }  // (a macro-tiled swizzle aside)

// a surface still in eye 0's maps (the pointer is compared, never followed, before it is found there)
bool still_cached(const Surface* s, uint32_t addr) {
    auto range = surfaces_of(0).equal_range(addr);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second.get() == s) return true;
    return false;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const bool want = switches::atlas_default_on("WWHD_VK_ATLAS_THREAD");  // (on by default, a Quest too; =0 turns it off)
        if (want && !stereo_atlas_stream_enabled())
            LOG("[vulkan stereo] sun atlas thread: needs the atlas stream as well (WWHD_VK_ATLAS_STREAM=0: off)");
        if (want && stereo_atlas_stream_enabled()) LOG("[vulkan stereo] sun atlas thread: on (stage 3.2: overlapping the scene; WWHD_VK_ATLAS_THREAD=0 turns it off)");
        return want && stereo_atlas_stream_enabled();
    }();
    return on && !overlap::disabled();
}

void begin() {
    auto& w = worker();
    end_encoder();  // the renderer's own pass ends first: the atlas's commands are emitted after it, never inside it
    // The textures the atlas sampled last time are made ready now, so the scene finds them settled and changes none
    // the atlas's thread may still sample (atlas_overlap.h). On this thread, before the overlap: their uploads and
    // transitions go before the atlas's held commands, where its first draws would have made them.
    for (auto& t : g_lastTextures) {
        const uint32_t addr = t.words[2] << 8;
        if (still_cached(t.surface, addr) || still_cached(t.surface, addr & ~0x700u)) {
            // as the atlas's first bind of it would: its upload (if it changed), then its layout for sampling
            if (Surface* s = sampled_texture(t.words, t.depth, t.maxLevels))
                transition_image(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
            ++g_stats.prepared;
        }
    }
    end_encoder();
    overlap::current().store(++g_overlaps, std::memory_order_release);
    w.begin(stereo_atlas_thread_stream());
    g_running = true;
    g_atlasImage = 0;
    ++g_stats.overlaps;
}

bool draw(const uint32_t* r, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances, bool proven) {
    auto& w = worker();
    if (!g_atlasImage) g_atlasImage = r[mmDB_DEPTH_BASE];  // (the register holds the byte address here)
    DrawThread::Job j{kDraw};
    j.a[0] = prim; j.a[1] = count; j.a[2] = indexType; j.a[3] = indexAddr; j.a[4] = baseVertex; j.a[5] = instances;
    j.a[6] = proven;
    w.add(j);
    ++g_stats.draws;
    if (proven) {
        w.hand();
        return true;
    }
    ++g_stats.waited;
    w.wait();
    return w.last_shared();
}

void draw_run(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
              const draw_batch::Batch& run) {
    auto& w = worker();
    static std::vector<uint32_t> words;  // (render thread)
    words.resize(2 + size_t(run.count) * 16);
    words[0] = run.count;
    words[1] = run.blocks;
    std::memcpy(words.data() + 2, run.addrs, size_t(run.count) * 16 * 4);
    DrawThread::Job j{kDrawRun};
    j.a[0] = prim; j.a[1] = count; j.a[2] = indexType; j.a[3] = indexAddr; j.a[4] = baseVertex;
    w.add(j, words.data(), uint32_t(words.size()));
    g_stats.draws += run.count;
    w.hand();
}

bool clear_depth(uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags) {
    if (!g_running || !stereo_atlas_holding() || !g_atlasImage || !depthBuffer) return false;
    if (!same_image(depth_buffer_image(depthBuffer), g_atlasImage)) return false;
    // the guest's depth buffer as it is now (the game may change it before the atlas's thread gets to the clear)
    const size_t bytes = depth_buffer_bytes();
    std::vector<uint32_t> copy((bytes + 3) / 4);
    std::memcpy(copy.data(), mem::ptr(depthBuffer), bytes);
    DrawThread::Job j{kClearDepth};
    j.f = depth;
    j.a[0] = stencil;
    j.a[1] = flags;
    worker().add(j, copy.data(), uint32_t(copy.size()));
    worker().hand();
    ++g_stats.clears;
    return true;
}

void defer_flush() {
    g_flushDeferred = true;
    ++g_stats.flushes;
}
bool take_deferred_flush() {
    const bool f = g_flushDeferred;
    g_flushDeferred = false;
    return f;
}

bool may_sample_atlas(const uint32_t* r) {
    if (!g_atlasImage) return false;
    // every stage's texture units, bound by the draw's shaders or not (conservative)
    for (uint32_t base : {uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                          uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                          uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
        for (uint32_t unit = 0; unit < 18; ++unit)
            if (same_image(r[base + unit * 7 + 2] << 8, g_atlasImage)) return true;
    return false;
}

void note_texture(const uint32_t* words, bool isDepthSampler, uint32_t maxLevels, Surface* s) {
    worker().note(words, isDepthSampler, maxLevels, s);
}

void finish() {
    if (!g_running) return;
    auto& w = worker();
    struct End {
        ~End() {
            overlap::current().store(0, std::memory_order_release);
            g_running = false;
        }
    } end;
    if (draw_threads::held_locks())
        overlap::disable("the render thread joined the atlas's thread while holding a cache lock");
    const auto t0 = std::chrono::steady_clock::now();
    w.finish();
    g_stats.joinMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_lastTextures.swap(w.textures());
    w.textures().clear();
}

Stats take_stats() {
    const Stats s = g_stats;
    g_stats = {};
    return s;
}
void scene_draw() { ++g_stats.sceneDraws; }

}  // namespace gfxvk::atlas_thread

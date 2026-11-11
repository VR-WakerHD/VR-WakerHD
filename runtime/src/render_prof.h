// Render-thread profiler shared by both renderers (render_prof.cpp).
//
// Opt-in diagnostics: every command op is counted; draws and register writes are timed on one
// call in 64 (and the phases inside those sampled draws), rare ops (clears, copies, flushes, swaps)
// on every call. Upload bytes are split by kind; "unique guest bytes" (the union of the guest ranges
// that vertex, index and uniform copies read) is measured on one frame in 16. A classifier counts
// draws whose registers changed only in buffer pointers / ALU constants since the previous draw
// (candidates for a continued-draw fast path).
//
// Every 120 swaps the counters are turned into a text report: logged with WWHD_PROFILE=1,
// WWHD_VK_STATS or WWHD_VK_CPU_ONLY_STATS=1, and kept for the overlay's "Copy performance report"
// button (latest_report()).
//
// Off by default. Settings > Graphics > "Detailed performance profiling" turns it on (saved as
// detailedProfiling); WWHD_PROFILE=1 forces it on and WWHD_PROFILE=0 off for that start (the setting
// is then not used). A change takes effect at the next swap, between two frames on the render
// thread (frame_end): turning it on starts the counters afresh (the first report follows 120 frames
// later), turning it off drops the last report.
#pragma once
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>

#include "per_thread.h"

namespace rprof {

enum Phase : int {
    // inside sampled draws
    kShader, kIndices, kTargets, kPipeline, kUniforms, kTextures, kDescriptors,
    kMvSetup, kMvLookups, kMvUniforms, kMultiview,  // multiview_draw's parts (kMultiview: its sets, and the rest)
    kPass, kRecord, kVertex,
    kSubmit, kDrawPhases
};
enum Op : int { kOpRegs, kOpDraw, kOpClear, kOpCopy, kOpScan, kOpInvalidate, kOpFlush, kOpDrawDone, kOpSwap, kOpOther, kOps };
enum Upload : int { kUpOther, kUpVertex, kUpIndex, kUpUbo, kUpUniformVars, kUpTexture, kUploadKinds };
enum Wait : int { kWaitGpu, kWaitAcquire, kWaitPresent, kWaits };
enum Sync : int { kSyncDrawDone, kSyncCopySurface, kSyncFlip, kSyncOther, kSyncs };

bool enabled();               // collecting now (any thread)
// (VR fork) the same, read by the per-draw hooks below before their per-thread state: a per-thread read is a
// thread-local lookup, and these run many times a draw, profiler on or off
inline std::atomic<bool> g_on{false};
constexpr const char* kSettingKey = "detailedProfiling";  // hostui setting: "1" on, "0" off
const char* env_override();   // WWHD_PROFILE when set (it wins over the setting), else nullptr
bool requested();             // what is in effect from the next swap on: WWHD_PROFILE, else the setting
void set_requested(bool on);  // the setting changed (any thread); applied at the next swap
void start(bool saved);       // start-up, before the first frame: the saved setting; logs the state
// the performance report's detailed part: the latest report, or one line saying why there is none
std::string report_section();
uint64_t now_ns();
uint64_t thread_cpu_ns();  // CPU time of the calling thread (0: not available)

// ---- render thread
// (per thread, docs/VR_ATLAS_THREAD.md stage 2.9: a thread drawing the sun atlas is never sampled)
PER_THREAD(bool, g_draw_sampled, false)  // the current draw is sampled (phase marks are recorded; per_thread.h)
PER_THREAD(uint64_t, g_mark, 0)          // time of the previous phase mark
void mark_slow(Phase p);
// (docs/VR_SCENE_THREADS.md, "Why S6 is slow") a draw thread's sampled draws' phases, apart from the render thread's:
// the thread sets g_phase_thread and samples its own draws (begin_thread_draw); thread_phases reads and clears them
PER_THREAD(bool, g_phase_thread, false)
void begin_thread_draw();  // (a draw thread, profiler on) its next draw's phases are timed
void end_thread_draw();
uint64_t thread_phases(uint64_t* ns);  // ns[kDrawPhases]: the draw threads' since the last call; returns the draws timed
inline void mark(Phase p) {
    if (g_on.load(std::memory_order_relaxed) && g_draw_sampled()) mark_slow(p);
}
// parts of a phase, timed on their own sample of draws (one in 64, never a draw whose phases are
// timed, so the phase totals do not carry these timers' cost); reported on their own line, scaled
// like the phases. sub_begin() returns 0 when the draw is not in that sample.
enum SubPhase : int { kSubUniformSetup, kSubUniformPack, kSubUniformBlocks, kSubUniformSupportUpload, kSubPhases };
PER_THREAD(bool, g_sub_sampled, false)  // the current draw's sub-phases are timed (per thread: the draw threads)
inline uint64_t sub_begin() { return g_on.load(std::memory_order_relaxed) && g_sub_sampled() ? now_ns() : 0; }
void sub_end_slow(SubPhase p, uint64_t started);
inline void sub_end(SubPhase p, uint64_t started) {
    if (started) sub_end_slow(p, started);
}

// op timing around execute_one: begin() returns 0 when this call is not timed
uint64_t op_begin(Op op);
void op_end(Op op, uint64_t started);

// uploads: the kind of the copies made while the scope lives
PER_THREAD(int, g_upload_kind, kUpOther)  // (per thread: stage 2.9)
// the calling thread's uploads and guest reads are not counted (a thread drawing the sun atlas: the frame's counts are
// the render thread's)
void set_thread_uncounted();
PER_THREAD(bool, g_uncounted, false)
struct UploadKind {
    bool on = g_on.load(std::memory_order_relaxed);
    int saved = on ? g_upload_kind() : 0;
    explicit UploadKind(Upload k) { if (on) g_upload_kind() = k; }
    ~UploadKind() { if (on) g_upload_kind() = saved; }
};
void add_upload(uint64_t bytes);            // in g_upload_kind
void add_upload(Upload kind, uint64_t bytes);
// a copy of guest memory [addr, addr+size) (vertex, index, uniform block): unique-byte tracking
extern bool g_track_unique;
void guest_read_slow(Upload kind, uint32_t addr, uint64_t size);
inline void guest_read(Upload kind, uint32_t addr, uint64_t size) {
    if (g_track_unique && !g_uncounted()) guest_read_slow(kind, addr, size);
}

void add_wait(Wait w, uint64_t ns);         // render thread blocked on the GPU / swapchain
void add_idle(uint64_t ns);                 // render thread waiting for commands

// draw classifier (render thread, gx2 register application)
extern uint32_t g_reg_dirty;  // bit 0: buffer/constant registers changed, bit 1: other registers changed;
                              // which buffer/constant words: kFast* below
void note_other_reg(uint32_t reg);          // a non-buffer register changed value
// at each draw: count its class, clear g_reg_dirty. regs: the register file (the draw's shaders, for the same-state
// runs: consecutive draws that change only buffer/constant words, what batching them into one draw would merge)
void classify_draw(const uint32_t* regs);
enum : uint32_t { kFastAlu = 4, kFastVsBlock = 8, kFastPsBlock = 16, kFastGsBlock = 32, kFastVertex = 64, kFastKinds = 0x7C };
inline uint32_t fast_kind(uint32_t reg) {  // (a buffer/constant word: fast_class_reg)
    if (reg >= 0xC000 && reg < 0xD000) return kFastAlu;
    if (reg >= 0xE7E0 && reg < 0xE7E0 + 7 * 16) return kFastVsBlock;
    if (reg >= 0xE250 && reg < 0xE250 + 7 * 16) return kFastPsBlock;
    if (reg >= 0xECB0 && reg < 0xECB0 + 7 * 16) return kFastGsBlock;
    return kFastVertex;
}
// ALU constants, uniform-block and vertex-buffer words (gx2 looks this up in its register class table)
inline bool fast_class_reg(uint32_t reg) {
    // RegDefines.h: mmSQ_ALU_CONSTANT0_0 0xC000; mmSQ_TEX_RESOURCE_WORD0 0xE000 + 0x7E0 (VS uniform
    // blocks), + 0x250 (PS), + 0xCB0 (GS), + 0x8C0 (vertex attribute buffers), 7 words each
    if (reg >= 0xC000 && reg < 0xC000 + 0x1000) return true;
    for (uint32_t base : {0xE7E0u, 0xE250u, 0xECB0u, 0xE8C0u})
        if (reg >= base && reg < base + 7 * 16) return true;  // includes the vertex stride (pipeline-only)
    return false;
}

// shader translations: called by the Vulkan renderer for each new variant (shaders.cpp)
void shader_variant(bool newProgram, bool onlyUnusedUnits, const char* const* groups, int groupCount);

// once per swap (render thread); hold: drawn in an interpolation hold pass
void frame_end(bool hold);

// ---- game thread
void add_sync(Sync site, uint64_t ns);
// GPU results written back to guest memory (linear surfaces the CPU reads, issue #53): the time the
// GX2DrawDone check spent walking the surfaces (walkNs), and per write-back its bytes and the time to
// read them from the GPU (readNs, including the wait for the GPU)
void add_write_back(uint64_t walkNs, uint32_t surfaces, uint64_t bytes, uint64_t readNs);

// texture cache (render thread): a full check of a CPU texture's guest bytes (hashedBytes; changed: uploaded again,
// else the cached image served), and the texture budget's releases and remakes (Vulkan WWHD_VK_TEXTURE_BUDGET)
void add_texture_check(uint64_t hashedBytes, bool changed);
void add_texture_release(uint64_t bytes);
void add_texture_remake();
// GPU timeline (any thread): one command buffer's / submission's GPU interval in ns on the GPU's clock, in
// submission order; the gaps between consecutive intervals are reported as GPU idle (Metal: GPUStartTime/
// GPUEndTime of each command buffer; Vulkan: the submission timestamps when GPU timestamps are on)
void add_gpu_span(double startNs, double endNs);

// ---- any thread
std::string latest_report();  // the last complete report ("" before the first one)

}  // namespace rprof

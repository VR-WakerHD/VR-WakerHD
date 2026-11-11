#include "gfx/depth_peek.h"
#include "gfx/draw_batch.h"
#include "gfx/shader_prepare.h"
#include "platform/present_observer.h"
#include "platform/r5_slow_render.h"
#include "platform/core_groups.h"
#include "gfx/display_modes.h"
#include "../game_clock.h"
#include "vsync_pacing.h"
#include "../mods/fast_forward.h"
#include "../mods/mod_actor_shadow_proof.h"
#include "../per_thread.h"
#include "../platform/host.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <string>
#include <vector>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "submitted_uniforms.h"
#include "../cloud_proto.h"
#include "gx2_regs.h"
#include "register_blocks.h"
#include "gx2_texture_regs.h"
#include "backend_hooks.h"
#ifdef WWHD_VR
#include "../vr/vr.h"
#endif
#include "state_groups.h"
#include "register_class.h"
#include "thread_identity.h"
#ifdef WWHD_HAS_VULKAN
#include "gfx/vulkan/api.h"
#include "gfx/vulkan/background_checkpoint.h"
#endif
#include "runtime.h"
#include "savestate.h"
#include "state_metadata_validation.h"
#include "guest_addr.h"
#include "../aspect.h"
#include "gfx/stereo.h"
#include "gfx/renderer.h"
#include "platform/perf_hint.h"
#include "platform/thread_sampler.h"
#include "render_prof.h"
#include "replay_machine.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
static std::recursive_mutex g_exec_mutex;
static RegisterBlocks<kNumRegs> g_register_blocks;

uint32* regs() { return g_regs; }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }

// shader_irrelevant_reg (register_class.h); WWHD_VK_SHADER_ADDRESS_MEMO=1 (Vulkan) adds the texture
// address words, which bind_stage resolves freshly
static bool shader_address_memo() {
#ifdef WWHD_HAS_VULKAN
    static const bool addressMemo = [] {
        const char* e = getenv("WWHD_VK_SHADER_ADDRESS_MEMO");
        return render::vulkan() && e && !strcmp(e, "1");
    }();
    return addressMemo;
#else
    return false;
#endif
}
static ShaderKeyDirtyStats shaderKeyDirtyStats;

// register state groups (state_groups.h): the continued-draw fast path of the Vulkan renderer
uint64_t g_full_state_version = 1;
uint32_t g_dirty_groups = 0;
static uint32_t g_groups_since_draw = 0;  // (the index-run diagnostics: the groups changed since the last draw)
static bool g_statRegsOn = false;          // (and the registers changed since the last draw, while they are on)
static uint32_t g_statRegList[128], g_statRegN = 0;
static ChangeLog g_change_log;  // (replay_machine.h)
// a replay machine fed every register change the render thread makes (replay_machine.h): the verifier
// (WWHD_GX2_REPLAY_VERIFY=1) or the sun atlas's thread's
static ReplayMachine* g_observer = nullptr;
static RegisterObserver* g_forward = nullptr;  // the sun atlas's thread's queue (set_register_forward)
static inline void note_full_state_change() {
    g_dirty_groups |= 1u << kSgOther;
    g_groups_since_draw |= 1u << kSgOther;
    ++g_full_state_version;
    g_change_log.reset();
    if (g_observer) g_observer->note_full_state_change();
    if (g_forward) g_forward->note_full_state_change();
}
ShaderKeyDirtyStats shader_key_dirty_stats() { return shaderKeyDirtyStats; }

// which register changes count for the shader key (backend_hooks.h), picked once the renderer runs
static ShaderKeyMask shader_key_mask() {
    static const ShaderKeyMask keyMask = pick_shader_key_mask(render::g_backend);
    return keyMask;
}

// ---------------------------------------------------------------- register classes
// register_class.h: per-register tables built on the first register command (the renderer is
// chosen by then), with the switches the apply paths read, so they are not looked up per write
static RegisterClasses g_reg_classes;
static const uint8* const g_reg_class = g_reg_classes.cls;
static ShaderKeyBits g_key_bits;     // shader_key_mask(): set-context restore and bulk writes
static ShaderKeyBits g_vk_key_bits;  // vulkan_shader_key_mask with WWHD_VK_SHADER_KEY_DIRTY=1: small writes
static bool g_reg_tables_ready = false;
static bool g_fused_small = false;    // Vulkan: the per-word path for writes of up to 16 words
static bool g_key_dirty = false;      // WWHD_VK_SHADER_KEY_DIRTY=1
static bool g_collect_stats = false;  // WWHD_VK_STATS (Vulkan)
static bool g_classify = false;       // render-thread profiler on (rprof::enabled(); updated at each swap)

static void build_reg_tables() {
    g_reg_classes.build(shader_address_memo(), [](uint32 r) { return rprof::fast_class_reg(r); });
    g_classify = rprof::enabled();
#ifdef WWHD_HAS_VULKAN
    {
        const char* e = getenv("WWHD_VK_FUSE_SMALL_REGS");
        // Enabled by default; explicit zero retains the original bulk path.
        g_fused_small = render::vulkan() && (!e || strcmp(e, "0") != 0);
        e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
        g_key_dirty = e && !strcmp(e, "1");
        g_collect_stats = render::vulkan() && getenv("WWHD_VK_STATS") != nullptr;
    }
    if (g_key_dirty) g_vk_key_bits.build(vulkan_shader_key_mask);
#endif
    g_key_bits.build(shader_key_mask());
    g_reg_tables_ready = true;
}
static inline void ensure_reg_tables() {
    if (__builtin_expect(!g_reg_tables_ready, 0)) build_reg_tables();
}

static inline void note_state_change(uint32 reg, uint8 cls, uint32 old, uint32 value) {
    const uint32 g = state_group_change(cls & kRcGroup, old, value);
    g_dirty_groups |= 1u << g;
    g_groups_since_draw |= 1u << g;
    if (g_statRegsOn) [[unlikely]]
        if (g_statRegN < 128) g_statRegList[g_statRegN++] = reg;
    const bool ends = !((kSgContinuedMask >> g) & 1);
    g_full_state_version += ends;
    if (ends) g_change_log.add(reg, old, value, (cls & kRcGroup) == kSgStencilRefMask ? 0xFFFFFF00u : ~0u, g);
}
static inline void classify_reg(uint8 cls, uint32 reg) {  // render-thread profiler's draw classifier
    if (cls & kRcProfFast) rprof::g_reg_dirty |= 1 | rprof::fast_kind(reg);
    else rprof::note_other_reg(reg);
}

// register writes copy a few words mostly (display lists set registers in groups of 1-7): short
// ones are copied inline instead of calling memcpy
static inline void copy_words(uint32* d, const uint32* v, uint32 n) {
    if (n > 8) {
        memcpy(d, v, n * 4);
        return;
    }
    for (uint32 i = 0; i < n; ++i) d[i] = v[i];
}

#ifdef WWHD_HAS_VULKAN
static void apply_small_regs(uint32 first, const uint32* v, uint32 n) {
    const bool keyDirty = g_key_dirty, collectStats = g_collect_stats, classify = g_classify;
    bool changed = false, baselineBump = false, actualBump = false;
    uint64_t maskedWords = 0;
    for (uint32 i = 0; i < n; ++i) {
        const uint32 reg = first + i, value = v[i], old = g_regs[reg];
        if (old != value) {
            const uint8 cls = g_reg_class[reg];
            changed = true;
            note_state_change(reg, cls, old, value);
            if (classify) classify_reg(cls, reg);
            if ((collectStats || !actualBump) && !(cls & kRcShaderIrrelevant)) {
                baselineBump = true;
                if (keyDirty && !g_vk_key_bits.changed(reg, old, value)) {
                    if (collectStats) ++maskedWords;
                } else actualBump = true;
            }
            g_regs[reg] = value;
        }
    }
    // Shadow can differ even when registers already match: draw mutates
    // primitive type directly, and context setup initializes only shadow.
    if (g_shadow) copy_words(&g_shadow[first], v, n);
    if (actualBump) ++g_shader_state_gen;
    if (changed && collectStats) {
        ++shaderKeyDirtyStats.changedBatches;
        shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
        shaderKeyDirtyStats.actualBumps += actualBump;
        shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
        shaderKeyDirtyStats.maskedWords += maskedWords;
    }
}
#endif

static void apply_regs(uint32 first, const uint32* v, uint32 n);
static inline __attribute__((always_inline)) void apply_regs_impl(uint32 first, const uint32* v, uint32 n) {
    if (first >= kNumRegs || n > kNumRegs - first) return;
    ensure_reg_tables();
    g_register_blocks.touch(first, n);
    // ALU constants (uniform registers, most of the register traffic): continued state (one dirty
    // group), shader-irrelevant, a buffer/constant word for the draw classifier; nothing else to
    // decide per word. (Not with WWHD_VK_STATS, which counts these batches too.)
    if (first >= mmSQ_ALU_CONSTANT0_0 && first + n <= mmSQ_ALU_CONSTANT0_0 + 0x1000 && !g_collect_stats) {
        uint32* d = &g_regs[first];
        if (memcmp(d, v, n * 4) != 0) {
            g_dirty_groups |= 1u << kSgAluConst;
            if (g_classify) rprof::g_reg_dirty |= 1 | rprof::kFastAlu;
            copy_words(d, v, n);
        }
        if (g_shadow) copy_words(&g_shadow[first], v, n);
        return;
    }
#ifdef WWHD_HAS_VULKAN
    // Vulkan renderer only (the Metal renderer keeps the original bulk path)
    if (g_fused_small && n <= 16) {
        apply_small_regs(first, v, n);
        return;
    }
#endif
    if (memcmp(&g_regs[first], v, n * 4) != 0) {
        const bool classify = g_classify;
        for (uint32 i = 0; i < n; i++)
            if (g_regs[first + i] != v[i]) {
                const uint8 cls = g_reg_class[first + i];
                note_state_change(first + i, cls, g_regs[first + i], v[i]);
                if (classify) classify_reg(cls, first + i);
            }
        const bool collectStats = g_collect_stats;
        if(!g_key_bits.bits.empty() || collectStats) {
            bool baselineBump = false, actualBump = false;
            uint64_t maskedWords = 0;
            for(uint32 i = 0; i < n; ++i) {
                uint32 reg = first + i;
                if(g_regs[reg] == v[i] || (g_reg_class[reg] & kRcShaderIrrelevant)) continue;
                baselineBump = true;
                if(!g_key_bits.changed(reg, g_regs[reg], v[i])) {
                    if(collectStats) ++maskedWords;
                } else actualBump = true;
                if(actualBump && !collectStats) break;
            }
            if(actualBump) ++g_shader_state_gen;
            if(collectStats) {
                ++shaderKeyDirtyStats.changedBatches;
                shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
                shaderKeyDirtyStats.actualBumps += actualBump;
                shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
                shaderKeyDirtyStats.maskedWords += maskedWords;
            }
        } else
        for (uint32 i = 0; i < n; i++)
            if (g_regs[first + i] != v[i] && !(g_reg_class[first + i] & kRcShaderIrrelevant)) { g_shader_state_gen++; break; }
        memcpy(&g_regs[first], v, n * 4);
    }
    if (g_shadow) memcpy(&g_shadow[first], v, n * 4);
}

// ---------------------------------------------------------------- the replay machine (replay_machine.h)
namespace {
// the GX2 machine's counters, filled before any draw (a static initializer)
const bool g_machineCountersSet = [] {
    g_machineCounters = {&g_shader_state_gen, &g_full_state_version, &g_dirty_groups, &g_change_log};
    return true;
}();
// (read once: every register write tests it)
const bool g_replayVerify = [] { const char* e = getenv("WWHD_GX2_REPLAY_VERIFY"); return e && !strcmp(e, "1"); }();
inline bool replay_verify() { return g_replayVerify; }
ReplayVerifyStats g_verifyStats;
}  // namespace
ReplayVerifyStats replay_verify_stats() { return g_verifyStats; }
static_assert(ChangeLog::kGroups == kSgCount, "the change log keys each register group");
ReplayMachine::ReplayMachine() : regs_(kNumRegs, 0) {
    counters_ = {&shaderGen_, &fullVersion_, &dirty_, &log_};
    log_.keyed = true;  // (a drawing thread's: replay_machine.h ChangeLog::keyed)
}
void ReplayMachine::load() {
    memcpy(regs_.data(), g_regs, sizeof g_regs);
    ++shaderGen_;
    ++fullVersion_;
    dirty_ = ~0u;
    log_.reset();
}
void ReplayMachine::attach() { threadCounters() = &counters_; }
const uint32_t* render_regs() { return g_regs; }
void ReplayMachine::detach() { threadCounters() = &g_machineCounters; }
void ReplayMachine::note_full_state_change() {
    dirty_ |= 1u << kSgOther;
    ++fullVersion_;
    log_.reset();
}
void ReplayMachine::load_regs(const uint32_t* regs) {
    ensure_reg_tables();
    log_.reset();  // (registers replaced without logging)
    // as set_context's restore: a changed register that is not shader-irrelevant and changes the key bumps it once
    // (blocks of 16 registers that are the same skipped whole: most are, and this runs on a drawing thread for each
    // context load it is forwarded, docs/VR_SCENE_THREADS.md)
    bool bump = false;
    constexpr uint32 kBlock = 16;
    for (uint32 block = 0; block < kNumRegs && !bump; block += kBlock) {
        if (!memcmp(&regs_[block], &regs[block], kBlock * 4)) continue;
        for (uint32 reg = block; reg < block + kBlock && !bump; ++reg)
            if (regs_[reg] != regs[reg] && !(g_reg_class[reg] & kRcShaderIrrelevant) && g_key_bits.changed(reg, regs_[reg], regs[reg]))
                bump = true;
    }
    if (bump) ++shaderGen_;
    if (regs != regs_.data()) memcpy(regs_.data(), regs, kNumRegs * 4);
}
void ReplayMachine::load_blocks(const uint32_t* words, uint32_t count, uint32_t blockWords) {
    ensure_reg_tables();
    log_.reset();  // (registers replaced without logging)
    bool bump = false;
    for (uint32 b = 0; b < count; ++b) {
        const uint32_t* block = words + size_t(b) * (blockWords + 1);
        const uint32 first = block[0] * blockWords, n = std::min<uint32>(blockWords, kNumRegs - first);
        const uint32_t* values = block + 1;
        if (!memcmp(&regs_[first], values, n * 4)) continue;
        for (uint32 i = 0; i < n && !bump; ++i)
            if (regs_[first + i] != values[i] && !(g_reg_class[first + i] & kRcShaderIrrelevant) &&
                g_key_bits.changed(first + i, regs_[first + i], values[i]))
                bump = true;
        memcpy(&regs_[first], values, n * 4);
    }
    if (bump) ++shaderGen_;
}
void ReplayMachine::restore(const uint32_t* regs) {
    load_regs(regs);
    note_full_state_change();
}
// apply_regs_impl's decisions on this machine's registers (without the context shadow, the profiler's classifier
// and the statistics)
void ReplayMachine::apply(uint32_t first, const uint32_t* v, uint32_t n) {
    if (first >= kNumRegs || n > kNumRegs - first) return;
    ensure_reg_tables();
    uint32* r = regs_.data();
    if (first >= mmSQ_ALU_CONSTANT0_0 && first + n <= mmSQ_ALU_CONSTANT0_0 + 0x1000 && !g_collect_stats) {
        if (memcmp(&r[first], v, n * 4) != 0) {
            dirty_ |= 1u << kSgAluConst;
            copy_words(&r[first], v, n);
        }
        return;
    }
    auto note = [&](uint32 reg, uint8 cls, uint32 old, uint32 value) {
        const uint32 g = state_group_change(cls & kRcGroup, old, value);
        dirty_ |= 1u << g;
        const bool ends = !((kSgContinuedMask >> g) & 1);
        fullVersion_ += ends;
        if (ends) log_.add(reg, old, value, (cls & kRcGroup) == kSgStencilRefMask ? 0xFFFFFF00u : ~0u, g);
    };
#ifdef WWHD_HAS_VULKAN
    if (g_fused_small && n <= 16) {
        bool actualBump = false;
        for (uint32 i = 0; i < n; ++i) {
            const uint32 reg = first + i, value = v[i], old = r[reg];
            if (old == value) continue;
            const uint8 cls = g_reg_class[reg];
            note(reg, cls, old, value);
            if (!actualBump && !(cls & kRcShaderIrrelevant) && !(g_key_dirty && !g_vk_key_bits.changed(reg, old, value)))
                actualBump = true;
            r[reg] = value;
        }
        if (actualBump) ++shaderGen_;
        return;
    }
#endif
    if (memcmp(&r[first], v, n * 4) == 0) return;
    for (uint32 i = 0; i < n; i++)
        if (r[first + i] != v[i]) note(first + i, g_reg_class[first + i], r[first + i], v[i]);
    bool bump = false;
    if (!g_key_bits.bits.empty() || g_collect_stats) {
        for (uint32 i = 0; i < n && !bump; ++i) {
            const uint32 reg = first + i;
            if (r[reg] == v[i] || (g_reg_class[reg] & kRcShaderIrrelevant)) continue;
            if (g_key_bits.changed(reg, r[reg], v[i])) bump = true;
        }
    } else {
        for (uint32 i = 0; i < n && !bump; i++)
            if (r[first + i] != v[i] && !(g_reg_class[first + i] & kRcShaderIrrelevant)) bump = true;
    }
    if (bump) ++shaderGen_;
    memcpy(&r[first], v, n * 4);
}
// the render thread's register writes: applied, and fed to the observer; WWHD_GX2_REPLAY_VERIFY=1 checks that both
// machines decided the same
void set_register_forward(RegisterObserver* observer) { g_forward = observer; }
static void apply_regs(uint32 first, const uint32* v, uint32 n) {
    // (the common case: no draw thread, no verifier)
    if (!g_forward && !g_observer && !g_replayVerify) [[likely]] { apply_regs_impl(first, v, n); return; }
    if (g_forward && first < kNumRegs && n <= kNumRegs - first) g_forward->apply(first, v, n);
    if (replay_verify() && !g_observer) {
        static ReplayMachine verifier;
        verifier.load();
        g_observer = &verifier;
    }
    if (!g_observer) { apply_regs_impl(first, v, n); return; }
    const uint64_t gen0 = g_shader_state_gen, ver0 = g_full_state_version, rgen0 = g_observer->shader_gen(),
                   rver0 = g_observer->full_version();
    // the groups each machine notes for this write alone: both start from none (the draws take the render thread's,
    // nothing takes a verifier's), then the render thread's are put back with them
    const uint32 dirty0 = g_dirty_groups;
    const bool verify = replay_verify();
    if (verify) { g_dirty_groups = 0; g_observer->clear_dirty(); }
    apply_regs_impl(first, v, n);
    g_observer->apply(first, v, n);
    const uint32 noted = g_dirty_groups;
    if (verify) g_dirty_groups |= dirty0;
    if (replay_verify() && first < kNumRegs && n <= kNumRegs - first) {
        ++g_verifyStats.writes;
        const bool same = memcmp(&g_regs[first], g_observer->regs() + first, n * 4) == 0 &&
                          (g_shader_state_gen - gen0) == (g_observer->shader_gen() - rgen0) &&
                          (g_full_state_version - ver0) == (g_observer->full_version() - rver0) &&
                          noted == g_observer->dirty();
        if (!same && ++g_verifyStats.mismatches <= 5)
            LOG("[gx2] replay verify: write %08X+%u decided differently (generation +%llu/+%llu, version +%llu/+%llu, "
                "groups %08X/%08X)", first, n, (unsigned long long)(g_shader_state_gen - gen0),
                (unsigned long long)(g_observer->shader_gen() - rgen0), (unsigned long long)(g_full_state_version - ver0),
                (unsigned long long)(g_observer->full_version() - rver0), noted, g_observer->dirty());
        if (!same) memcpy(g_observer->regs(), g_regs, sizeof g_regs);  // (go on from the same registers)
        if (g_verifyStats.writes % 2000000 == 0)
            LOG("[gx2] replay verify: %llu register writes checked, %llu decided differently",
                (unsigned long long)g_verifyStats.writes, (unsigned long long)g_verifyStats.mismatches);
    }
}

// ---------------------------------------------------------------- display list recording
// Display lists being recorded: one slot per recording thread, found by the thread's identity (its
// thread pointer where there is one) instead of a thread_local, which cost a TLS lookup on every
// emit of every thread (in a dlopened library, as on Android, a call into the TLS resolver). A slot
// is written only by its owner; an emit while no thread records skips the scan. The owners sit
// apart from the positions, which the recording thread advances per command. Idea from the
// GreenNaugahyde Android fork (v0.9.5).
struct alignas(64) Recording {
    uint32 start = 0, pos = 0, end = 0;
    uint32 dropped = 0;  // bytes of commands that didn't fit (said once, when the list ends)
};
constexpr int kMaxRecorders = 64;
alignas(64) static std::atomic<uintptr_t> g_rec_owner[kMaxRecorders];
static Recording g_recs[kMaxRecorders];
alignas(64) static std::atomic<int> g_recording{0};  // slots in use
static std::atomic<int> g_rec_slots{0};               // slots ever claimed (the scan's end)
static inline uintptr_t self_id() { return thread_identity(); }
static inline Recording* find_rec(uintptr_t id) {
    for (int i = 0, n = g_rec_slots.load(std::memory_order_relaxed); i < n; i++)
        if (g_rec_owner[i].load(std::memory_order_relaxed) == id) return &g_recs[i];
    return nullptr;
}
static inline Recording* recording() {
    if (!g_recording.load(std::memory_order_relaxed)) return nullptr;
    return find_rec(self_id());
}
static Recording* claim_rec() {
    const uintptr_t id = self_id();
    if (Recording* r = find_rec(id)) return r;
    for (int i = 0; i < kMaxRecorders; i++) {
        uintptr_t free = 0;
        if (g_rec_owner[i].compare_exchange_strong(free, id)) {
            int n = g_rec_slots.load();
            while (n < i + 1 && !g_rec_slots.compare_exchange_weak(n, i + 1)) {}
            g_recording++;
            return &g_recs[i];
        }
    }
    LOG("[gx2] too many threads recording display lists");
    return nullptr;
}
static void release_rec(Recording* r) {
    *r = Recording{};
    g_rec_owner[r - g_recs].store(0);
    g_recording--;
}
// a display list is being recorded on this thread
bool recording_display_list() {
    const Recording* rec = recording();
    return rec && rec->start;
}
// a command that does not fit: dropped, counted for the line at GX2EndDisplayList
static void record_overflow(Recording* rec, uint32 bytes) {
    if (!rec->dropped) {
        static std::atomic<unsigned> full{0};
        if (full.fetch_add(1) < 8) LOG("[gx2] display list at %08X (%u bytes) is full; its further commands are dropped", rec->start, rec->end - rec->start);
    }
    rec->dropped += bytes;  // the total is said at GX2EndDisplayList (a line per command filled the disk)
}

static void execute_one(Op op, const uint32* p, uint32 n);

// ---------------------------------------------------------------- submitted uniform blocks
// (submitted_uniforms.h) WWHD_GX2_SUBMITTED_UNIFORMS=0 turns it off: draws in display lists then read
// uniform blocks from guest memory whenever the render thread executes them (the old behaviour).
static bool submitted_uniforms_on() {
    static const bool on = [] {
        const char* e = getenv("WWHD_GX2_SUBMITTED_UNIFORMS");
        // (VR fork) the active snapshot is the render thread's (OP_SUBMITTED_UNIFORMS, set as it reads the commands),
        // and a replaced one goes back to the game to be filled again; the draw threads (the sun atlas's, eye 1's,
        // the scene's: per_thread.h) draw later than the render thread reads, so they would read another call's
        // blocks or one being refilled. Off while a draw thread is possible, unless WWHD_GX2_SUBMITTED_UNIFORMS=1
        // (debug); the draws then read guest memory when drawn, as before the snapshots
        const bool threads = per_thread::possible() && !(e && atoi(e) == 1);
        const bool v = (!e || atoi(e) != 0) && !threads;
        LOG("[gx2] display-list uniform blocks: %s", v ? "as submitted (default)"
            : threads ? "read when executed (draw threads: their draws come after the render thread's snapshot)"
                      : "read when executed (WWHD_GX2_SUBMITTED_UNIFORMS=0)");
        return v;
    }();
    return on;
}
static submitted::DisplayListIndex g_dl_index;
// per recording slot: the blocks bound and lists called while recording
static std::vector<submitted::Range> g_rec_blocks[kMaxRecorders];
static std::vector<uint32> g_rec_calls[kMaxRecorders];
static std::mutex g_snap_mutex;
static std::vector<submitted::Snapshot*> g_snap_free;
static uint64_t g_snap_ids = 0;  // submitting thread (the game's GX2 thread)
static submitted::Snapshot* g_snap_active = nullptr;  // render thread

void note_uniform_block(uint32 addr, uint32 size) {
    if (!submitted_uniforms_on()) return;
    if (Recording* rec = recording(); rec && rec->start) g_rec_blocks[rec - g_recs].push_back({addr, size});
}
const uint8_t* submitted_uniform_block(uint32 addr, uint32 size) {
    return g_snap_active ? g_snap_active->find(addr, size) : nullptr;
}
uint64_t submitted_uniforms_id() { return g_snap_active ? g_snap_active->id() : 0; }
bool submitted_uniforms_enabled() { return submitted_uniforms_on(); }
static bool snapshot_readable(uint32 addr, uint32 size) {
    const uint64_t end = uint64_t(addr) + size + submitted::Snapshot::kTail;
    auto in = [&](uint64_t lo, uint64_t hi) { return addr >= lo && end <= hi; };
    return in(mem::kMem2Start, mem::kMem2End) || in(mem::kRuntimeStart, mem::kRuntimeEnd) ||
           in(mem::kMem1, uint64_t(mem::kMem1) + mem::kMem1Size) || in(mem::kFgBucket, uint64_t(mem::kFgBucket) + mem::kFgBucketSize);
}
// a display-list call from the command stream: its blocks as they are now go with it
static submitted::Snapshot* submit_snapshot(uint32 list, uint32 size) {
    if (!submitted_uniforms_on()) return nullptr;
    static thread_local std::vector<submitted::Range> ranges;
    ranges.clear();
    if (!g_dl_index.blocks(list, size, ranges) || ranges.empty()) return nullptr;
    submitted::Snapshot* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_snap_mutex);
        if (!g_snap_free.empty()) { s = g_snap_free.back(); g_snap_free.pop_back(); }
    }
    if (!s) s = new submitted::Snapshot;
    const size_t bytes = s->build(++g_snap_ids, ranges, [](uint32 a) { return (const uint8_t*)mem::ptr(a); }, snapshot_readable);
    // debug: WWHD_GX2_SUBMITTED_STATS=1 logs the copies made, every 1000 submissions
    static const bool stats = getenv("WWHD_GX2_SUBMITTED_STATS") != nullptr;
    if (stats) {
        static uint64_t blocks = 0, total = 0;
        blocks += s->blocks();
        total += bytes;
        if (g_snap_ids % 1000 == 0) {
            LOG("[gx2] submitted uniforms: %llu submissions, last 1000: %.1f blocks and %.1f KB each; %zu lists indexed",
                (unsigned long long)g_snap_ids, blocks / 1000.0, total / 1000.0 / 1024, g_dl_index.size());
            blocks = total = 0;
        }
    }
    return s;
}
static void set_active_snapshot(submitted::Snapshot* s) {  // render thread
    if (g_snap_active) {
        std::lock_guard<std::mutex> lk(g_snap_mutex);
        g_snap_free.push_back(g_snap_active);
    }
    g_snap_active = s;
}

// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into Metal work. WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv, g_q_done_cv;
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;
static uint64_t g_fence_issued = 0, g_fence_done = 0;
static std::atomic<uint64_t> g_swaps_executed{0};  // swaps the render thread has executed (OP_SWAP)

static int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void render_thread_main() {
    host::set_thread_name("GX2 render");
    // the game thread waits at its swap for depth-peek answers: wake this thread's command wait so it
    // retires the copies (gfx/depth_peek.h)
    gfx::depth_peek::set_render_wake([] { { std::lock_guard<std::mutex> lk(g_q_mutex); } g_q_cv.notify_all(); });
    if (!r5_slow_render::apply("GX2 render")) host::boost_thread_priority();
    core_groups::place_current_thread(core_groups::Role::Scene, "render");  // (WWHD_VK_PLACE_THREADS=0: not placed)
    perf_hint::add_current_thread();
    thread_sampler::add_current_thread("render");
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            if (g_q_pending.empty()) {
                // work held for later is done before the thread waits (the game may be waiting for it)
                lk.unlock();
                {
                    std::lock_guard<std::recursive_mutex> ex(g_exec_mutex);
                    render::before_idle();
                }
                lk.lock();
            }
            if (g_q_pending.empty()) {
                const uint64_t idle = rprof::enabled() ? rprof::now_ns() : 0;
                // even frame delivery: a held frame is presented at its time while no commands come
                // (a timed wait wakes up to a millisecond late: it ends 0.3 ms early, the rest yields)
                for (int64_t due; g_q_pending.empty() && (due = render::present_due()) != 0;) {
                    const int64_t now = steady_ns();
                    if (now < due - 300'000) {
                        g_q_cv.wait_until(lk, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(due - 300'000)));
                        continue;
                    }
                    if (now < due) {
                        lk.unlock();
                        std::this_thread::yield();
                        lk.lock();
                        continue;
                    }
                    lk.unlock();
                    {
                        std::lock_guard<std::recursive_mutex> ex(g_exec_mutex);
                        render::with_autorelease_pool([] { render::service_present(); });
                    }
                    lk.lock();
                }
                // the game waits at its swap for depth-peek answers: retire their copy now (depth_peek.h)
                while (g_q_pending.empty() && gfx::depth_peek::wanted()) {
                    lk.unlock();
                    {
                        std::lock_guard<std::recursive_mutex> ex(g_exec_mutex);
                        render::with_autorelease_pool([] { render::peek_retire(); });
                    }
                    lk.lock();
                    if (gfx::depth_peek::wanted()) g_q_cv.wait_for(lk, std::chrono::microseconds(200));
                }
                // VR: while no commands come (the game thread loading a stage or stalled), the headset is given its last
                // frame again at its rate (vr_vulkan.h headset_frame), so the runtime does not time the game out
                for (;;) {
                    const int64_t due = render::headset_frame_due();
                    if (!due) {
                        g_q_cv.wait(lk, [] { return !g_q_pending.empty() || gfx::depth_peek::wanted(); });
                        break;
                    }
                    if (g_q_cv.wait_until(lk, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(due)),
                                          [] { return !g_q_pending.empty() || gfx::depth_peek::wanted(); }))
                        break;
                    lk.unlock();
                    {
                        std::lock_guard<std::recursive_mutex> ex(g_exec_mutex);
                        render::headset_frame();
                    }
                    lk.lock();
                }
                if (g_q_pending.empty()) { lk.unlock(); continue; }  // woken for depth-peek answers: retire them
                if (idle) rprof::add_idle(rprof::now_ns() - idle);
            }
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            render::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
    }
}

static void start_render_thread() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread([] { exception_report::boundary("GX2 render thread", render_thread_main); }).detach(); });
}
// ---- the commands GX2DrawDone protects (docs/VR_PERFORMANCE.md, "PERF-07a")
// GX2DrawDone makes the game wait until the render thread has executed everything queued: after it returns, the game
// may rewrite the memory those commands read, and read what they wrote back. A command reads guest memory on the
// render thread (draws: their vertices, indices, uniforms, shaders, textures; display lists; copies; depth peeks) or may
// write it back (draws and clears to linear targets, a full GPU wait): those are "guarded". The others carry their data
// in the queue (register writes, the scan copies, the swap, flushes) or change only the render thread's own state: a
// context switch selects its shadow of that context, and an invalidate only marks cached surfaces dirty and hints the
// buffer cache's write watch (the data is read by the draws that use it, which are guarded). Measured before the swap's
// wait: register writes, a scan copy, 2 context switches, 2 invalidates, a flush, the swap. A GX2DrawDone with no guarded command
// queued since the last completed wait has nothing to protect, and returns at once (the swap's wait: the game goes on
// with its next frame while the render thread presents). Unknown commands count as guarded.
static bool guarded(uint32 op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: case OP_SET_REGS: case OP_COPY_TO_SCAN: case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: case OP_FLUSH:
    case OP_SWAP: case OP_SETUP_CONTEXT: case OP_FENCE: case OP_SET_PROJ_REGS: case OP_LAYOUT_ROOT:
    case OP_LAYOUT_CONTENT: case OP_CLOUDS_BEGIN: case OP_CLOUDS_END: case OP_SET_CONTEXT: case OP_INVALIDATE:
    case OP_WEATHER_QUADS:
        return false;
    case OP_DRAW_DONE: return n && p[0];  // (a full GPU wait reads linear targets back: save states)
    default: return true;
    }
}
static uint64_t g_guarded_queued = 0;  // guarded commands queued so far (g_q_mutex)
static uint64_t g_guarded_done = 0;    // of them, executed before a completed wait (g_q_mutex)
static uint32_t g_ops_since[OP_COUNT + 1] = {};  // (diagnostics) commands queued since the last GX2DrawDone, by kind
static void count_queued(uint32 op, const uint32* p, uint32 n) {  // (g_q_mutex held)
    if (guarded(op, p, n)) ++g_guarded_queued;
    ++g_ops_since[op < OP_COUNT ? op : OP_COUNT];
}
static void enqueue(Op op, const uint32* payload, uint32 n) {
    start_render_thread();
    std::lock_guard<std::mutex> lk(g_q_mutex);
    count_queued(op, payload, n);
    g_q_pending.push_back(op | (n << 8));
    g_q_pending.insert(g_q_pending.end(), payload, payload + n);
    if (g_q_waiting) g_q_cv.notify_one();
}
static void enqueue_regs(uint32 first, const uint32* values, uint32 count) {  // OP_SET_REGS {first, values...}
    start_render_thread();
    std::lock_guard<std::mutex> lk(g_q_mutex);
    count_queued(OP_SET_REGS, nullptr, 0);
    g_q_pending.push_back(OP_SET_REGS | ((count + 1) << 8));
    g_q_pending.push_back(first);
    g_q_pending.insert(g_q_pending.end(), values, values + count);
    if (g_q_waiting) g_q_cv.notify_one();
}

// block the game thread until the render thread has executed everything queued so far
// debug: WWHD_SYNC_STATS=1 logs, every 5 s, how often each caller waited for the render thread to
// catch up (render_sync) and for how long
enum SyncSite { kSyncShutdown, kSyncFlip, kSyncDrawDone, kSyncVsyncUncapped, kSyncVsyncFlip, kSyncSaveState, kSyncCopySurface, kSyncSites };
static void sync_stat(int site, std::chrono::steady_clock::duration waited) {
    static const bool on = getenv("WWHD_SYNC_STATS") != nullptr;
    if (!on) return;
    static std::mutex mu;
    static uint64_t count[kSyncSites], ns[kSyncSites];
    static auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    count[site]++;
    ns[site] += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - t0).count();
    static const char* names[kSyncSites] = {"shutdown", "flip", "DrawDone", "vsync-uncapped", "vsync-flip", "savestate", "CopySurface"};
    char buf[400];
    int k = snprintf(buf, sizeof buf, "[gx2] render_sync per second:");
    for (int i = 0; i < kSyncSites; i++)
        if (count[i]) k += snprintf(buf + k, sizeof buf - k, " %s %.1f x %.2f ms", names[i], count[i] / secs, ns[i] / 1e6 / count[i]);
    LOG("%s", buf);
    memset(count, 0, sizeof count); memset(ns, 0, sizeof ns);
    t0 = now;
}
static void render_sync(int site = kSyncShutdown) {
    if (!g_render_thread) return;
    const auto started = std::chrono::steady_clock::now();
    struct Done {
        int site;
        std::chrono::steady_clock::time_point t;
        ~Done() {
            const auto waited = std::chrono::steady_clock::now() - t;
            sync_stat(site, waited);
            if (rprof::enabled())
                rprof::add_sync(site == kSyncDrawDone ? rprof::kSyncDrawDone : site == kSyncCopySurface ? rprof::kSyncCopySurface
                                : site == kSyncFlip || site == kSyncVsyncFlip ? rprof::kSyncFlip : rprof::kSyncOther,
                                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
        }
    } done{site, started};
    uint64_t id, covers;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        id = ++g_fence_issued;
        covers = g_guarded_queued;  // (everything queued before the fence is executed when it is reached)
    }
    uint32 w = (uint32)id;
    enqueue(OP_FENCE, &w, 1);
    std::unique_lock<std::mutex> lk(g_q_mutex);
    g_q_done_cv.wait(lk, [&] { return g_fence_done >= id; });
    g_guarded_done = std::max(g_guarded_done, covers);
}
// nothing GX2DrawDone protects is queued and not yet executed (PERF-07a)
static bool nothing_guarded_pending() {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    return g_guarded_queued == g_guarded_done;
}

std::string (*g_draw_context_provider)() = nullptr;
static std::mutex g_draw_context_mutex;
static std::vector<std::string> g_draw_context_texts{"-"};
static std::unordered_map<std::string, uint32_t> g_draw_context_ids;
static std::atomic<uint32_t> g_draw_context{0};
std::string current_draw_context() {
    std::lock_guard<std::mutex> lk(g_draw_context_mutex);
    const uint32_t id = g_draw_context.load(std::memory_order_relaxed);
    return id < g_draw_context_texts.size() ? g_draw_context_texts[id] : "-";
}
void emit(Op op, const uint32* payload, uint32 n) {
    static thread_local bool describing = false;
    if (g_draw_context_provider && !describing && (op == OP_DRAW || op == OP_DRAW_INDEXED || op == OP_CALL) &&
        !(recording() && recording()->start)) {
        describing = true;
        const std::string text = g_draw_context_provider();
        uint32 id;
        {
            std::lock_guard<std::mutex> lk(g_draw_context_mutex);
            auto [it, fresh] = g_draw_context_ids.try_emplace(text, uint32_t(g_draw_context_texts.size()));
            if (fresh) g_draw_context_texts.push_back(text);
            id = it->second;
        }
        emit(OP_DEBUG_CONTEXT, &id, 1);
        describing = false;
    }
    if (Recording* rec = recording(); rec && rec->start) {
        uint32 bytes = 4 * (n + 1);
        if (rec->pos + bytes > rec->end) {
            record_overflow(rec, bytes);
            return;
        }
        uint32* w = (uint32*)mem::ptr(rec->pos);
        w[0] = op | (n << 8);
        memcpy(w + 1, payload, n * 4);
        rec->pos += bytes;
        return;
    }
    if (g_render_thread) {
        enqueue(op, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload, n); });
}

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload.begin(), (uint32)payload.size()); });
}

// 60 fps (generated, framegen.h): the camera of the frame being drawn, host-only (also while a display
// list is being recorded: the camera belongs to the frame, not to the list). Never called while off.
void framegen_camera(const uint32* words, uint32 n) {
    if (g_render_thread) {
        enqueue(OP_FRAMEGEN_CAMERA, words, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(OP_FRAMEGEN_CAMERA, words, n); });
}

#ifdef WWHD_HAS_VULKAN
static gfxvk::BackgroundCheckpoint& background_cache_checkpoint() {
    static gfxvk::BackgroundCheckpoint worker([] {
        host::set_thread_name("Vulkan cache save");
        // If a periodic write was already in flight, wait outside execution
        // and take a second snapshot for any newer revision before returning.
        for (int pass = 0; pass < 2; ++pass) {
            std::function<void()> wait;
            {
                std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
                host::with_autorelease_pool([&] { wait = gfxvk::background_renderer_caches(); });
            }
            if (wait) wait();
        }
    });
    return worker;
}
void request_background_vulkan_cache_checkpoint() {
    // SDL can send lifecycle events while main-thread Vulkan initialization is still running.
    // Do not create the saver or inspect renderer/cache objects until publication completes.
    if (!gfxvk::background_renderer_caches_ready()) return;
    background_cache_checkpoint().request();
}
#ifdef __ANDROID__
void quiesce_quest_presentation() {
    if (!gfxvk::background_renderer_caches_ready()) return;
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    gfxvk::wait_idle();
}
#endif
void checkpoint_present_observer() {
    if (!present_observer::enabled()) return;
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] { gfxvk::flush_present_observer(); });
}
void checkpoint_vulkan_caches() {
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] { gfxvk::save_renderer_caches(); });
}
void shutdown_vulkan_caches() {
    // The background saver may be waiting for g_exec_mutex. Join before locking.
    gfxvk::disable_background_renderer_caches();
    background_cache_checkpoint().stop();
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] { gfxvk::shutdown_renderer_caches(); });
}
#endif

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (!count) return;
    // the command is the first register and the values: written in place (no staging copy)
    const uint32 n = count + 1;
    if (Recording* rec = recording(); rec && rec->start) {
        const uint32 bytes = 4 * (n + 1);
        if (rec->pos + bytes > rec->end) {
            record_overflow(rec, bytes);
            return;
        }
        uint32* w = (uint32*)mem::ptr(rec->pos);
        w[0] = OP_SET_REGS | (n << 8);
        w[1] = first;
        memcpy(w + 2, values, count * 4);
        rec->pos += bytes;
        return;
    }
    if (g_render_thread) {
        enqueue_regs(first, values, count);
        return;
    }
    std::vector<uint32> buf(n);  // WWHD_NO_RENDER_THREAD (debug)
    buf[0] = first;
    memcpy(&buf[1], values, count * 4);
    emit(OP_SET_REGS, buf.data(), n);
}
void set_reg(uint32 reg, uint32 value) { emit(OP_SET_REGS, {reg, value}); }

// A register write that changes nothing (issue #1: about two thirds of a busy frame's register writes, mostly one-word
// context registers the display lists set again for every draw): neither the registers nor the context's shadow would
// change, so it is a no-op for this machine, a held run, the profiler's draw classifier and a draw thread's forwarded
// machine (which mirrors these registers). It is dropped here, before the op's dispatch, application and forwarding.
// WWHD_GX2_SKIP_SAME=0 applies it as before (diagnostics, A/Bs)
static const bool g_skipSame = [] {
    const char* e = getenv("WWHD_GX2_SKIP_SAME");
    return !(e && !strcmp(e, "0"));
}();
// (most of these writes are one or two words: compared in place, not through memcmp; checked in the display list's
// loop, before the op's dispatch: execute)
static inline __attribute__((always_inline)) bool same_regs(const uint32* p, uint32 n) {
    if (n < 2 || n > 17) return false;
    const uint32 first = p[0], count = n - 1;
    if (first >= kNumRegs || count > kNumRegs - first || g_observer || g_replayVerify) [[unlikely]] return false;
    const uint32* v = p + 1;
    const uint32* r = &g_regs[first];
    for (uint32 i = 0; i < count; ++i)
        if (r[i] != v[i]) return false;
    if (const uint32* s = g_shadow ? &g_shadow[first] : nullptr)
        for (uint32 i = 0; i < count; ++i)
            if (s[i] != v[i]) return false;
    g_register_blocks.touch(first, count);  // (as apply_regs_impl: the block is restored with a context from now on)
    return true;
}
void execute(const uint32* words, uint32 count) {
    uint32 i = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || i + 1 + n > count) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        if (op != OP_SET_REGS || !g_skipSame || !same_regs(&words[i + 1], n)) execute_one(op, &words[i + 1], n);
        i += 1 + n;
    }
}

static void set_context(uint32 ctx) {
    if (!ctx) {
        g_shadow = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        return;
    }
    g_shadow = it->second.data();
    // Draw writes primitive type without updating the context shadow.
    g_register_blocks.touch(uint32(REGADDR::VGT_PRIMITIVE_TYPE), 1);
    ensure_reg_tables();
    if (g_register_blocks.restore(g_regs, g_shadow, [](uint32 reg, uint32 old, uint32 value) {
        return !(g_reg_class[reg] & kRcShaderIrrelevant) && g_key_bits.changed(reg, old, value);
    })) ++g_shader_state_gen;
    if (g_observer) g_observer->load_regs(g_regs);  // (its full-state change: note_full_state_change, below)
    if (g_forward) {  // (only the blocks restore may have changed: docs/VR_SCENE_THREADS.md, "register loads by block")
        const auto& blocks = g_register_blocks.touched_blocks();
        g_forward->load_blocks(g_regs, blocks.data(), uint32_t(blocks.size()), uint32_t(g_register_blocks.kBlockWords));
    }
    note_full_state_change();
    rprof::g_reg_dirty |= 2;  // draw classifier: a context load counts as a full state change
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}

// Vulkan: GX2DrawDone queues the work instead of waiting for an idle device (the default on every
// platform since 2026-10-07; WWHD_VK_LAZY_DRAW_DONE=0 restores the full wait). The Metal renderer
// reads large vertex buffers straight from guest memory, so it keeps the real GPU wait.
static bool lazy_draw_done() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_LAZY_DRAW_DONE");
        return render::vulkan() && (!e || atoi(e) != 0);
    }();
    return on;
}

static void execute_op(Op op, const uint32* p, uint32 n);
static void test_skip_log();  // (WWHD_TEST_SKIP_BATCHABLE, below)
static void reg_stats_swap();  // (WWHD_GX2_REG_STATS, below)

// Batched same-state runs (gfx/draw_batch.h; issue #1, docs/VR_PERFORMANCE.md "Batched same-state runs"): a draw that
// may start one is held; the next draws with the same arguments join it while nothing but vertex-shader uniform block
// addresses changes in between (the grass: a tuft a draw, each with its own matrix block). Any other op, or a register
// write that changes anything else, first draws the held run: as one instanced draw if the renderer takes it, else
// draw by draw with each piece's own block addresses, exactly as before. On with the Vulkan renderer;
// WWHD_VK_BATCH_RUNS=0 turns it off
namespace {
struct Held {
    bool on = false;
    Op op = OP_NOP;
    uint32 n = 0;
    uint32 args[6] = {};
    draw_batch::Batch batch;
};
Held g_held;  // (the executing thread's: ops run one at a time)
// the last draw drawn here (not held) that could begin a run: a run is held from its second draw on, when the next
// draw repeats it with only vertex-shader uniform block addresses changed in between (held_or_draw). So draws that
// begin no run cost a few compares, never a hold
constexpr uint32 kBlockWords = 16 * 7;
constexpr uint32 kBlockRegions[3] = {mmSQ_VTX_UNIFORM_BLOCK_START, mmSQ_PS_UNIFORM_BLOCK_START,
                                     mmSQ_GS_UNIFORM_BLOCK_START};
struct LastDraw {
    bool valid = false;
    Op op = OP_NOP;
    uint32 n = 0;
    uint32 args[6] = {};
    uint32 blocks[3][kBlockWords] = {};  // the uniform block words then
};
LastDraw g_last;
// the arguments of the last draw drawn here: only a draw that repeats them is kept in g_last (its block words copied)
struct PrevArgs { Op op = OP_NOP; uint32 n = 0; uint32 args[6] = {}; };
PrevArgs g_prev;
constexpr uint32 kVsBlocks = mmSQ_VTX_UNIFORM_BLOCK_START;
inline bool vs_block_addr(uint32 reg) {
    return reg >= kVsBlocks && reg < kVsBlocks + 16 * 7 && (reg - kVsBlocks) % 7 == 0;
}
inline void vs_block_addrs(uint32* out) {
    for (uint32 i = 0; i < 16; ++i) out[i] = g_regs[kVsBlocks + i * 7];
}
// (a held run's pieces, drawn: the sun atlas's thread draws from its own copy of the registers, so it gets the
// addresses too, in order with the draws: g_forward)
inline void set_vs_block_addrs(const uint32* addrs) {
    for (uint32 i = 0; i < 16; ++i) {
        uint32& reg = g_regs[kVsBlocks + i * 7];
        if (reg == addrs[i]) continue;
        reg = addrs[i];
        if (g_forward) g_forward->apply(kVsBlocks + i * 7, &addrs[i], 1);
    }
}
bool held_enabled() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_BATCH_RUNS");
        const char* skip = getenv("WWHD_TEST_SKIP_BATCHABLE");
        const bool v = render::vulkan() && !(e && !strcmp(e, "0")) && !(skip && atoi(skip));
        LOG("[gx2] batched same-state runs %s", v ? "on (WWHD_VK_BATCH_RUNS=0 turns them off)" : "off");
        return v;
    }();
    return on;
}
// the uniform block words as at the last draw, but for vertex-shader block addresses
bool only_vs_addrs_changed() {
    for (uint32 b = 0; b < 3; ++b) {
        const uint32* now = &g_regs[kBlockRegions[b]];
        if (!memcmp(now, g_last.blocks[b], kBlockWords * 4)) continue;
        if (b) return false;
        for (uint32 w = 0; w < kBlockWords; ++w)
            if (now[w] != g_last.blocks[0][w] && w % 7) return false;
    }
    return true;
}
// a register write that ends the held run: it changes a register other than a vertex-shader uniform block's address
bool held_breaks(uint32 first, const uint32* v, uint32 n) {
    if (first >= kNumRegs || n > kNumRegs - first) return true;
    if (!memcmp(&g_regs[first], v, n * 4)) return false;
    for (uint32 i = 0; i < n; ++i)
        if (g_regs[first + i] != v[i] && !vs_block_addr(first + i)) return true;
    return false;
}
}  // namespace
static void held_draw(Op op, const uint32* a, uint32 instances) {
    if (op == OP_DRAW) render::draw(g_regs, a[0], a[1], 0, 0, a[2], instances);
    else render::draw(g_regs, a[0], a[1], a[2], a[3], a[4], instances);
}
// (diagnostics) WWHD_VK_BATCH_STATS=1: what ended the held runs of 2 and more draws, every 600 frames
namespace {
const bool g_heldStats = [] { const char* e = getenv("WWHD_VK_BATCH_STATS"); return e && !strcmp(e, "1"); }();
std::unordered_map<uint32, uint64_t> g_heldEnds;  // op, or 0x10000 | the first changed register of a write
std::unordered_map<uint32, uint64_t> g_heldByVs, g_notHeldByVs;  // runs' pieces by vertex program; draws not held
uint64_t g_heldRuns = 0, g_heldPieces = 0, g_heldFlushNs = 0, g_heldFlushes = 0;
void held_note_end(uint32 why) {
    if (g_held.batch.count < 2) return;
    ++g_heldEnds[why];
    g_heldByVs[g_regs[mmSQ_PGM_START_VS]] += g_held.batch.count;
    ++g_heldRuns;
    g_heldPieces += g_held.batch.count;
}
void held_stats_log() {
    static uint64_t swaps = 0;
    if (++swaps % 600) return;
    std::vector<std::pair<uint64_t, uint32>> v;
    for (auto& [k, c] : g_heldEnds) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
        char b[48];
        if (v[i].second & 0x10000) snprintf(b, sizeof b, " reg %04X: %.1f", v[i].second & 0xFFFF, v[i].first / 600.0);
        else snprintf(b, sizeof b, " op %u: %.1f", v[i].second, v[i].first / 600.0);
        line += b;
    }
    LOG("[gx2] held runs of 2+ per frame: %.1f (%.0f draws), drawn in %.3f ms (%.1f us a run); ended by:%s",
        g_heldRuns / 600.0, g_heldPieces / 600.0, g_heldFlushNs / 600.0 / 1e6,
        g_heldFlushes ? g_heldFlushNs / 1e3 / double(g_heldFlushes) : 0.0, line.c_str());
    g_heldFlushNs = g_heldFlushes = 0;
    for (auto* m : {&g_heldByVs, &g_notHeldByVs}) {
        std::vector<std::pair<uint64_t, uint32>> w;
        for (auto& [k, c] : *m) w.push_back({c, k});
        std::sort(w.rbegin(), w.rend());
        std::string l;
        for (size_t i = 0; i < w.size() && i < 8; ++i) {
            char b[48];
            snprintf(b, sizeof b, " VS %08X: %.0f", w[i].second << 8, w[i].first / 600.0);
            l += b;
        }
        LOG("[gx2]   %s per frame:%s", m == &g_heldByVs ? "held runs' draws" : "draws not held (renderer)", l.c_str());
        m->clear();
    }
    g_heldEnds.clear();
    g_heldRuns = g_heldPieces = 0;
}
}  // namespace
// the held run drawn: as one draw of all its pieces if the renderer takes it, else piece by piece
static void held_flush() {
    if (!g_held.on) return;
    g_held.on = false;
    g_last.valid = false;
    const draw_batch::Batch& b = g_held.batch;
    struct Timed {
        uint64_t t0 = g_heldStats ? steady_ns() : 0;
        ~Timed() { if (g_heldStats) g_heldFlushNs += steady_ns() - t0, ++g_heldFlushes; }
    } timed;
    uint32 saved[16];
    vs_block_addrs(saved);
    struct Restore {
        const uint32* saved;
        ~Restore() {
            draw_batch::current() = nullptr;
            set_vs_block_addrs(saved);
        }
    } restore{saved};
    bool done = false;
    if (b.count > 1) {
        set_vs_block_addrs(b.addrs[0]);
        draw_batch::consumed() = false;
        draw_batch::current() = &b;
        held_draw(g_held.op, g_held.args, b.count);
        draw_batch::current() = nullptr;
        done = draw_batch::consumed();
    }
    for (uint32 k = 0; !done && k < b.count; ++k) {
        set_vs_block_addrs(b.addrs[k]);
        held_draw(g_held.op, g_held.args, 1);
    }
}
// a draw: joins the held run, starts one, or is drawn
static bool g_statHeld = false;  // (the draw-run stats: the draw just went into a same-state run, drawn instanced)
static void held_or_draw_now(Op op, const uint32* p, uint32 n) {
    const uint32 instances = op == OP_DRAW ? p[3] : p[5];
    if (g_held.on) {
        draw_batch::Batch& b = g_held.batch;
        if (op == g_held.op && n == g_held.n && !memcmp(p, g_held.args, n * 4) && b.count < draw_batch::kMaxPieces) {
            uint32* addrs = b.addrs[b.count++];
            vs_block_addrs(addrs);
            for (uint32 i = 0; i < 16; ++i)
                if (addrs[i] != b.addrs[0][i]) b.blocks |= 1u << i;
            g_statHeld = true;
            return;
        }
        if (g_heldStats) held_note_end(0x20000 | (op == g_held.op && !memcmp(p, g_held.args, n * 4) ? 1 : 0));
        held_flush();
    }
    const bool candidate = instances == 1 && n <= 6 && !g_observer && held_enabled();
    // a run's second draw: held, with the ones after it
    if (candidate && g_last.valid && op == g_last.op && n == g_last.n && !memcmp(p, g_last.args, n * 4) &&
        !(g_dirty_groups & ~(1u << kSgUniformBlock)) &&
        (!(g_dirty_groups & 1u << kSgUniformBlock) || only_vs_addrs_changed())) {
        g_last.valid = false;
        g_held.on = true;
        g_held.op = op;
        g_held.n = n;
        memcpy(g_held.args, p, n * 4);
        g_held.batch.count = 1;
        g_held.batch.blocks = 0;
        vs_block_addrs(g_held.batch.addrs[0]);
        g_statHeld = true;
        return;
    }
    if (g_heldStats && candidate) ++g_notHeldByVs[g_regs[mmSQ_PGM_START_VS]];
    held_draw(op, p, instances);
    // (the draw took the dirty groups: what changes from here on is gathered for the next draw. A draw handed to
    // another thread, the sun atlas's, took none here: they are dropped as a draw would take them)
    g_dirty_groups = 0;
    const bool repeats = op == g_prev.op && n == g_prev.n && !memcmp(p, g_prev.args, n * 4);
    if (!repeats && n <= 6) {
        g_prev.op = op;
        g_prev.n = n;
        memcpy(g_prev.args, p, n * 4);
    }
    g_last.valid = candidate && repeats && draw_batch::can_hold() && draw_batch::can_hold()(g_regs);
    if (g_last.valid) {
        g_last.op = op;
        g_last.n = n;
        memcpy(g_last.args, p, n * 4);
        for (uint32 b = 0; b < 3; ++b) memcpy(g_last.blocks[b], &g_regs[kBlockRegions[b]], kBlockWords * 4);
    }
}
// ---- draw runs (docs/VR_PERFORMANCE.md, "Index-range runs and unseen GamePad draws"): consecutive draws of the same
// kind with no register changed and no other op between them are drawn as one draw. Indexed triangle lists and strips
// (one instance, the same index type) wherever their indices are and whatever their base vertex; non-indexed lists
// (points, lines, triangles, rects, quads) whose vertices follow on. A list whose pieces are one range with one base
// vertex is drawn as it is; otherwise the indices are copied into a scratch ring: strips stitched (the last index of
// one and the first of the next repeated, plus one more where the next would start on an odd index: triangles with no
// area, the pieces' own winding kept), different base vertices folded into the indices (as 32-bit indices, from the
// smallest base). A restart index, or a run too large for the ring: piece by piece. A run of one is drawn as any draw.
// In a busy VR scene 1,792 of the main pass's 3,500 draws a frame were such runs. WWHD_VK_INDEX_RUNS=0 turns them off;
// WWHD_VK_INDEX_RUN_STATS=1 logs, by shader, what came between draws of the same shader that were not drawn as one
namespace {
constexpr uint32 kPrimPoints = 1, kPrimLines = 2, kPrimTriangles = 4, kPrimTriangleStrip = 6, kPrimRects = 0x11,
                 kPrimQuads = 0x13;
constexpr uint32 kIndexRunMax = 256;              // pieces in a run
constexpr uint32 kIndexScratchBytes = 8u << 20;   // the copied indices' ring (several frames' worth)
constexpr uint32 kPieceMax = 1u << 20;            // elements in a piece
struct IndexRun {
    bool on = false;
    Op op = OP_DRAW_INDEXED;
    uint32 n = 6;
    uint32 args[6] = {};  // the first piece's
    uint32 pieces = 0, total = 0;
    uint32 counts[kIndexRunMax] = {}, addrs[kIndexRunMax] = {}, bases[kIndexRunMax] = {};  // addrs: indices, or first vertex
    bool contiguous = true, oneBase = true;
};
IndexRun g_irun;
uint64_t g_irunRuns = 0, g_irunPieces = 0, g_irunCopied = 0, g_irunReused = 0, g_irunFrame0 = 0;
inline uint32 index_size(uint32 type) { return type == 1 || type == 9 ? 4 : 2; }  // GX2_INDEX_TYPE_U32(_LE)
inline bool index_le(uint32 type) { return type == 0 || type == 1; }             // GX2_INDEX_TYPE_U16_LE, U32_LE
inline bool index_type_known(uint32 type) { return type == 0 || type == 1 || type == 4 || type == 9; }
inline uint32 list_vertices(uint32 prim) {
    switch (prim) {
    case kPrimPoints: return 1;
    case kPrimLines: return 2;
    case kPrimTriangles: case kPrimRects: return 3;
    case kPrimQuads: return 4;
    default: return 0;
    }
}
bool index_runs_enabled() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_INDEX_RUNS");
        const bool v = render::vulkan() && !(e && !strcmp(e, "0"));
        LOG("[gx2] draw runs %s", v ? "drawn as one draw (WWHD_VK_INDEX_RUNS=0 turns them off)" : "off");
        return v;
    }();
    return on;
}
bool run_shape(Op op, const uint32* p, uint32 n) {
    if (op == OP_DRAW_INDEXED)
        return n == 6 && (p[0] == kPrimTriangles || p[0] == kPrimTriangleStrip) && p[5] == 1 && p[1] >= 3 &&
               p[1] < kPieceMax && index_type_known(p[2]);
    const uint32 v = op == OP_DRAW && n == 4 ? list_vertices(p[0]) : 0;
    return v && p[3] == 1 && p[1] && p[1] < kPieceMax && p[1] % v == 0;
}
bool index_run_candidate(Op op, const uint32* p, uint32 n) {
    return run_shape(op, p, n) && !g_observer && !g_held.on && index_runs_enabled();
}
bool index_run_continues(Op op, const uint32* p, uint32 n) {
    const IndexRun& r = g_irun;
    if (op != r.op || n != r.n || p[0] != r.args[0] || r.pieces >= kIndexRunMax || !run_shape(op, p, n) ||
        r.total + p[1] > kIndexScratchBytes / 16)
        return false;
    if (op == OP_DRAW_INDEXED) return p[2] == r.args[2];
    return p[2] == r.addrs[r.pieces - 1] + r.counts[r.pieces - 1];  // (non-indexed: the vertices follow on)
}
void index_run_add(Op op, const uint32* p) {
    IndexRun& r = g_irun;
    const uint32 k = r.pieces++;
    r.counts[k] = p[1];
    r.total += p[1];
    if (op == OP_DRAW_INDEXED) {
        r.addrs[k] = p[3], r.bases[k] = p[4];
        if (k) {
            r.contiguous &= p[3] == r.addrs[k - 1] + r.counts[k - 1] * index_size(p[2]);
            r.oneBase &= p[4] == r.bases[0];
        }
    } else {
        r.addrs[k] = p[2], r.bases[k] = 0;
    }
}
// A run's copy kept for the frames after (docs/VR_PERFORMANCE.md, "Copied run indices kept"): a run copied again
// with the same bytes is given the earlier copy's address instead of a new one, so the renderer's buffer cache finds
// its converted indices there (a new address each frame was a miss for every such run: the range armed, converted and
// uploaded under the cache's shared lock, about 500 runs a frame in busy views). Exact: the bytes are compared, and the
// cache still checks the copy's pages for writes. A copy is reused only while the ring has moved on less than half
// its size since it was written (a new copy now would last a whole ring), so a draw still reading it on a drawing
// thread is as far from being overwritten as before. WWHD_VK_INDEX_RUN_REUSE=0 turns it off
struct KeptCopy { uint64_t pos = 0; uint32_t at = 0, bytes = 0, type = 0, count = 0; };
std::unordered_map<uint64_t, KeptCopy> g_keptCopies;  // by the run's shape: its pieces' counts, addresses, bases
bool index_reuse_enabled() {
    static const bool on = [] { const char* e = getenv("WWHD_VK_INDEX_RUN_REUSE"); return !(e && !strcmp(e, "0")); }();
    return on;
}
uint64_t run_shape_key(const IndexRun& r) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; h ^= h >> 29; };
    mix(r.args[0] | uint64_t(r.args[2]) << 32);
    mix(r.pieces);
    for (uint32 k = 0; k < r.pieces; ++k) mix(r.counts[k] | uint64_t(r.addrs[k]) << 32), mix(r.bases[k]);
    return h;
}
// the run's indices copied into the scratch ring (see above); 0 when they cannot be
uint32 gather_indices(uint32& outType, uint32& outCount) {
    static uint32 ring = 0, at = 0;
    static uint64_t head = 0;  // bytes the ring has moved on in all (the skipped end of each lap included)
    static std::vector<uint8_t> scratch;
    const IndexRun& r = g_irun;
    const uint32 inType = r.args[2], inSize = index_size(inType);
    const bool le = index_le(inType), strip = r.args[0] == kPrimTriangleStrip;
    uint32 minBase = r.bases[0];
    for (uint32 k = 1; k < r.pieces; ++k) minBase = std::min(minBase, r.bases[k]);
    outType = r.oneBase ? inType : le ? 1 : 9;
    const uint32 outSize = index_size(outType);
    const uint64_t bytes = (uint64_t(r.total) + (strip ? 3ull * r.pieces : 0)) * outSize;
    if (bytes > kIndexScratchBytes / 4) return 0;
    if (!ring) ring = mem::host_alloc(kIndexScratchBytes, 0x100);
    if (scratch.size() < bytes) scratch.resize(size_t(bytes));
    uint8_t* out = scratch.data();  // (gathered here, then compared with a kept copy or copied into the ring)
    const uint32 restart = inSize == 4 ? 0xFFFFFFFFu : 0xFFFFu;
    auto get = [&](const uint8_t* e) -> uint32 {
        if (inSize == 2) return le ? uint32(e[0] | e[1] << 8) : uint32(e[0] << 8 | e[1]);
        return le ? uint32(e[0] | e[1] << 8 | e[2] << 16 | uint32(e[3]) << 24)
                  : uint32(uint32(e[0]) << 24 | e[1] << 16 | e[2] << 8 | e[3]);
    };
    uint32 len = 0, last = 0;
    auto put = [&](uint32 v) {
        uint8_t* e = out + outSize * len++;
        if (outSize == 2) {
            if (le) e[0] = uint8_t(v), e[1] = uint8_t(v >> 8);
            else e[0] = uint8_t(v >> 8), e[1] = uint8_t(v);
        } else if (le) {
            e[0] = uint8_t(v), e[1] = uint8_t(v >> 8), e[2] = uint8_t(v >> 16), e[3] = uint8_t(v >> 24);
        } else {
            e[0] = uint8_t(v >> 24), e[1] = uint8_t(v >> 16), e[2] = uint8_t(v >> 8), e[3] = uint8_t(v);
        }
        last = v;
    };
    for (uint32 k = 0; k < r.pieces; ++k) {
        const uint8_t* in = (const uint8_t*)mem::ptr(r.addrs[k]);
        const uint64_t delta = r.bases[k] - minBase;
        for (uint32 i = 0; i < r.counts[k]; ++i) {
            const uint32 v = get(in + inSize * i);
            const uint64_t w = v + delta;
            if (v == restart || w >= 0xFFFFFFFFull) return 0;  // (a restart index: a new strip on the seam's index)
            if (strip && k && i == 0) {
                const uint32 prev = last;
                if (len & 1) put(prev);  // (the next piece starts on an even index: its own winding)
                put(prev);
                put(uint32(w));
            }
            put(uint32(w));
        }
    }
    const uint32 used = len * outSize;
    outCount = len;
    const bool reuse = index_reuse_enabled();
    const uint64_t key = reuse ? run_shape_key(r) : 0;
    if (reuse) {
        auto it = g_keptCopies.find(key);
        if (it != g_keptCopies.end()) {
            const KeptCopy& c = it->second;
            if (c.type == outType && c.count == len && c.bytes == used && head - c.pos <= kIndexScratchBytes / 2 &&
                !memcmp(mem::ptr(ring + c.at), out, used)) {
                ++g_irunReused;
                return ring + c.at;
            }
        }
    }
    const uint32 step = uint32((uint64_t(used) + 0xFF) & ~0xFFull);
    if (at + step > kIndexScratchBytes) head += kIndexScratchBytes - at, at = 0;
    const uint32 dst = ring + at;
    memcpy(mem::ptr(dst), out, used);
    if (reuse) {
        if (g_keptCopies.size() > 8192) g_keptCopies.clear();
        g_keptCopies[key] = {head, at, used, outType, len};
    }
    at += step;
    head += step;
    return dst;
}
// (WWHD_VK_INDEX_RUN_STATS=1) between draws of the same shader pair: what kept them from one draw
struct RunStat {
    uint32 ps = 0;
    uint64_t after = 0, joined = 0, kind = 0, op = 0, groups[kSgCount] = {}, ops[OP_COUNT + 1] = {};
    // the signature: the programs' code (a hash of their first 256 bytes, the same wherever they are loaded), the
    // draws' shape, and what of the uniform blocks changed between two of them
    uint64_t vsHash = 0, psHash = 0;
    uint32 prim = 0, indexType = 0;
    uint64_t indexed = 0, elements = 0, sameArgs = 0, vsAddrsOnly = 0, vsBlocks = 0, otherBlocks = 0;
    std::unordered_map<uint32, uint64_t> regs;  // the registers changed between two of them, how often
};
uint64_t program_signature(uint32 pgm) {  // (SQ_PGM_START_*: the address >> 8)
    const uint8_t* b = (const uint8_t*)mem::ptr(pgm << 8);
    uint64_t h = 1469598103934665603ull;
    for (uint32 i = 0; i < 256; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint32 g_statBlocks[3][kBlockWords] = {};
bool index_run_stats_on() {
    static const bool on = [] {
        const bool v = getenv("WWHD_VK_INDEX_RUN_STATS") && !strcmp(getenv("WWHD_VK_INDEX_RUN_STATS"), "1");
        g_statRegsOn = v;
        return v;
    }();
    return on;
}
std::unordered_map<uint32, RunStat> g_runStats;
uint32 g_statPrevVs = 0, g_statPrevPs = 0, g_statOpBetween = 0;
Op g_statPrevOp = OP_NOP;
uint32 g_statPrevArgs[6] = {};
uint64_t g_statDraws = 0, g_statFrame0 = 0;
void run_stats_log() {
    std::vector<std::pair<uint32, const RunStat*>> v;
    for (const auto& [vs, st] : g_runStats) v.push_back({vs, &st});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second->after - a.second->joined > b.second->after - b.second->joined; });
    LOG("[gx2] draw-run stats: %.1f draws a frame; shader pairs with the most draws after one of their own not drawn as one "
        "(merged runs or instanced same-state runs):",
        g_statDraws / 600.0);
    for (size_t i = 0; i < v.size() && i < 10; ++i) {
        const RunStat& st = *v[i].second;
        if (st.after == st.joined) break;
        std::string why;
        char buf[96];
        for (int g = 0; g < kSgCount; ++g)
            if (st.groups[g]) snprintf(buf, sizeof buf, ", %s changed %.1f", state_group_name(g), st.groups[g] / 600.0), why += buf;
        if (st.op) {
            uint32 top = 0;
            for (uint32 o = 1; o <= OP_COUNT; ++o) if (st.ops[o] > st.ops[top]) top = o;
            snprintf(buf, sizeof buf, ", an op between %.1f (mostly op %u)", st.op / 600.0, top), why += buf;
        }
        if (st.kind) snprintf(buf, sizeof buf, ", another kind of draw %.1f", st.kind / 600.0), why += buf;
        LOG("[gx2]   VS %08X PS %08X: %.1f a frame after their own, %.1f drawn as one%s", v[i].first, st.ps,
            st.after / 600.0, st.joined / 600.0, why.c_str());
        // the signature
        const double after = double(std::max<uint64_t>(st.after, 1));
        LOG("[gx2]     signature: code %016llX/%016llX, primitive %u, %s, %.0f elements a draw; the same draw as the "
            "last %.0f%%; uniform blocks: only VS addresses %.0f%%, VS contents %.0f%%, PS/GS %.0f%%",
            (unsigned long long)st.vsHash, (unsigned long long)st.psHash, st.prim,
            st.indexed ? (st.indexType == 9 || st.indexType == 1 ? "32-bit indices" : "16-bit indices") : "not indexed",
            st.elements / after, 100.0 * st.sameArgs / after, 100.0 * st.vsAddrsOnly / after, 100.0 * st.vsBlocks / after,
            100.0 * st.otherBlocks / after);
        std::vector<std::pair<uint64_t, uint32>> regs;
        for (const auto& [reg, count] : st.regs) regs.push_back({count, reg});
        std::sort(regs.rbegin(), regs.rend());
        std::string list;
        for (size_t r = 0; r < regs.size() && r < 16; ++r)
            snprintf(buf, sizeof buf, "%s%04X %.0f%%", r ? ", " : "", regs[r].second, 100.0 * regs[r].first / after), list += buf;
        if (!list.empty()) LOG("[gx2]     registers changed: %s", list.c_str());
    }
    g_runStats.clear();
    g_statDraws = 0;
}
void run_stats_note(Op op, const uint32* p, uint32 n, bool joined) {
    const uint32 vs = g_regs[mmSQ_PGM_START_VS], ps = g_regs[mmSQ_PGM_START_PS];
    ++g_statDraws;
    if (vs == g_statPrevVs && ps == g_statPrevPs) {
        RunStat& st = g_runStats[vs];
        if (!st.after) st.vsHash = program_signature(vs), st.psHash = program_signature(ps);
        st.ps = ps;
        ++st.after;
        st.prim = p[0];
        if (op == OP_DRAW_INDEXED) ++st.indexed, st.indexType = p[2];
        st.elements += p[1];
        st.sameArgs += op == g_statPrevOp && !memcmp(p, g_statPrevArgs, std::min<uint32>(n, 6) * 4);
        bool addrs = false, contents = false, other = false;
        for (uint32 b = 0; b < 3; ++b) {
            const uint32* now = &g_regs[kBlockRegions[b]];
            if (!memcmp(now, g_statBlocks[b], kBlockWords * 4)) continue;
            if (b) { other = true; continue; }
            for (uint32 w = 0; w < kBlockWords; ++w)
                if (now[w] != g_statBlocks[0][w]) (w % 7 ? contents : addrs) = true;
        }
        st.vsAddrsOnly += addrs && !contents && !other;
        if (!joined) {
            uint32 seen[128];
            uint32 ns = 0;
            for (uint32 r = 0; r < g_statRegN; ++r) {
                const uint32 reg = g_statRegList[r];
                if (std::find(seen, seen + ns, reg) != seen + ns) continue;
                seen[ns++] = reg;
                ++st.regs[reg];
            }
        }
        st.vsBlocks += contents;
        st.otherBlocks += other;
        if (joined) ++st.joined;
        else if (g_statOpBetween) ++st.op, ++st.ops[std::min<uint32>(g_statOpBetween, OP_COUNT)];
        else if (g_groups_since_draw)
            for (int g = 0; g < kSgCount; ++g) st.groups[g] += (g_groups_since_draw >> g) & 1;
        else if (op != g_statPrevOp || !run_shape(op, p, n) || p[0] != g_statPrevArgs[0] || (op == OP_DRAW_INDEXED && p[2] != g_statPrevArgs[2]))
            ++st.kind;
    }
    for (uint32 b = 0; b < 3; ++b) memcpy(g_statBlocks[b], &g_regs[kBlockRegions[b]], kBlockWords * 4);
    g_statPrevVs = vs, g_statPrevPs = ps, g_statPrevOp = op;
    memcpy(g_statPrevArgs, p, std::min<uint32>(n, 6) * 4);
    g_statOpBetween = 0;
    g_groups_since_draw = 0;
    g_statRegN = 0;
    const uint64_t frame = render::frame_count();
    if (frame >= g_statFrame0 + 600) {
        if (g_statFrame0) run_stats_log();
        else g_runStats.clear(), g_statDraws = 0;
        g_statFrame0 = frame;
    }
}
}  // namespace
// the held run drawn: one draw, or (one piece) as the draw it is
static void index_run_flush() {
    if (!g_irun.on) return;
    g_irun.on = false;
    IndexRun& r = g_irun;
    if (r.pieces == 1) { held_or_draw_now(r.op, r.args, r.n); return; }
    uint32 a[6];
    memcpy(a, r.args, sizeof a);
    if (r.op == OP_DRAW) {
        a[1] = r.total;
    } else if (r.args[0] == kPrimTriangles && r.contiguous && r.oneBase) {
        a[1] = r.total;
    } else {
        uint32 type = 0, count = 0;
        const uint32 addr = gather_indices(type, count);
        if (!addr) {  // (too large for the ring, or a restart index: piece by piece, as before)
            for (uint32 k = 0; k < r.pieces; ++k) {
                a[1] = r.counts[k], a[3] = r.addrs[k], a[4] = r.bases[k];
                held_or_draw_now(OP_DRAW_INDEXED, a, 6);
            }
            return;
        }
        uint32 minBase = r.bases[0];
        for (uint32 k = 1; k < r.pieces; ++k) minBase = std::min(minBase, r.bases[k]);
        a[1] = count, a[2] = type, a[3] = addr, a[4] = minBase;
        ++g_irunCopied;
    }
    held_or_draw_now(r.op, a, r.n);
    ++g_irunRuns;
    g_irunPieces += r.pieces;
    const uint64_t frame = render::frame_count();
    if (frame >= g_irunFrame0 + 600) {
        if (g_irunFrame0)
            LOG("[gx2] draw runs per frame: %.1f drawn as one draw each (%.1f with their indices copied, %.1f of them "
                "an earlier frame's copy kept), %.1f draws in them",
                g_irunRuns / 600.0, g_irunCopied / 600.0, g_irunReused / 600.0, g_irunPieces / 600.0);
        g_irunRuns = g_irunPieces = g_irunCopied = g_irunReused = 0;
        g_irunFrame0 = frame;
    }
}
// ---- weather quads (VR fork, weather_quads.cpp; docs/VR_PERFORMANCE.md, "Weather quads"): between OP_WEATHER_QUADS
// 1 and 0 (the weather packets' draw functions), consecutive draws with the same arguments (a small indexed strip or
// triangle list, one instance, base vertex 0) join a run while the only registers written between them are attribute
// buffer 0's address and the fetch shader's address, and the fetch shader's bytes are the same as the first's (each
// quad has its own vertex buffer and its own copy of the fetch shader). The run is drawn as one draw: each piece's
// vertex buffer is copied into a scratch ring one after another (each padded to the stride), attribute buffer 0 and
// the fetch shader point at the copy and the first piece's for that one draw, and the indices are stitched with each
// piece's base vertex (gather_indices, as a draw run); the registers are put back after it. Anything else ends the
// run first: a register write that changes something else (a uniform block's address, a texture...), another op, a
// draw of another shape. A run of one is drawn as the draw it is. The merged draw is drawn at once (never held: the
// registers it was resolved with are put back right after it)
namespace {
constexpr uint32 kAttrBuf0 = mmSQ_VTX_ATTRIBUTE_BLOCK_START;  // its 7 words: address, size - 1, stride (bits 11..26), ...
constexpr uint32 kWeatherMax = 256;                           // pieces in a run
constexpr uint32 kWeatherRingBytes = 4u << 20;                // the copied vertices' ring (many frames' worth)
constexpr uint32 kWeatherPieceMax = 4096;                     // a piece's vertex buffer, bytes
int g_weatherDepth = 0;                                       // inside OP_WEATHER_QUADS brackets (nested: counted)
struct WeatherRun {
    bool on = false;
    uint32 args[6] = {};
    uint32 pieces = 0, size = 0, stride = 0, fs = 0, fsBytes = 0;
    uint32 addrs[kWeatherMax] = {};
};
WeatherRun g_wrun;
uint64_t g_wRuns = 0, g_wPieces = 0, g_wSingles = 0, g_wFallbacks = 0, g_wFrame0 = 0;
// (why a draw in the brackets did not join: logged every 600 frames, also when nothing joined)
uint64_t g_wMarkers = 0, g_wDraws = 0, g_wNotIndexed = 0, g_wPrim = 0, g_wCount = 0, g_wBase = 0, g_wInstances = 0,
         g_wBuffer = 0, g_wFetch = 0, g_wArgs = 0, g_wSize = 0, g_wFetchDiffers = 0, g_wFull = 0, g_wEndWrite = 0,
         g_wEndOp = 0;
uint32 g_wEndReg = 0;  // the last register a write ended a run with
// a fetch shader's length in bytes as the renderer reads it (vk::get_fetch_shader: the host's decoded form, the compact
// form, or SQ_PGM_SIZE_FS); 0 when it cannot be read
uint32 fetch_shader_bytes(uint32 address) {
    if (!address || uint64_t(address) + 32 > 0x100000000ull) return 0;
    const uint8_t* p = (const uint8_t*)mem::ptr(address);
    if (!memcmp(p, "WWFA0001", 8)) {
        const uint32 count = uint32(p[16]) | uint32(p[17]) << 8 | uint32(p[18]) << 16 | uint32(p[19]) << 24;
        return count > 64 ? 0 : 32 + count * 52;
    }
    if (ld32(address) == 0x57574653) {
        const uint32 count = ld32(address + 4);
        return count > 64 ? 0 : 16 + count * 16;
    }
    const uint32 size = g_regs[mmSQ_PGM_START_FS + 1] << 3;
    return size && size <= 0x1000 && uint64_t(address) + size <= 0x100000000ull ? size : 0;
}
bool weather_shape(Op op, const uint32* p, uint32 n) {
    if (op != OP_DRAW_INDEXED || n != 6 || !index_type_known(p[2])) return ++g_wNotIndexed, false;
    if (!(p[0] == kPrimTriangleStrip || p[0] == kPrimTriangles)) return ++g_wPrim, false;
    if (p[1] < 3 || p[1] > 64) return ++g_wCount, false;
    if (p[4] != 0) return ++g_wBase, false;
    if (p[5] != 1) return ++g_wInstances, false;
    const uint32 addr = g_regs[kAttrBuf0], size = g_regs[kAttrBuf0 + 1] + 1, stride = (g_regs[kAttrBuf0 + 2] >> 11) & 0xFFFF;
    if (!(addr && size && size <= kWeatherPieceMax && stride && stride <= size)) return ++g_wBuffer, false;
    return true;
}
// another piece of the held run: the same arguments, buffer size and stride, the same fetch shader's bytes
bool weather_continues(const uint32* p) {
    const WeatherRun& w = g_wrun;
    if (w.pieces >= kWeatherMax) return ++g_wFull, false;
    if (memcmp(p, w.args, sizeof w.args)) return ++g_wArgs, false;
    if (g_regs[kAttrBuf0 + 1] + 1 != w.size || ((g_regs[kAttrBuf0 + 2] >> 11) & 0xFFFF) != w.stride) return ++g_wSize, false;
    const uint32 fs = g_regs[mmSQ_PGM_START_FS] << 8;
    if (fs == w.fs) return true;
    if (fetch_shader_bytes(fs) == w.fsBytes && !memcmp(mem::ptr(fs), mem::ptr(w.fs), w.fsBytes)) return true;
    return ++g_wFetchDiffers, false;
}
// a register write while a run is held: only attribute buffer 0's address and the fetch shader's address may change
bool weather_write_continues(const uint32* p, uint32 n) {
    if (!n) return true;
    const uint32 first = p[0], count = n - 1;
    if (first >= kNumRegs || count > kNumRegs - first) return false;
    for (uint32 i = 0; i < count; ++i) {
        const uint32 reg = first + i;
        if (g_regs[reg] != p[1 + i] && reg != kAttrBuf0 && reg != mmSQ_PGM_START_FS) return g_wEndReg = reg, false;
    }
    return true;
}
void weather_set_reg(uint32 reg, uint32 value) {
    if (g_regs[reg] != value) apply_regs(reg, &value, 1);
}
void weather_stats() {
    const uint64_t frame = render::frame_count();
    if (frame < g_wFrame0 + 600) return;
    if (g_wFrame0) {
        LOG("[gx2] weather quads per frame: %.1f runs drawn as one draw each (%.1f quads in them), %.1f drawn alone, "
            "%.1f runs drawn piece by piece (too large, or a restart index); brackets %.1f, draws in them %.1f",
            g_wRuns / 600.0, g_wPieces / 600.0, g_wSingles / 600.0, g_wFallbacks / 600.0, g_wMarkers / 600.0, g_wDraws / 600.0);
        LOG("[gx2]   not joined: shape (op/index type %.1f, primitive %.1f, count %.1f, base vertex %.1f, instances %.1f, "
            "vertex buffer %.1f); after a run's draw: arguments %.1f, buffer size/stride %.1f, fetch shader bytes %.1f, "
            "run full %.1f; runs ended by a write %.1f (last register %04X), by another op %.1f",
            g_wNotIndexed / 600.0, g_wPrim / 600.0, g_wCount / 600.0, g_wBase / 600.0, g_wInstances / 600.0, g_wBuffer / 600.0,
            g_wArgs / 600.0, g_wSize / 600.0, g_wFetchDiffers / 600.0, g_wFull / 600.0, g_wEndWrite / 600.0, g_wEndReg,
            g_wEndOp / 600.0);
    }
    g_wRuns = g_wPieces = g_wSingles = g_wFallbacks = 0;
    g_wMarkers = g_wDraws = g_wNotIndexed = g_wPrim = g_wCount = g_wBase = g_wInstances = g_wBuffer = g_wFetch = g_wArgs =
        g_wSize = g_wFetchDiffers = g_wFull = g_wEndWrite = g_wEndOp = 0;
    g_wFrame0 = frame;
}
}  // namespace
static void weather_flush() {
    if (!g_wrun.on) return;
    g_wrun.on = false;
    WeatherRun& w = g_wrun;
    // (the registers now may already hold the next draw's buffer and fetch shader: put back after this run)
    const uint32 savedAddr = g_regs[kAttrBuf0], savedSize = g_regs[kAttrBuf0 + 1], savedFs = g_regs[mmSQ_PGM_START_FS];
    struct Restore {
        uint32 a, s, f;
        ~Restore() { weather_set_reg(kAttrBuf0, a), weather_set_reg(kAttrBuf0 + 1, s), weather_set_reg(mmSQ_PGM_START_FS, f); }
    } restore{savedAddr, savedSize, savedFs};
    weather_set_reg(mmSQ_PGM_START_FS, w.fs >> 8);
    auto draw_one = [&](uint32 k) {
        weather_set_reg(kAttrBuf0, w.addrs[k]);
        weather_set_reg(kAttrBuf0 + 1, w.size - 1);
        held_draw(OP_DRAW_INDEXED, w.args, 1);
        g_dirty_groups = 0;
    };
    g_last.valid = false;
    if (w.pieces == 1) { draw_one(0); ++g_wSingles; weather_stats(); return; }
    // the pieces' vertices, one after another (each padded to a whole number of vertices)
    static uint32 ring = 0, at = 0;
    const uint32 pad = (w.size + w.stride - 1) / w.stride * w.stride, bytes = pad * w.pieces;
    if (!ring) ring = mem::host_alloc(kWeatherRingBytes, 0x100);
    if (!ring || bytes > kWeatherRingBytes / 8) {
        for (uint32 k = 0; k < w.pieces; ++k) draw_one(k);
        ++g_wFallbacks;
        weather_stats();
        return;
    }
    const uint32 step = (bytes + 0xFF) & ~0xFFu;
    if (at + step > kWeatherRingBytes) at = 0;
    const uint32 dst = ring + at;
    at += step;
    for (uint32 k = 0; k < w.pieces; ++k) {
        uint8_t* out = (uint8_t*)mem::ptr(dst + k * pad);
        memcpy(out, mem::ptr(w.addrs[k]), w.size);
        if (pad > w.size) memset(out + w.size, 0, pad - w.size);
    }
    // the indices: the draw-run gather, each piece's base vertex where its copy starts
    IndexRun& r = g_irun;
    r.on = false;
    r.op = OP_DRAW_INDEXED, r.n = 6;
    memcpy(r.args, w.args, sizeof w.args);
    r.pieces = r.total = 0;
    r.contiguous = false, r.oneBase = false;
    for (uint32 k = 0; k < w.pieces; ++k) {
        r.counts[k] = w.args[1], r.addrs[k] = w.args[3], r.bases[k] = k * (pad / w.stride);
        r.total += w.args[1];
        ++r.pieces;
    }
    uint32 type = 0, count = 0;
    const uint32 indices = gather_indices(type, count);
    r.pieces = r.total = 0;
    if (!indices) {
        for (uint32 k = 0; k < w.pieces; ++k) draw_one(k);
        ++g_wFallbacks;
        weather_stats();
        return;
    }
    weather_set_reg(kAttrBuf0, dst);
    weather_set_reg(kAttrBuf0 + 1, bytes - 1);
    uint32 a[6];
    memcpy(a, w.args, sizeof a);
    a[1] = count, a[2] = type, a[3] = indices, a[4] = 0;
    held_draw(OP_DRAW_INDEXED, a, 1);
    g_dirty_groups = 0;
    ++g_wRuns;
    g_wPieces += w.pieces;
    weather_stats();
}
// a draw inside the weather brackets: joins the held weather run, or starts one (true), or is not a weather quad
static bool weather_draw(Op op, const uint32* p, uint32 n) {
    if (!g_weatherDepth || g_observer) return false;
    ++g_wDraws;
    if (!weather_shape(op, p, n)) { weather_flush(); return false; }
    WeatherRun& w = g_wrun;
    if (w.on && weather_continues(p)) {
        w.addrs[w.pieces++] = g_regs[kAttrBuf0];
        return true;
    }
    weather_flush();
    // (the draws held before this one are drawn first, in their order)
    index_run_flush();
    held_flush();
    const uint32 fs = g_regs[mmSQ_PGM_START_FS] << 8, fsBytes = fetch_shader_bytes(fs);
    if (!fsBytes) return false;
    w.on = true;
    memcpy(w.args, p, sizeof w.args);
    w.size = g_regs[kAttrBuf0 + 1] + 1, w.stride = (g_regs[kAttrBuf0 + 2] >> 11) & 0xFFFF;
    w.fs = fs, w.fsBytes = fsBytes;
    w.pieces = 0;
    w.addrs[w.pieces++] = g_regs[kAttrBuf0];
    return true;
}
// ---- GamePad draws unseen: in VR with the GamePad screen's panel hidden and no GamePad window on the desktop
// (gfx::display_vr_gamepad_is_unseen), the draws into the colour buffer the game last copied to the GamePad
// (OP_COPY_TO_SCAN, target 4) are left out: about 150 a frame at three times the GamePad's size that nobody sees. Its
// clears and copies still run; the panel shows the game's next picture once it opens (one frame late at most). Not
// while the TV's picture comes from the same buffer. WWHD_VK_SKIP_UNSEEN_GAMEPAD=0 draws them
namespace {
uint32 g_drcSource = 0, g_drcSize = 0, g_tvSource = 0;
uint64_t g_drcSkipped = 0, g_drcSkipFrame0 = 0;
bool unseen_gamepad_draw() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_SKIP_UNSEEN_GAMEPAD");
        const bool v = !(e && !strcmp(e, "0"));
        LOG("[gx2] GamePad draws %s", v ? "left out while the GamePad screen is seen nowhere in VR (WWHD_VK_SKIP_UNSEEN_GAMEPAD=0 draws them)"
                                        : "always drawn (WWHD_VK_SKIP_UNSEEN_GAMEPAD=0)");
        return v;
    }();
    if (!g_drcSource || g_drcSource == g_tvSource || g_regs[mmCB_COLOR0_BASE] != g_drcSource ||
        g_regs[mmCB_COLOR0_SIZE] != g_drcSize || g_observer || !gfx::display_vr_gamepad_is_unseen() || !on)
        return false;
    ++g_drcSkipped;
    const uint64_t frame = render::frame_count();
    if (frame >= g_drcSkipFrame0 + 600) {
        if (g_drcSkipFrame0) LOG("[gx2] GamePad draws unseen: %.1f a frame left out", g_drcSkipped / 600.0);
        g_drcSkipped = 0;
        g_drcSkipFrame0 = frame;
    }
    return true;
}
void note_scan_source(uint32 colorBuffer, uint32 target) {
    const auto* cb = (const GX2::GX2ColorBuffer*)mem::ptr(colorBuffer);
    const uint32 addr = gx2::color_buffer_address(cb);
    if (target == 4) g_drcSource = addr, g_drcSize = cb->reg_size;
    else if (target == 1) g_tvSource = addr;
}
}  // namespace
// a draw: continues the held run, starts one, or goes on to the same-state runs and the renderer
static void held_or_draw(Op op, const uint32* p, uint32 n) {
    if (unseen_gamepad_draw()) return;
    if (g_weatherDepth && weather_draw(op, p, n)) [[unlikely]] return;  // (weather quads, above)
    if (g_irun.on) {
        if (index_run_continues(op, p, n)) {
            if (index_run_stats_on()) [[unlikely]] run_stats_note(op, p, n, true);
            index_run_add(op, p);
            return;
        }
        index_run_flush();
    }
    if (index_run_candidate(op, p, n)) {
        if (index_run_stats_on()) [[unlikely]] run_stats_note(op, p, n, false);
        IndexRun& r = g_irun;
        r.on = true, r.op = op, r.n = n;
        memcpy(r.args, p, n * 4);
        r.pieces = r.total = 0;
        r.contiguous = r.oneBase = true;
        index_run_add(op, p);
        return;
    }
    g_statHeld = false;
    held_or_draw_now(op, p, n);
    if (index_run_stats_on()) [[unlikely]] run_stats_note(op, p, n, g_statHeld);
}
// before an op: ends the held run unless the op continues it (a draw is decided in held_or_draw)
static inline void held_before(Op op, const uint32* p, uint32 n) {
    if (g_wrun.on) [[unlikely]] {  // (weather quads: only their buffer's and fetch shader's addresses may change)
        switch (op) {
        case OP_NOP: case OP_DEBUG_CONTEXT: case OP_DRAW: case OP_DRAW_INDEXED: break;  // (a draw: weather_draw decides)
        case OP_SET_REGS: if (!weather_write_continues(p, n)) ++g_wEndWrite, weather_flush(); break;
        default: if (op != OP_WEATHER_QUADS) ++g_wEndOp; weather_flush(); break;
        }
    }
    if (index_run_stats_on()) [[unlikely]]
        if (op != OP_NOP && op != OP_DEBUG_CONTEXT && op != OP_SET_REGS && op != OP_DRAW && op != OP_DRAW_INDEXED)
            g_statOpBetween = op;
    if (g_irun.on) [[unlikely]] {
        switch (op) {
        case OP_NOP: case OP_DEBUG_CONTEXT: case OP_DRAW: case OP_DRAW_INDEXED: break;  // (a draw: held_or_draw decides)
        case OP_SET_REGS:
            if (n && p[0] < kNumRegs && n - 1 <= kNumRegs - p[0] && !memcmp(&g_regs[p[0]], p + 1, (n - 1) * 4)) break;
            index_run_flush();
            break;
        default: index_run_flush(); break;
        }
    }
    if (!g_held.on) [[likely]] {
        // (any other op between a run's draws: no run)
        if (g_last.valid && op != OP_SET_REGS && op != OP_DRAW && op != OP_DRAW_INDEXED && op != OP_NOP &&
            op != OP_DEBUG_CONTEXT && op != OP_CALL)
            g_last.valid = false;
        return;
    }
    switch (op) {
    case OP_NOP: case OP_DEBUG_CONTEXT: case OP_CALL: case OP_DRAW: case OP_DRAW_INDEXED: return;
    case OP_SET_REGS:
        if (n && !held_breaks(p[0], p + 1, n - 1)) return;
        if (g_heldStats && n) {
            uint32 first = p[0];
            for (uint32 i = 0; i + 1 < n; ++i)
                if (first + i < kNumRegs && g_regs[first + i] != p[1 + i] && !vs_block_addr(first + i)) {
                    first += i;
                    break;
                }
            held_note_end(0x10000 | (first & 0xFFFF));
        }
        break;
    default:
        if (g_heldStats) held_note_end(op);
        break;
    }
    held_flush();
}
// the photo pane's state as the render thread last set it or handed it to the scene's thread (docs/VR_SCENE_THREADS.md,
// stage S5): stereo::layoutPhotograph is that thread's while it records the scene
static bool g_layoutPhotographSet = false;
// every op is counted and (sampled) timed for the render-thread profiler (render_prof.h); display-list
// calls are not timed themselves: their ops are
// (diagnostics) WWHD_LOG_SLOW_OPS=ms: every op that takes longer than ms on this thread (a freeze's place: a draw
// waiting for a shader or pipeline, a copy, a flush, the swap), with the draw's vertex and pixel programs
static const double g_slowOpMs = getenv("WWHD_LOG_SLOW_OPS") ? atof(getenv("WWHD_LOG_SLOW_OPS")) : 0;
static void execute_one_timed(Op op, const uint32* p, uint32 n);
static void execute_one(Op op, const uint32* p, uint32 n) {
    if (op == OP_SET_REGS && g_skipSame && same_regs(p, n)) return;
    if (g_slowOpMs > 0 && op != OP_SET_REGS && op != OP_CALL) [[unlikely]] {
        const uint32 vs = g_regs[mmSQ_PGM_START_VS] << 8, ps = g_regs[mmSQ_PGM_START_PS] << 8;
        const int64_t t0 = steady_ns();
        execute_one_timed(op, p, n);
        const double ms = (steady_ns() - t0) / 1e6;
        if (ms >= g_slowOpMs)
            LOG("[gx2] slow op %u: %.1f ms (VS %08X PS %08X, swap %llu)", unsigned(op), ms, vs, ps,
                (unsigned long long)g_swaps_executed.load(std::memory_order_relaxed));
        return;
    }
    execute_one_timed(op, p, n);
}
static void execute_one_timed(Op op, const uint32* p, uint32 n) {
    held_before(op, p, n);  // (batched same-state runs, above)
    rprof::Op kind;
    switch (op) {
    case OP_DEBUG_CONTEXT: g_draw_context.store(n ? p[0] : 0, std::memory_order_relaxed); return;
    case OP_SUBMITTED_UNIFORMS:
        set_active_snapshot(n >= 2 ? (submitted::Snapshot*)(uintptr_t(p[0]) | (uint64_t(p[1]) << 32)) : nullptr);
        return;
    case OP_CALL: execute_op(op, p, n); return;
    case OP_SET_REGS: case OP_SET_PROJ_REGS: kind = rprof::kOpRegs; break;
    case OP_DRAW: case OP_DRAW_INDEXED: kind = rprof::kOpDraw; rprof::classify_draw(g_regs); break;
    case OP_CLEAR_COLOR: case OP_CLEAR_DEPTH: case OP_CLEAR_BUFFERS: kind = rprof::kOpClear; break;
    case OP_COPY_SURFACE: kind = rprof::kOpCopy; break;
    case OP_COPY_TO_SCAN: kind = rprof::kOpScan; break;
    case OP_INVALIDATE: kind = rprof::kOpInvalidate; break;
    case OP_PEEK_Z: kind = rprof::kOpCopy; break;
    case OP_FLUSH: kind = rprof::kOpFlush; break;
    case OP_DRAW_DONE: kind = rprof::kOpDrawDone; break;
    case OP_SWAP: kind = rprof::kOpSwap; break;
    default: kind = rprof::kOpOther; break;
    }
    // even frame delivery: a held frame is presented when its time has come (checked before each
    // draw, clear and copy, which is fine-grained enough: one takes a few microseconds)
    if (kind != rprof::kOpOther && kind != rprof::kOpRegs) {
        const int64_t due = render::present_due();
        if (due && steady_ns() >= due) render::service_present();
    }
    const uint64_t started = rprof::op_begin(kind);
    execute_op(op, p, n);
    rprof::op_end(kind, started);
    if (op == OP_SWAP) {
        test_skip_log();
        if (g_heldStats) held_stats_log();
        reg_stats_swap();
        rprof::frame_end(n >= 2 && p[1]);  // applies a change of the profiling setting
        g_classify = rprof::enabled();
    }
}

// Test only: WWHD_TEST_RENDER_LOAD_MS=x / WWHD_TEST_GAME_LOAD_MS=x busy-wait x ms per frame on the
// render thread (before each swap is executed) / on the game thread (at each GX2SwapScanBuffers):
// the frame time of a slower computer, for measuring frame pacing (docs/performance.md).
// WWHD_TEST_GAME_LOAD_MS=x:y busy-waits x ms at the swap of a logic step's frame and y ms at an
// in-between frame's (the uneven passes of a phone whose game thread is the bottleneck).
static void test_frame_load(const char* name, bool hold = false) {
    const char* e = getenv(name);
    const char* colon = e ? strchr(e, ':') : nullptr;
    const double ms = e ? (hold && colon ? atof(colon + 1) : atof(e)) : 0;
    if (ms <= 0) return;
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(int64_t(ms * 1e6));
    while (std::chrono::steady_clock::now() < until) {}
}

// Test only (VR fork, docs/VR_PERFORMANCE.md): WWHD_TEST_SKIP_BATCHABLE=1 leaves out every draw that only continues a
// same-state run (nothing but buffer/constant words changed since the draw before: g_full_state_version unchanged), as
// if each run were one batched draw. The register writes are still applied. The picture is WRONG (most of the grass and
// other repeated pieces are missing): it measures what batching such runs could save at most, never for play
static std::atomic<uint64_t> g_testSkipped{0}, g_testDraws{0};
static bool test_skip_batchable() {
    // =2: only the draws a held run could take (the render thread's: draw_batch::can_hold), =3: only the others (the sun
    // atlas's)
    static const int on = [] {
        const char* e = getenv("WWHD_TEST_SKIP_BATCHABLE");
        const int v = e ? atoi(e) : 0;
        if (v) LOG("[gx2] TEST WWHD_TEST_SKIP_BATCHABLE=%d: draws continuing a same-state run are left out (the picture is wrong)", v);
        return v;
    }();
    if (!on) return false;
    static uint64_t lastVersion = 0;
    bool continues = g_full_state_version == lastVersion;
    lastVersion = g_full_state_version;
    if (continues && on >= 2) {
        const bool holdable = draw_batch::can_hold() && draw_batch::can_hold()(g_regs);
        continues = on == 2 ? holdable : !holdable;
    }
    g_testDraws.fetch_add(1, std::memory_order_relaxed);
    if (continues) g_testSkipped.fetch_add(1, std::memory_order_relaxed);
    return continues;
}
static void test_skip_log() {
    static uint64_t swaps = 0, draws0 = 0, skipped0 = 0;
    if (++swaps % 600 || !g_testDraws.load(std::memory_order_relaxed)) return;
    const uint64_t d = g_testDraws.load(), s = g_testSkipped.load();
    LOG("[gx2] TEST skip batchable: %.0f of %.0f draws a frame left out", (s - skipped0) / 600.0, (d - draws0) / 600.0);
    draws0 = d;
    skipped0 = s;
}

// (diagnostics) WWHD_GX2_REG_STATS=1: register writes per frame, every 600 frames: ops, words, the ops and words that
// change nothing, in display lists or not, and the register areas (first register / 256) with the most ops (the writes
// that change nothing are dropped before this unless WWHD_GX2_SKIP_SAME=0: execute_one)
namespace {
const bool g_regStats = [] { const char* e = getenv("WWHD_GX2_REG_STATS"); return e && !strcmp(e, "1"); }();
struct RegArea { uint64_t ops = 0, same = 0, words = 0, sameWords = 0; };
std::unordered_map<uint32, RegArea> g_regAreas;
uint64_t g_regOps = 0, g_regSame = 0, g_regWords = 0, g_regSameWords = 0, g_regListOps = 0;
int g_callDepth = 0;
void reg_stats_note(uint32 first, const uint32* v, uint32 n) {
    uint32 same = 0;
    for (uint32 i = 0; i < n && first + i < kNumRegs; ++i) same += g_regs[first + i] == v[i];
    auto& a = g_regAreas[first >> 8];
    ++a.ops; a.words += n; a.sameWords += same;
    ++g_regOps; g_regWords += n; g_regSameWords += same;
    if (same == n) ++a.same, ++g_regSame;
    if (g_callDepth) ++g_regListOps;
}
void reg_stats_log() {
    static uint64_t swaps = 0;
    if (++swaps % 600) return;
    std::vector<std::pair<uint64_t, uint32>> v;
    for (auto& [k, a] : g_regAreas) v.push_back({a.ops, k});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    for (size_t i = 0; i < v.size() && i < 10; ++i) {
        const RegArea& a = g_regAreas[v[i].second];
        char b[96];
        snprintf(b, sizeof b, " %04X: %.0f ops (%.0f%% unchanged), %.1f words", v[i].second << 8, a.ops / 600.0,
                 100.0 * a.same / a.ops, double(a.words) / a.ops);
        line += b;
    }
    LOG("[gx2] register writes per frame: %.0f ops (%.0f%% in display lists), %.0f words; %.0f%% of ops and %.0f%% of words "
        "change nothing; by area:%s", g_regOps / 600.0, 100.0 * g_regListOps / std::max<uint64_t>(g_regOps, 1),
        g_regWords / 600.0, 100.0 * g_regSame / std::max<uint64_t>(g_regOps, 1),
        100.0 * g_regSameWords / std::max<uint64_t>(g_regWords, 1), line.c_str());
    g_regAreas.clear();
    g_regOps = g_regSame = g_regWords = g_regSameWords = g_regListOps = 0;
}
}  // namespace
static void reg_stats_swap() {
    if (g_regStats) reg_stats_log();
}

static void execute_op(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS:
        if (g_regStats && n) reg_stats_note(p[0], p + 1, n - 1);
        apply_regs(p[0], p + 1, n - 1);
        break;
    case OP_DRAW: if (!test_skip_batchable()) held_or_draw(op, p, n); break;
    case OP_DRAW_INDEXED: if (!test_skip_batchable()) held_or_draw(op, p, n); break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        render::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 0), bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        uint32 cb = unpack_struct(p, kColorBufferWords, 0), db = unpack_struct(p + kColorBufferWords, kDepthBufferWords, 1);
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, cb, rgba);
        render::clear_depth_stencil(g_regs, db, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        // debug: WWHD_GX2_DELAY_COPY=ms stalls the render thread before each surface copy (a slow
        // or busy render thread; reproduced the agl boot crash every time before GX2CopySurface waited)
        static const int delay = getenv("WWHD_GX2_DELAY_COPY") ? atoi(getenv("WWHD_GX2_DELAY_COPY")) : 0;
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        uint32 src = unpack_struct(p, kSurfaceWords, 0);
        const uint32* q = p + kSurfaceWords;
        uint32 dst = unpack_struct(q + 2, kSurfaceWords, 1);
        const uint32* r = q + 2 + kSurfaceWords;
        render::copy_surface(src, q[0], q[1], dst, r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN: {
        const uint32 cb = unpack_struct(p, kColorBufferWords, 0);
        note_scan_source(cb, p[kColorBufferWords]);
        render::copy_to_scan(cb, p[kColorBufferWords]);
        break;
    }
    case OP_CALL:
        ++g_callDepth;
        execute((const uint32*)mem::ptr(p[0]), p[1] / 4);
        --g_callDepth;
        break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: render::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_PEEK_Z: render::peek_z(p, n); break;  // answered (gfx::depth_peek) when its copy completes
    case OP_FLUSH: render::guest_flush(); break;  // Vulkan: asynchronous submission
    case OP_DRAW_DONE:
        // The Vulkan renderer writes GPU results back to guest memory only for linear surfaces (guest data is copied
        // into fenced upload slices when work is recorded), so GX2DrawDone needs this op executed
        // (render_sync in the HLE), not an idle GPU. Lazy DrawDone (lazy_draw_done(), the default)
        // queues the work instead of waiting for the whole device every frame. A payload word of 1
        // (save states) always waits for the idle GPU (a platform backend's gpu_idle, else wait_idle),
        // then the CPU reads back linear render targets (backend_hooks.h).
        draw_done(lazy_draw_done(), n && p[0]);
        break;
    case OP_SWAP: {
        static const bool load = getenv("WWHD_TEST_RENDER_LOAD_MS") != nullptr;
        if (load) test_frame_load("WWHD_TEST_RENDER_LOAD_MS");
        if (n) render::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        if (n >= 3 && (p[2] & 1)) render::request_capture();
        if (n >= 3 && (p[2] & 2)) render::view_cut();  // (this frame's view cut: render::note_view_cut)
        if (n >= 5 && present_observer::enabled())
            present_observer::render_swap(uint64_t(p[3]) | (uint64_t(p[4]) << 32), render::frame_count());
        render::set_present_interval(n >= 6 ? p[5] : 0u, n >= 2 && p[1]);
#ifdef WWHD_VR
        vr::set_render_views(n >= 7 ? p[6] : 0u);
#endif
        render::swap();
        g_swaps_executed.fetch_add(1);
        break;
    }
    case OP_SET_PROJ_REGS: {
        uint32 v[16];
        memcpy(v, p + 1, sizeof v);
        float kx, ky;
        if (n == 17 && render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky)) {
            // (docs/VR_CUTSCENE_TEXT.md, "Dialogue in the UI layer") with the eyes' own frustums the layouts are drawn
            // into the UI layer's image, the symmetric field's shape: placed for that, not for an eye's narrower
            // picture (they were stretched across by the two shapes' ratio, and their clip with them)
            if (stereo::ui_layer_on()) stereo::ui_layer_factors(aspect::game(), kx, ky);
#ifdef WWHD_VR
            // VR menu scenes: the layouts drawn smaller around the centre, as if farther away (vr::layout_scale)
            const float vrScale = vr::layout_scale();
            kx /= vrScale;
            ky /= vrScale;
#endif
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = fbits(bitsf(v[i]) / kx);
                v[4 + i] = fbits(bitsf(v[4 + i]) / ky);
            }
#ifdef WWHD_VR
            // VR conversations and cutscenes: the text moved up or down (y += dy * w: a shift in the picture), and
            // across (x += dx * w) while a conversation's text is held in the world in first person
            if (const float dy = vr::layout_offset_y(); dy != 0.0f)
                for (int i = 0; i < 4; i++) v[4 + i] = fbits(bitsf(v[4 + i]) + dy * bitsf(v[12 + i]));
            if (const float dx = vr::layout_offset_x(); dx != 0.0f)
                for (int i = 0; i < 4; i++) v[i] = fbits(bitsf(v[i]) + dx * bitsf(v[12 + i]));
#endif
        }
        apply_regs(p[0], v, std::min<uint32>(n - 1, 16));
        if (n == 17) {
            // (docs/VR_SCENE_THREADS.md, stage S5: on the scene's thread while it records the scene, in order)
            uint32 tag[17];
            tag[0] = p[0];
            memcpy(tag + 1, v, sizeof v);
            if (!render::hand_draw_state(render::kStateProjectionTag, tag, 17)) stereo::tag_layout(p[0], v);
        }
        break;
    }
    case OP_LAYOUT_CONTENT:
        // (stage S5) on the scene's thread while it records the scene; the machine's full state change stays here, and is
        // forwarded to it after the handed state, as it would follow it here
        if (render::hand_draw_state(render::kStateLayoutContent, p, n)) {
            if (const bool photo = n >= 3 && p[2]; photo != g_layoutPhotographSet) {
                g_layoutPhotographSet = photo;
                note_full_state_change();
            }
            break;
        }
        aspect::set_content_clip(n && p[0]);
        if (n >= 2) stereo::layout_draw(p[1] != 0);
        // A photo pane samples the canonical captured image. Changing that
        // policy invalidates continued texture bindings, even with identical registers.
        if (const bool photo = n >= 3 && p[2]; photo != stereo::layoutPhotograph) {
            stereo::layoutPhotograph = photo;
            note_full_state_change();
        }
        g_layoutPhotographSet = stereo::layoutPhotograph;
        break;
    case OP_CLOUDS_BEGIN:
        render::before_draw_state();
        cloud_proto::g_active_seq = n ? p[0] : 0;
        cloud_proto::g_drawn = false;
        if (render::g_backend->sky_clouds_begin && cloud_proto::g_active_seq)
            render::g_backend->sky_clouds_begin(g_regs, cloud_proto::g_active_seq);
        break;
    case OP_CLOUDS_END:
        render::before_draw_state();
        if (cloud_proto::g_on_end && cloud_proto::g_active_seq) cloud_proto::g_on_end();
        cloud_proto::g_active_seq = 0;
        break;
    case OP_GAMEPAD_BACKDROP:
        if (render::g_backend->gamepad_backdrop_scope)
            render::g_backend->gamepad_backdrop_scope(g_regs, n && p[0]);
        break;
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        const bool tv = render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky);
        const uint32 root[2] = {p[0], tv};
        if (!render::hand_draw_state(render::kStateLayoutRoot, root, 2)) aspect::layout_root_target(p[0], tv);  // (S5)
        break;
    }
    // 60 fps (generated, framegen.h): only emitted while it is on
    case OP_FRAMEGEN_CAMERA:
        if (render::g_backend->framegen_camera) render::g_backend->framegen_camera(p, n);
        break;
    case OP_WEATHER_QUADS:  // (weather quads, weather_quads.cpp: the run held is drawn by held_before)
        if (n && p[0]) ++g_weatherDepth, ++g_wMarkers;
        else if (g_weatherDepth) --g_weatherDepth;
        weather_stats();
        break;
    case OP_FRAMEGEN_LAYOUT:
        if (render::g_backend->framegen_layout)
            render::g_backend->framegen_layout(g_regs[mmCB_COLOR0_BASE], g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG],
                                               g_regs[mmDB_DEPTH_BASE]);
        break;
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        break;
    case OP_FENCE: {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_fence_done = std::max<uint64_t>(g_fence_done, p[0]);
        g_q_done_cv.notify_all();
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
// 120/240 fps frame interpolation needs 4/8 flips per logic step: the virtual vsync then ticks 2/4
// times per 59.94 Hz vsync, and 40 fps (4 flips per 3 steps) every 1.5 vsyncs. The clock counts
// twelfths of a vsync ("ticks"; quarters until VR's 90 fps, three frames a step, needed thirds); a vsync at
// the current rate is a granule of ticks (interp::vsync_granule_ticks: 12, 8 at 90 fps, 6 at 120, 3 at 240;
// 18 at 40 fps), and every vsync count below is in ticks, rounded to granules, so at 30/60 fps (granule 12)
// the timing is the 59.94 Hz one exactly.
static uint64_t g_swap_count = 0, g_flip_count = 0;
namespace gx2 {
uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); }  // live fps in the title
// swaps the game has queued that the render thread has not executed yet: 0 while the game waits for each swap, 1 when
// it builds its next frame while the render thread presents (PERF-07a). VR's pose prediction counts them
// (vr_game.cpp display_time)
uint64_t swaps_in_flight() {
    const uint64_t issued = __atomic_load_n(&g_swap_count, __ATOMIC_RELAXED), done = g_swaps_executed.load();
    return issued > done ? issued - done : 0;
}
}  // namespace gx2
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); uint64_t logic_steps(); uint32_t vsync_granule_ticks(); uint64_t step_ticks(uint32_t game); bool paced_step_schedule(); int64_t frame_interval_ns(); }
static std::mutex g_flip_mutex;
static const auto g_vsync_epoch = std::chrono::steady_clock::now();
static constexpr std::chrono::nanoseconds kVsyncPeriod(16683333);  // 59.94 Hz
static constexpr uint64_t kTicksPerVsync = 12;
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
// (at: when it was swapped; step: the first frame of a logic step, not an interpolation hold pass)
struct PendingFlip { uint64_t swap; std::chrono::steady_clock::time_point at; bool step; };
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;  // tick (a granule boundary)
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

// The clock is advanced (g_vsync_shift) when a late flip moves the grid (vsync_pacing.h).
static std::atomic<int64_t> g_vsync_shift{0};  // ns; written with g_flip_mutex held
// The vsync's period: 59.94 Hz, or in VR the headset's (gx2::lock_vsync_to_display, docs/VR_FRAME_RATE.md "Locked to the
// headset's clock"). Written with g_flip_mutex held
static std::atomic<int64_t> g_vsync_period{kVsyncPeriod.count()};
static vsync_pacing::Clock vsync_clock() { return {g_vsync_period.load(std::memory_order_relaxed), kTicksPerVsync}; }
static int64_t vsync_clock_now() {  // ns since the epoch on the vsync's clock (the game's clock when it runs faster)
    if (!game_clock::shifted()) return (std::chrono::steady_clock::now() - g_vsync_epoch).count();
    const auto epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(g_vsync_epoch.time_since_epoch()).count();
    return game_clock::sample().guest - epoch;
}
static uint64_t vsync_index() {  // in ticks
    return vsync_clock().index(vsync_clock_now(), g_vsync_shift.load(std::memory_order_relaxed));
}
static uint64_t vsync_granule() {  // ticks per vsync at the current rate: 12 (30/60 fps), 18 (40), 8 (90), 6 (120), 3 (240)
    return interp::vsync_granule_ticks();
}
static std::chrono::steady_clock::time_point tick_time(uint64_t tick) {
    const auto at = std::chrono::nanoseconds(vsync_clock().time(tick, g_vsync_shift.load(std::memory_order_relaxed)));
    if (!game_clock::shifted())
        return g_vsync_epoch + at;
    const auto sample = game_clock::sample();
    const auto target = g_vsync_epoch + at;
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count() - sample.guest;
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sample.host + std::max<int64_t>(0, (left + sample.rate - 1) / sample.rate)));
}
// Step slots (vsync_pacing.h): with paced frame interpolation (60, 120 and 240 fps) a logic step's
// first frame is due on its step's slot (ns on the vsync's clock, -1: none), two vsyncs after the
// previous step's
static int64_t g_step_slot = -1;  // g_flip_mutex held
static bool step_slots() { return interp::paced_step_schedule(); }
static int64_t step_ns() {  // the frames of a step, one (virtual) vsync each: the game's swap interval, two vsyncs
    return vsync_clock().granule_ns(interp::step_ticks(g_swap_interval));
}
// the first tick at which this flip may execute: `swap interval` vsyncs after the previous flip, and
// a step's first frame not before its step's slot. (On hardware also the vsync after its swap, which
// is the same for a swap made before that tick; a later swap, a frame that missed its vsync, flips
// as soon as it is ready: vsync_pacing.h.)
static uint64_t flip_due(const PendingFlip& f) {
    const uint64_t g = vsync_granule();
    const uint64_t due = g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval) * g;
    if (!f.step || !step_slots()) return due;
    return vsync_pacing::step_due(vsync_clock(), due, g_step_slot, g_vsync_shift.load(std::memory_order_relaxed), g);
}
// VR: the game's frames on the headset's clock. At 45 fps the game drew a frame every 22.244 ms (4/3 of a 59.94 Hz
// vsync) and the headset at 90 Hz shows one every 22.222 ms: the frames slid slowly across the headset's deadline, and
// while one was near it (about 1 frame in 7) they landed on the wrong refresh, a frame shown for 3 refreshes and the
// next for 1: the world judders when the head moves (reported in the headset). The vsync is now 1.5 refreshes of a
// 90 Hz headset (2 of a 120 Hz one): the game steps at the headset's 30.00 Hz instead of 29.97 (0.1%, the user's
// choice). 0: back to 59.94 Hz. The tick the game is on stays the same (the shift takes up the change)
void gx2::lock_vsync_to_display(int64_t period) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    if (period <= 0) period = kVsyncPeriod.count();
    const int64_t before = g_vsync_period.load(std::memory_order_relaxed);
    if (period == before) return;
    const int64_t t = vsync_clock_now(), shift = g_vsync_shift.load(std::memory_order_relaxed);
    g_vsync_shift.store(int64_t(double(t + shift) * double(period) / double(before)) - t, std::memory_order_relaxed);
    g_vsync_period.store(period, std::memory_order_relaxed);
    g_step_slot = -1;  // (in the old period's times: the next step starts a new one)
    LOG("[gx2] vsync %s: %.4f ms (%.3f Hz)", period == kVsyncPeriod.count() ? "back to 59.94 Hz" : "on the headset's clock",
        double(period) / 1e6, 1e9 / double(period));
}
// VR: the frames reach the headset near its deadline (vr_vulkan.cpp): the grid moves later by `ns` once, so they are
// due away from it. (The clocks are locked, so it stays put)
void gx2::delay_vsync_grid(int64_t ns) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    g_vsync_shift.fetch_sub(ns, std::memory_order_relaxed);
}
int64_t gx2::vsync_period_ns() { return g_vsync_period.load(std::memory_order_relaxed); }
// paced interpolation (interp.cpp): a logic pass more than a step late drops the schedule (the next
// logic frame starts a new one)
void gx2::drop_step_slot() {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    g_step_slot = -1;
}
// paced interpolation (interp.cpp): when the next logic step's frame is due, if there is a slot
bool gx2::next_step_time(std::chrono::steady_clock::time_point& at) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    if (g_step_slot < 0 || !step_slots()) return false;
    at = tick_time(vsync_pacing::slot_tick(vsync_clock(), g_step_slot, g_vsync_shift.load(std::memory_order_relaxed), vsync_granule()));
    return true;
}
// paced interpolation at 40 fps (interp.cpp): the frames the next logic step has before the step
// after it is due (1 or 2), 0 without a slot
int gx2::step_frames() {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    if (g_step_slot < 0 || !step_slots()) return 0;
    return vsync_pacing::step_frames(vsync_clock(), g_step_slot, step_ns(), g_vsync_shift.load(std::memory_order_relaxed), vsync_granule());
}
// Uncapped (gx2.h): debug, to see how fast the renderer can go; GPU completion ordering is kept
static std::atomic<int> g_uncapped{-1};  // -1: not read from the environment yet
bool gx2::uncapped() {
    int v = g_uncapped.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* any = getenv("WWHD_UNCAPPED");
        const char* vk = getenv("WWHD_VK_UNCAPPED");
        v = (any && !strcmp(any, "1")) || (vk && !strcmp(vk, "1") && render::vulkan()) ? 1 : 0;
        int expected = -1;
        g_uncapped.compare_exchange_strong(expected, v);
        v = g_uncapped.load(std::memory_order_relaxed);
    }
    return v == 1;
}
void gx2::set_uncapped(bool on) {
    if (uncapped() == on) return;
    g_uncapped = on ? 1 : 0;
    LOG("[gx2] uncapped %s", on ? "on (debug: no frame limit, no vsync; the game runs faster than real time)" : "off");
}
// Fast Forward has a selected rate even when debug uncapped is selected: remove
// only the host presentation limit, retaining the accelerated virtual-vsync waits.
static bool uncapped_benchmark() { return gx2::uncapped() && !mods::fast_forward_active(); }

// debug: WWHD_LOG_FLIP_PACING=1 logs, every 300 flips, the intervals between flips (the frame times
// the game sees) and what the flips waited for
static void log_flip_pacing(const PendingFlip& f) {
    static const bool on = getenv("WWHD_LOG_FLIP_PACING") != nullptr;
    if (!on) return;
    using clk = std::chrono::steady_clock;
    static clk::time_point last{};
    static std::vector<float> intervals;
    static double waitSum = 0;
    const auto now = clk::now();
    if (last != clk::time_point{}) intervals.push_back(std::chrono::duration<float, std::milli>(now - last).count());
    last = now;
    waitSum += std::chrono::duration<double, std::milli>(now - f.at).count();
    if (intervals.size() < 300) return;
    std::vector<float> v = intervals;
    std::sort(v.begin(), v.end());
    double sum = 0;
    int over25 = 0, over34 = 0;
    for (float x : v) { sum += x; over25 += x > 25; over34 += x > 34; }
    LOG("[gx2 flips] %.1f flips/s, interval p50 %.2f p95 %.2f p99 %.2f max %.2f ms, >25 ms %d, >34 ms %d of %zu; swap to flip %.2f ms",
        1000.0 * v.size() / sum, v[v.size() / 2], v[v.size() * 95 / 100], v[v.size() * 99 / 100], v.back(), over25, over34, v.size(),
        waitSum / v.size());
    intervals.clear();
    waitSum = 0;
}
// debug: WWHD_TRACE_FLIPS=file writes a line per flip: when it executed, when its frame was
// swapped, when it was due (steady clock, ms), the tick it was due on and the tick it counts as
// (they differ when it moved the grid), whether it was an in-between frame, and the granule. With
// WWHD_TRACE_STEPS (interp.cpp) for the frame pacing at 120/240 fps.
static FILE* trace_flips_file() {
    static FILE* out = getenv("WWHD_TRACE_FLIPS") ? fopen(getenv("WWHD_TRACE_FLIPS"), "w") : nullptr;
    return out;
}
static void trace_flip(const PendingFlip& f, std::chrono::steady_clock::time_point dueAt, uint64_t due, uint64_t at) {
    FILE* out = trace_flips_file();
    if (!out) return;
    const auto ms = [](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count(); };
    fprintf(out, "%.3f %.3f %.3f %llu %llu %d %d\n", ms(std::chrono::steady_clock::now()), ms(f.at), ms(dueAt), (unsigned long long)due,
            (unsigned long long)at, int(!f.step), int(vsync_granule()));
}
static void update_flips() {  // g_flip_mutex held
    static const bool observeDeadlines = present_observer::enabled();
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        const uint64_t due = flip_due(g_pending_flips.front());
        if ((!uncapped_benchmark() && due > now) || render::frames_completed() < g_pending_flips.front().swap) break;
        uint64_t at = now / vsync_granule() * vsync_granule();
        // Preserve the actual scheduler target BEFORE this flip may move its grid.
        const auto dueAt = (trace_flips_file() || observeDeadlines) ? tick_time(due) : std::chrono::steady_clock::time_point{};
        const int64_t shiftBefore = observeDeadlines ? g_vsync_shift.load(std::memory_order_relaxed) : 0;
        if (!uncapped_benchmark()) {  // on its vsync, or late: then the grid may move (vsync_pacing.h)
            int64_t shift = g_vsync_shift.load(std::memory_order_relaxed);
            const int64_t t = vsync_clock_now();
            const int64_t dueTime = vsync_clock().time(due, shift);
            at = vsync_pacing::flip_executed(vsync_clock(), shift, due, t, vsync_granule());
            g_vsync_shift.store(shift, std::memory_order_relaxed);
            now = std::max(now, at);
            if (g_pending_flips.front().step) {  // the next step's slot
                const bool slotted = g_step_slot >= 0 && step_slots();
                g_step_slot = step_slots() ? vsync_pacing::next_slot(slotted ? g_step_slot : dueTime, t, step_ns(),
                                                                       vsync_clock().granule_ns(vsync_granule())) : -1;
            }
        } else {
            g_step_slot = -1;
        }
        if (observeDeadlines)
            present_observer::scheduler_due(g_pending_flips.front().swap,
                std::chrono::duration_cast<std::chrono::nanoseconds>(dueAt.time_since_epoch()).count(),
                due, shiftBefore, g_vsync_shift.load(std::memory_order_relaxed), uint32_t(vsync_granule()),
                game_clock::shifted(), uncapped_benchmark());
        log_flip_pacing(g_pending_flips.front());
        trace_flip(g_pending_flips.front(), dueAt, due, at);
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_time = timebase::simulation_now();
        g_flip_count++;
    }
}
// A flip whose vsync has come but whose frame is not ready yet (the game swapped late, or the
// renderer has not finished it): GX2WaitForVsync waits for that frame in short steps instead of
// sleeping to the next vsync, and returns when the flip has executed (vsync_pacing.h).
static bool late_flip_pending() {  // g_flip_mutex held
    if (g_pending_flips.empty()) return false;
    const auto& front = g_pending_flips.front();
    return flip_due(front) <= vsync_index() && render::frames_completed() < front.swap;
}
static bool wait_for_late_flip(std::chrono::steady_clock::time_point deadline) {
    uint64_t flips;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        flips = g_flip_count;
        update_flips();
        if (g_flip_count != flips) return true;
        if (!late_flip_pending()) return false;
    }
    constexpr auto kStep = std::chrono::microseconds(250);
    for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
        threads::park_sleep_until(std::min(deadline, now + kStep));
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        if (g_flip_count != flips) return true;
        if (!late_flip_pending()) return false;
    }
    return false;
}
#ifdef WWHD_HAS_VULKAN
static void ready_flip_before_resume() {
    bool needsSync;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        if(g_pending_flips.empty()) return;
        const auto& front = g_pending_flips.front();
        const uint64_t at = flip_due(front);
        if(at > vsync_index()) return;
        needsSync = render::frames_completed() < front.swap;
    }
    if(needsSync) render_sync(kSyncFlip); // Core already released; no flip lock held.
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
}
#endif

// Hang watchdog: when the game has not swapped a frame for 3 s, log what the render thread and every
// guest thread are doing (again every 15 s, at most 6 times per stall); the log file is written
// line by line, so the dump survives a force-stop. WWHD_NO_WATCHDOG=1 turns it off. A legitimate
// pause (the app in the background, a debugger) logs it too, at most 6 times.
// Idea from SwitchWakerHD (thanks @Pulpparty / @depende3000).
static std::atomic<uint64_t> g_watch_swaps{0};
static void watchdog_thread() {
    host::set_thread_name("watchdog");
    uint64_t lastSwaps = g_watch_swaps.load();
    auto lastChange = std::chrono::steady_clock::now();
    auto nextReport = lastChange + std::chrono::seconds(3);
    int reports = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = std::chrono::steady_clock::now();
        const uint64_t swaps = g_watch_swaps.load(std::memory_order_relaxed);
        if (swaps != lastSwaps) {
            if (reports) LOG("[watchdog] frames again after %.1f s", std::chrono::duration<double>(now - lastChange).count());
            lastSwaps = swaps;
            lastChange = now;
            nextReport = now + std::chrono::seconds(3);
            reports = 0;
            continue;
        }
        if (now < nextReport || reports >= 6) continue;
        reports++;
        nextReport = now + std::chrono::seconds(15);
        size_t pending = 0;
        uint64_t issued = 0, done = 0;
        bool idle = false, locked = false;
        {
            std::unique_lock<std::mutex> lk(g_q_mutex, std::defer_lock);
            if ((locked = lk.try_lock())) {
                pending = g_q_pending.size();
                issued = g_fence_issued;
                done = g_fence_done;
                idle = g_q_waiting;
            }
        }
        if (locked)
            LOG("[watchdog] no frame for %.1f s (swap %llu). Render thread: %s; %zu command words queued; game syncs issued %llu, "
                "done %llu", std::chrono::duration<double>(now - lastChange).count(), (unsigned long long)swaps,
                idle ? "waiting for commands" : "executing", pending, (unsigned long long)issued, (unsigned long long)done);
        else
            LOG("[watchdog] no frame for %.1f s (swap %llu). Render queue locked", std::chrono::duration<double>(now - lastChange).count(),
                (unsigned long long)swaps);
        threads::dump_state();
    }
}

HLE(gx2, GX2Init) {
    static std::once_flag watchdog;
    std::call_once(watchdog, [] {
        const char* e = getenv("WWHD_NO_WATCHDOG");
        if (e && *e && strcmp(e, "0")) LOG("[watchdog] off (WWHD_NO_WATCHDOG)");
        else std::thread(watchdog_thread).detach();
    });
    set_default_state();
    LOG("[gx2] initialized (native GX2 -> %s)", render::api_name(render::active()));
    if(uncapped_benchmark())
        LOG("[gx2 benchmark] uncapped guest flips; GPU completion ordering retained; frame-based simulation accelerates while timebase/audio clocks remain real-time");
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    if (Recording* rec = claim_rec()) {
        rec->start = rec->pos = arg(c, 0);
        rec->end = arg(c, 0) + arg(c, 1);
        g_rec_blocks[rec - g_recs].clear();
        g_rec_calls[rec - g_recs].clear();
        if (!rec->start) release_rec(rec);  // as before: a null list records nothing
    }
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = 0;
    if (Recording* rec = recording()) {
        size = rec->pos - rec->start;
        if (submitted_uniforms_on() && rec->start) {
            const size_t slot = rec - g_recs;
            g_dl_index.publish(rec->start, size, g_rec_blocks[slot], g_rec_calls[slot]);
            g_rec_blocks[slot].clear();
            g_rec_calls[slot].clear();
        }
        if (rec->dropped) {
            static unsigned said = 0;
            if (said < 32 && ++said) LOG("[gx2] display list overflow at %08X (%u bytes): %u bytes of commands dropped%s", rec->start, rec->end - rec->start, rec->dropped,
                                         said == 32 ? " (no more of these lines)" : "");
        }
        release_rec(rec);
    }
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    const Recording* rec = recording();
    const uint32 start = rec ? rec->start : 0;
    if (arg(c, 0)) st32(arg(c, 0), start);
    if (arg(c, 1)) st32(arg(c, 1), start ? rec->end - rec->start : 0);
    ret(c, start != 0);
}
// A list called while another is recorded runs with that one (its blocks are noted for it); a call
// from the command stream takes the blocks of the list as they are now with it (submitted_uniforms.h).
static void call_display_list(uint32 list, uint32 size) {
    if (Recording* rec = recording(); rec && rec->start) {
        if (submitted_uniforms_on()) g_rec_calls[rec - g_recs].push_back(list);
        emit(OP_CALL, {list, size});
        return;
    }
    submitted::Snapshot* s = submit_snapshot(list, size);
    if (s) emit_host(OP_SUBMITTED_UNIFORMS, {uint32(uintptr_t(s)), uint32(uint64_t(uintptr_t(s)) >> 32)});
    emit(OP_CALL, {list, size});
    if (s) emit_host(OP_SUBMITTED_UNIFORMS, {});
}
HLE(gx2, GX2CallDisplayList) { call_display_list(arg(c, 0), arg(c, 1)); }
HLE(gx2, GX2DirectCallDisplayList) { call_display_list(arg(c, 0), arg(c, 1)); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { mod_actor_shadow_proof::draw_submitted(arg(c,1));emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { mod_actor_shadow_proof::draw_submitted(arg(c,1));emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(std::vector<uint32>& p, uint32 addr, uint32 words) {
    size_t at = p.size();
    p.resize(at + words);
    if (addr) memcpy(&p[at], mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p.push_back(fbits(farg(c, i)));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), arg(c, 1) & 0xFF, arg(c, 2)});
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    put_struct(p, arg(c, 1), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)), fbits(farg(c, 4)),
                       arg(c, 2) & 0xFF, arg(c, 3)});
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
HLE(gx2, GX2CopySurface) {
    // debug: WWHD_COPYDBG=1 logs each copy as issued (thread, caller, source and destination images)
    static const bool dbg = getenv("WWHD_COPYDBG") != nullptr;
    if (dbg)
        LOG("[copydbg] issue t=%.3f thread %08X lr %08X src %08X img %08X dst %08X img %08X size %X", timebase::now() / (double)timebase::kTicksPerSec,
            threads::current_thread(), c->lr, arg(c, 0), ld32(arg(c, 0) + 0x24), arg(c, 3), ld32(arg(c, 3) + 0x24), ld32(arg(c, 3) + 0x20));
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kSurfaceWords);
    p.insert(p.end(), {arg(c, 1), arg(c, 2)});
    put_struct(p, arg(c, 3), kSurfaceWords);
    p.insert(p.end(), {arg(c, 4), arg(c, 5)});
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // The copy is complete when GX2CopySurface returns: the game uses the result (and frees the
    // surfaces) right away. agl's tile-mode conversion (027B5EEC) copies into a temporary surface,
    // OSBlockMoves it back and frees it at once; executed later on the render thread, the copy
    // wrote into the freed memory after the heap had reused it (boot crash: agl shader program
    // array 21EFE28C, program 0's +0x7c zeroed). Not for display lists (they run when called).
    if (Recording* rec = recording(); !rec || !rec->start) {
        BlockingScope b;
        render_sync(kSyncCopySurface);
    }
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    p.push_back(arg(c, 1));
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) { emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)}); }

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { emit_host(OP_FLUSH, {}); }
// (diagnostics) WWHD_TRACE_DRAWDONE=1: the game's GX2DrawDone calls by guest caller, every 600 swaps: per frame, how
// long the game waited for the render thread, and where in the frame (ms after the previous swap, game thread) it came
namespace drawdone_trace {
std::atomic<int64_t> lastSwapNs{0};
void on_swap() { lastSwapNs.store(std::chrono::steady_clock::now().time_since_epoch().count()); }
}  // namespace drawdone_trace
HLE(gx2, GX2DrawDone) {
    BlockingScope b;
    static const bool trace = getenv("WWHD_TRACE_DRAWDONE") != nullptr;
    // PERF-07a: on by default; WWHD_GX2_SKIP_SAFE_DRAWDONE=0 always waits (as before)
    static const bool skipSafe = [] {
        const char* e = getenv("WWHD_GX2_SKIP_SAFE_DRAWDONE");
        const bool on = !e || strcmp(e, "0");
        LOG("[gx2] DrawDone: %s", on ? "a wait with nothing it protects queued is skipped (WWHD_GX2_SKIP_SAFE_DRAWDONE=0 always waits)"
                                     : "always waits (WWHD_GX2_SKIP_SAFE_DRAWDONE=0)");
        return on;
    }();
    const auto started = std::chrono::steady_clock::now();
    const bool safe = g_render_thread && nothing_guarded_pending();
    uint32_t since[OP_COUNT + 1] = {};
    if (trace) {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        memcpy(since, g_ops_since, sizeof since);
        memset(g_ops_since, 0, sizeof g_ops_since);
    }
    emit_host(OP_DRAW_DONE, {});
    const bool skipped = safe && skipSafe;
    if (!skipped) render_sync(kSyncDrawDone);
    if (trace) {
        struct Site { uint64_t calls = 0, safe = 0, skipped = 0; double waitMs = 0, atMs = 0; uint64_t ops[OP_COUNT + 1] = {}; };
        static std::unordered_map<uint64_t, Site> sites;
        static uint64_t swaps0 = 0;
        // the caller and its caller (the guest's back chain)
        uint32_t sp = c->r[1], up = 0;
        if (sp) { const uint32_t prev = ld32(sp); if (prev > sp) up = ld32(prev + 4); }
        auto& site = sites[uint64_t(c->lr) << 32 | up];
        ++site.calls;
        site.safe += safe;
        site.skipped += skipped;
        for (uint32_t k = 0; k <= OP_COUNT; ++k) site.ops[k] += since[k];
        site.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        site.atMs += double(started.time_since_epoch().count() - drawdone_trace::lastSwapNs.load()) / 1e6;
        const uint64_t swaps = render::frame_count();
        if (swaps >= swaps0 + 600) {
            const double frames = swaps0 ? double(swaps - swaps0) : 600.0;
            for (auto& [key, v] : sites) {
                LOG("[gx2] DrawDone from %08X (<- %08X): %.2f per frame, waits %.2f ms each, %.2f ms after the swap; "
                    "nothing guarded queued %.0f%%, skipped %.0f%%",
                    uint32_t(key >> 32), uint32_t(key), v.calls / frames, v.waitMs / double(v.calls), v.atMs / double(v.calls),
                    100.0 * double(v.safe) / double(v.calls), 100.0 * double(v.skipped) / double(v.calls));
                // the commands queued since the previous GX2DrawDone, by kind (per call), guarded ones marked *
                char buf[600];
                int k = snprintf(buf, sizeof buf, "[gx2]   queued before it, per call:");
                for (uint32_t op = 0; op <= OP_COUNT && k < int(sizeof buf) - 24; ++op)
                    if (v.ops[op])
                        k += snprintf(buf + k, sizeof buf - k, " op%u%s %.1f", op,
                                      op < OP_COUNT && guarded(op, nullptr, 0) ? "*" : "", double(v.ops[op]) / double(v.calls));
                LOG("%s", buf);
            }
            sites.clear();
            swaps0 = swaps;
        }
    }
    ret(c, 1);
}
namespace dtrace { void on_swap(uint64_t swap, uint64_t steps); }
// (VR fork) Prepare shaders at start (gfx/shader_prepare.h): the game's first frame waits while the Vulkan warm-up
// builds the saved pipelines, at most 3 minutes; progress in the log every 2 s
static void wait_for_prepared_shaders() {
    const auto began = std::chrono::steady_clock::now();
    auto nextLog = began;
    LOG("[shaders] prepare shaders at start: the game waits while the saved pipelines are built");
    while (shaderprep::active.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - began > std::chrono::minutes(3)) {
            LOG("[shaders] prepare shaders at start: still building after 3 minutes; the game goes on (%zu of %zu ready)",
                shaderprep::done.load(), shaderprep::total.load());
            return;
        }
        if (now >= nextLog) {
            if (const size_t total = shaderprep::total.load())
                LOG("[shaders] preparing shaders: %zu of %zu", shaderprep::done.load(), total);
            nextLog = now + std::chrono::seconds(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    LOG("[shaders] prepare shaders at start: %zu pipelines in %.1f s", shaderprep::total.load(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
}
HLE(gx2, GX2SwapScanBuffers) {
    if (shaderprep::active.load(std::memory_order_acquire)) wait_for_prepared_shaders();
    drawdone_trace::on_swap();
    const uint64_t watchSwap = g_watch_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
    // test aid for the watchdog: WWHD_TEST_HANG_AT=<swap>:<seconds> stops the game's thread in that swap
    static const std::pair<uint64_t, unsigned> hangAt = [] {
        const char* e = getenv("WWHD_TEST_HANG_AT");
        unsigned long long at = 0;
        unsigned secs = 0;
        if (e && sscanf(e, "%llu:%u", &at, &secs) == 2) return std::pair<uint64_t, unsigned>{at, secs};
        return std::pair<uint64_t, unsigned>{0, 0};
    }();
    if (hangAt.first && watchSwap == hangAt.first) {
        LOG("[test] WWHD_TEST_HANG_AT: the game's thread stops for %u s at swap %llu", hangAt.second, (unsigned long long)watchSwap);
        std::this_thread::sleep_for(std::chrono::seconds(hangAt.second));
    }
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    gfx::display_read_gamepad_ui(); // read-only; no guest reads outside auto or a pending pause view
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    uint32 ab;
    memcpy(&ab, &a, 4);
    // the frame drew no new logic step (an interpolation hold pass): for the render-thread profiler
    static uint64_t lastSteps = ~0ull;
    const uint64_t steps = interp::logic_steps();
    const bool hold = steps == lastSteps;
    lastSteps = steps;
    {
        static const bool load = getenv("WWHD_TEST_GAME_LOAD_MS") != nullptr;
        if (load) test_frame_load("WWHD_TEST_GAME_LOAD_MS", hold);
    }
    // Opt-in correctness capture: select the guest frame before it enters
    // the asynchronous render queue. Renderer-frame parity is not a game clock.
    static const char* captureCounter = getenv("WWHD_TEST_CAPTURE_LOAD_COUNTER");
    static const char* captureStep = getenv("WWHD_TEST_CAPTURE_LOAD_STEP");
    bool capture = false;
    // the last full-state load; without one, a portable state's first frame in its stage (fixed seed)
    const bool fullLoad = ss::last_load_frame() != 0;
    const uint64_t loaded = fullLoad ? ss::last_load_frame() : ss::seeded_arrival_frame();
    if ((captureCounter || captureStep) && loaded) {
        static uint64_t capturedLoad = 0;
        const uint32_t counter = ld32(GD(0x101FF560));
        const uint64_t offset = captureStep ? steps - (fullLoad ? ss::last_load_step() : ss::seeded_arrival_step())
                                            : uint32_t(counter - (fullLoad ? ss::last_load_counter() : ss::seeded_arrival_counter()));
        const uint64_t target = strtoull(captureStep ? captureStep : captureCounter, nullptr, 10);
        if (capturedLoad != loaded && !hold && offset == target) {
            capturedLoad = loaded;
            capture = true;
            LOG("[test] capture at loaded %s +%llu (game counter %08X), full pass",
                captureStep ? "logic step" : "game counter",(unsigned long long)offset,counter);
        }
    }
    static const bool observeSwap = present_observer::enabled();
    uint64_t observedSwap = 0;
    // even frame delivery (interp.h frame_interval_ns): the frame's intended distance to the
    // previous one travels with its swap (ns; 0: present it as soon as it is ready)
    const uint32_t interval = uncapped_benchmark() ? 0u : uint32_t(std::clamp<int64_t>(interp::frame_interval_ns(), 0, 1'000'000'000));
#ifdef WWHD_VR
    // VR (vr/vr.h): the id of the head views this frame was drawn with travels with its swap
    const uint32_t vrViews = vr::active() ? vr::take_frame_views() : 0u;
#else
    const uint32_t vrViews = 0;
#endif
    // word 2: bit 0 capture, bit 1 the view cut in this frame (render::note_view_cut: new temporal histories)
    const uint32_t swapFlags = (capture ? 1u : 0u) | (render::take_view_cut() ? 2u : 0u);
    if (observeSwap || vrViews) {
        if (observeSwap) {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            if (g_swap_count == UINT64_MAX) present_observer::fail("diagnostic swap ID overflow");
            else observedSwap = g_swap_count + 1;
        }
        // Explicit ID travels with this command; never sample mutable producer
        // counters or drawable-acquisition timing on the render thread.
        emit_host(OP_SWAP, {ab, hold ? 1u : 0u, swapFlags,
                           uint32_t(observedSwap), uint32_t(observedSwap >> 32), interval, vrViews});
    } else if (interval) emit_host(OP_SWAP, {ab, hold ? 1u : 0u, swapFlags, 0u, 0u, interval});
    else if (swapFlags) emit_host(OP_SWAP, {ab, hold ? 1u : 0u, swapFlags});
    else emit_host(OP_SWAP, {ab, hold ? 1u : 0u});
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        g_swap_count++;
        if (observeSwap && (!observedSwap || observedSwap != g_swap_count))
            present_observer::fail("diagnostic swap ID ordering/reset mismatch");
        g_pending_flips.push_back({g_swap_count, std::chrono::steady_clock::now(), !hold});
    }
    // depth peeks of this frame (the sun's glare test) are answered before the game's next step reads
    // them, as on the Wii U (gfx/depth_peek.h)
    gfx::depth_peek::wait_answered();
    dtrace::on_swap(g_swap_count, steps);
    // debug: WWHD_LOG_SLOW_SWAP=ms logs swaps that came more than ms after the previous one
    static const double slow_ms = getenv("WWHD_LOG_SLOW_SWAP") ? atof(getenv("WWHD_LOG_SLOW_SWAP")) : 0;
    if (slow_ms > 0) {
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        if (ms > slow_ms) {
            LOG("[gx2] slow swap %llu: %.1f ms", (unsigned long long)g_swap_count, ms);
            thread_sampler::mark();  // (with WWHD_SAMPLE_THREADS: the samples before it show the slow frame)
        }
    }
    // Console/file output is synchronous: routine performance reporting must not
    // introduce a stall on the game thread during ordinary play.
    static const bool report_stats = getenv("WWHD_VK_STATS") || getenv("WWHD_LOG_FLIP_PACING") || getenv("WWHD_SCHED_STATS");
    if (report_stats && g_swap_count % 300 == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u", (unsigned long long)g_swap_count, 300 / s, g_swap_interval);
        if (getenv("WWHD_SCHED_STATS")) threads::report_sched();
    }
}
HLE(gx2, GX2GetSwapStatus) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(g_flip_count + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
HLE(gx2, GX2WaitForVsync) {
    if(uncapped_benchmark()) {
        BlockingScope b;
        // The queue fence follows earlier swaps, whose presentation path waits
        // for GPU completion. Never wait while holding the flip mutex.
        render_sync(kSyncVsyncUncapped);
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        return;
    }
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipWait = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_WAIT");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    if(readyFlipWait) {
        bool eligible = false, needsSync = false;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            if(!g_pending_flips.empty()) {
                const auto& front = g_pending_flips.front();
                const uint64_t at = flip_due(front);
                eligible = at <= vsync_index();
                if(eligible) needsSync = render::frames_completed() < front.swap;
            }
        }
        if(eligible) {
            if(needsSync) {
                BlockingScope b;
                render_sync(kSyncVsyncFlip); // Queued swap completion; never hold flip mutex.
            }
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips(); // Retains minimum interval and FIFO GPU guards.
            return;
        }
    }
#endif
    static const bool preciseSleep = [] {
#ifdef WWHD_HAS_VULKAN
        // Vulkan renderer's pacing (docs/vulkan.md); the Metal renderer keeps plain sleeping
        if (!render::vulkan()) return false;
        const char* value = getenv("WWHD_VSYNC_PRECISE");
#ifdef __APPLE__
        // Avoid the measured macOS sleep overshoot; explicit zero opts out.
        return !value || atoi(value) != 0;
#else
        return value && atoi(value) != 0;
#endif
#else
        return false;
#endif
    }();
    const uint64_t granule = vsync_granule();
    const auto deadline = tick_time((vsync_index() / granule + 1) * granule);  // the next (virtual) vsync
    if (wait_for_late_flip(deadline)) return;
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipPark = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_PARK");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    threads::park_sleep_until(deadline, preciseSleep,
        readyFlipPark ? ready_flip_before_resume : nullptr);
#else
    threads::park_sleep_until(deadline, preciseSleep);
#endif
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    static uint64_t calls = 0;
    if (getenv("WWHD_LOG_VSYNC") && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    LOG("[gx2] TV buffer format %X", arg(c, 3));
    render::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { render::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {1});  // full GPU wait, also with lazy DrawDone
    render_sync(kSyncSaveState);
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    w.u32(kNumRegs);
    w.bytes(g_regs, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : g_contexts) {
        keys.push_back(k);
        if (g_shadow == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)g_contexts[k].size());
        w.bytes(g_contexts[k].data(), g_contexts[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_validate(ss::Reader r, std::string& why) {
    if (!ss::metadata::gx2(r,kNumRegs)) { why = "corrupt gx2 section"; return false; }
    return true;
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (!gx2_ss_validate(r,why)) return false;
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    g_held.on = false;  // (a held run of the frame the state replaces is dropped)
    g_last.valid = false;
    r.bytes(g_regs, sizeof g_regs);
    g_change_log.reset();
    g_register_blocks = {};
    g_register_blocks.include(g_regs);
    if (g_observer) g_observer->load();  // (a replay machine goes on from the loaded registers)
    if (g_forward) g_forward->resync(g_regs);
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
        if (words == kNumRegs) g_register_blocks.include(v.data());
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_shader_state_gen++;
    note_full_state_change();
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    stereo::layoutDrawDepth = 0;
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    render::ss_reset();  // the renderer forgets surface contents and shader memos (Vulkan)
    g_dl_index.clear();  // lists recorded before the load say nothing about the loaded ones
    set_active_snapshot(nullptr);
}

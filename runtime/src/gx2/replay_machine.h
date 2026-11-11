// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// A second copy of the GX2 register machine for a thread that replays part of the command stream (docs/
// VR_ATLAS_THREAD.md, stage 2.9: the sun atlas's draws recorded on a thread of their own).
//
// The renderer's draws read the register file and three counters derived from its writes: the shader-state generation
// (shader lookups are reused while it is unchanged), the full-state version and the dirty register groups (the
// continued-draw fast path, state_groups.h). On the render thread they are the GX2 machine's own (gx2_core.cpp). A
// replay machine has its own register file and counters, and applies register writes with exactly the classification
// apply_regs uses (the same read-only tables), so its draws keep the same fast paths and memos. The counters' values
// differ from the render thread's; each thread's memos compare only its own.
//
// WWHD_GX2_REPLAY_VERIFY=1 (diagnostics): the render thread feeds a replay machine with every register write it
// applies and checks that both decided the same (registers, generation and version bumps, dirty groups); mismatches are
// counted and logged.
#pragma once
#include <cstdint>

#include "per_thread.h"
#include <algorithm>
#include <memory>
#include <vector>

namespace gx2 {

// The register changes that end continuations (outside kSgContinuedMask), each with the value it replaced, in order:
// the continued-draw fast path's recent states (draw.cpp, issue #1) are reused while every register changed since one
// holds the value it had then. A full-state change (context and save-state loads) forgets them all (reset).
struct ChangeLog {
    static constexpr uint32_t kSize = 256;
    struct Entry { uint32_t reg, old; };
    uint64_t pos = 0, resetPos = 0;  // changes logged; the first change after the last reset
    Entry entries[kSize];
    // (2026-11-07; draw.cpp resolve_replay_targets on the scene's drawing threads) a key of the logged registers'
    // values per register group (state_groups.h): the XOR, over every register of the group changed since the last
    // reset, of mix(its value now) ^ mix(its value at the reset), kept as each change is logged. Two moments with the
    // same reset count and the same registers in a group have the same key there, however many changes lie between
    // them (a change and its undoing cancel). keyMask: the bits keyed (DB_STENCILREFMASK(_BF): not the reference byte,
    // which changes without a log entry, a dynamic word; so the keyed bits always change through the log). Kept only
    // in a drawing thread's replay machine (keyed, ReplayMachine's constructor): the render thread's own log keeps none
    // (no work added there).
    static constexpr uint32_t kGroups = 11;  // (kSgCount, static_assert in gx2_core.cpp)
    uint64_t groupHash[kGroups] = {};
    uint64_t resets = 0;
    bool keyed = false;
    static uint64_t mix(uint32_t reg, uint32_t value) {
        uint64_t x = (uint64_t(reg) << 32 | value) + 0x9E3779B97F4A7C15ull;  // (splitmix64)
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }
    void add(uint32_t reg, uint32_t old, uint32_t value, uint32_t keyMask = ~0u, uint32_t group = kGroups - 1) {
        entries[pos++ % kSize] = {reg, old};
        if (keyed) groupHash[group < kGroups ? group : kGroups - 1] ^= mix(reg, old & keyMask) ^ mix(reg, value & keyMask);
    }
    void reset() {
        resetPos = pos;
        for (auto& g : groupHash) g = 0;
        ++resets;
    }
    // whether the registers in regs equal those at log position from, and the changes since fit in at most limit entries
    // (one pass: a register's first change since from holds its value then; seen marks the registers already checked).
    // Registers skipCount from skipFirst are not compared; *skipped: whether one of them changed.
    bool unchanged_since(uint64_t from, const uint32_t* regs, uint32_t limit, uint32_t skipFirst = 0, uint32_t skipCount = 0,
                         bool* skipped = nullptr) const {
        if (skipped) *skipped = false;
        if (from < resetPos || pos - from > limit || pos - from > kSize) return false;
        if (!seen) seen.reset(new uint32_t[1u << 16]());
        if (++check == 0) { std::fill_n(seen.get(), 1u << 16, 0u); check = 1; }
        for (uint64_t i = from; i < pos; ++i) {
            const Entry& e = entries[i % kSize];
            uint32_t& mark = seen[e.reg & 0xFFFF];
            if (mark == check) continue;
            mark = check;
            if (e.reg - skipFirst < skipCount) {
                if (skipped && regs[e.reg] != e.old) *skipped = true;
                continue;
            }
            if (regs[e.reg] != e.old) return false;
        }
        return true;
    }
    mutable std::unique_ptr<uint32_t[]> seen;
    mutable uint32_t check = 0;
};

// the counters a thread's draws read
struct StateCounters {
    uint64_t* shaderStateGen = nullptr;
    uint64_t* fullStateVersion = nullptr;
    uint32_t* dirtyGroups = nullptr;
    ChangeLog* changeLog = nullptr;
};
// the GX2 machine's own (gx2_core.cpp fills it at start)
inline StateCounters g_machineCounters;
// this thread's: the GX2 machine's own unless a replay machine is attached (a direct thread-local load: read for every
// draw)
PER_THREAD(const StateCounters*, threadCounters, &g_machineCounters)  // (per_thread.h)
inline const StateCounters& thread_state_counters() { return *threadCounters(); }
inline uint64_t shader_state_gen() { return *thread_state_counters().shaderStateGen; }
inline uint64_t full_state_version() { return *thread_state_counters().fullStateVersion; }
inline const ChangeLog& thread_change_log() { return *thread_state_counters().changeLog; }
inline uint32_t take_thread_dirty_groups() {
    uint32_t* d = thread_state_counters().dirtyGroups;
    const uint32_t g = *d;
    *d = 0;
    return g;
}

class ReplayMachine {
public:
    ReplayMachine();
    // the GX2 machine's registers as they are now (render thread); every counter bumped, every group dirty
    void load();
    // a register write, as apply_regs applies it
    void apply(uint32_t first, const uint32_t* values, uint32_t count);
    // the whole register file replaced (a context load, or anything else that changed it): as set_context counts it
    void restore(const uint32_t* regs);
    // its two parts: the registers (and the generation, if a shader key changed), and the full-state change
    void load_regs(const uint32_t* regs);
    // load_regs for registers that differ only in some blocks: words holds each block's index, then its blockWords
    // values (RegisterObserver::load_blocks)
    void load_blocks(const uint32_t* words, uint32_t count, uint32_t blockWords);
    void note_full_state_change();
    uint32_t* regs() { return regs_.data(); }
    // this thread's draws read this machine's counters (until detach)
    void attach();
    static void detach();
    uint64_t shader_gen() const { return shaderGen_; }
    uint64_t full_version() const { return fullVersion_; }
    uint32_t dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = 0; }  // (the verifier)

private:
    std::vector<uint32_t> regs_;
    uint64_t shaderGen_ = 1, fullVersion_ = 1;
    uint32_t dirty_ = 0;
    ChangeLog log_;
    StateCounters counters_;
};

// Every register change the render thread makes, passed on (the sun atlas's thread queues them for its replay machine,
// in order with its draws): a write with the values applied, a context load's registers, a full-state change, and a
// resync with the whole file (a save-state load)
class RegisterObserver {
public:
    virtual ~RegisterObserver() = default;
    virtual void apply(uint32_t first, const uint32_t* values, uint32_t count) = 0;
    virtual void load_regs(const uint32_t* regs) = 0;
    // (docs/VR_SCENE_THREADS.md, "register loads by block") a context load that changed registers only in these blocks
    // of blockWords: the same as load_regs, for an observer whose registers are the render thread's until now
    virtual void load_blocks(const uint32_t* regs, const uint32_t* blocks, uint32_t count, uint32_t blockWords) {
        (void)blocks; (void)count; (void)blockWords;
        load_regs(regs);
    }
    virtual void note_full_state_change() = 0;
    virtual void resync(const uint32_t* regs) = 0;
};
// (render thread) where the changes go from now on; null: nowhere
void set_register_forward(RegisterObserver* observer);
// (render thread) the GX2 machine's registers as they are now (docs/VR_SCENE_THREADS.md, stage S6: a running draw
// thread's reload, queued as a load_regs job)
const uint32_t* render_regs();

// (diagnostics, WWHD_GX2_REPLAY_VERIFY=1) writes checked and mismatches found since the start
struct ReplayVerifyStats { uint64_t writes = 0, mismatches = 0; };
ReplayVerifyStats replay_verify_stats();

}  // namespace gx2

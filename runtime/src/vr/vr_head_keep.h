// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Link's head, hidden in first person, given back (2026-11-20, a tester in the Tower of the Gods: "there is a cutscene
// and link steps into the switch his head is missing during the scene").
//
// hide_head (vr_game.cpp) folds the head's, neck's and hair's joint matrices onto the neck in Link's own matrices, not
// in a copy. In play the game works them out again every step, so the fold lasted one draw. A cutscene that holds Link
// still (stepping on a switch while the camera shows the room) does not work them out again: the fold from the last
// first-person draw stood, and the cutscene showed him without a head (his body, hidden in a per-draw copy, showed).
//
// HeadKeep keeps each joint's matrix as the game made it before it is folded, and puts it back on a draw that does not
// hide the head, but only where the fold still stands: a joint the game has worked out since (any other values) is
// left alone, so a fresh pose is never overwritten. Raw words in guest byte order (12 words a joint: a 3x4 matrix);
// header only, no guest memory, so vr_test covers it.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace vr {

class HeadKeep {
public:
    // before a joint (at guest address `addr`, its words `m`) is folded: keep the game's matrix, unless it is still the
    // fold of the last time (then the kept one is still the game's)
    void before_fold(uint32_t addr, const uint32_t* m) {
        Entry* e = find(addr);
        if (e && e->hasAt && folded(m, e->at)) return;
        if (!e) {
            entries_.push_back({});
            e = &entries_.back();
            e->addr = addr;
        }
        std::memcpy(e->words, m, sizeof e->words);
        e->hasAt = false;
    }
    // after it is folded: what the fold looks like (its rotation all zero, its translation the point it went to)
    void after_fold(uint32_t addr, const uint32_t* m) {
        if (Entry* e = find(addr)) {
            e->at[0] = m[3];
            e->at[1] = m[7];
            e->at[2] = m[11];
            e->hasAt = true;
        }
    }
    // a draw that does not hide the head: the game's matrices back where the fold still stands. resolve(addr) gives
    // the joint's words, or nullptr where the address is no longer one of Link's (a new stage, another model: left).
    // Returns how many joints were given back; forgets them all
    template <class Resolve>
    int restore(Resolve resolve) {
        int n = 0;
        for (const Entry& e : entries_) {
            uint32_t* m = e.hasAt ? resolve(e.addr) : nullptr;
            if (m && folded(m, e.at)) {
                std::memcpy(m, e.words, sizeof e.words);
                ++n;
            }
        }
        entries_.clear();
        return n;
    }
    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

    // the fold's mark: the 3x3 part all zero and the translation the fold's point
    static bool folded(const uint32_t* m, const uint32_t at[3]) {
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                if (m[r * 4 + k] != 0) return false;
        return m[3] == at[0] && m[7] == at[1] && m[11] == at[2];
    }

private:
    struct Entry {
        uint32_t addr = 0;
        uint32_t words[12] = {};
        uint32_t at[3] = {};
        bool hasAt = false;
    };
    Entry* find(uint32_t addr) {
        for (Entry& e : entries_)
            if (e.addr == addr) return &e;
        return nullptr;
    }
    std::vector<Entry> entries_;
};

}  // namespace vr

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The previous frame's data of each draw (docs/VR_DLSS.md, stage D2b step 2a): what the motion variant
// (motion_glsl.h) binds as the previous frame's elements. Upload arenas are reused from frame to frame, so the store
// keeps CPU copies of the bytes each draw used (per eye: the uniform blocks are eye-shifted) and the draw uploads them
// again as its previous frame's.
//
// The render thread numbers this frame's draws in the game's order and keys each before it is recorded; the key
// finds the draw's previous data at once (the k-th draw of a key this frame is the k-th of the previous frame, as
// motion_match.h matches, keyed without the vertex buffer). Whichever thread records a draw (the scene's drawing threads
// included) fills only that draw's slot, so recording takes no lock; the render thread keys a draw before handing it
// over. A frame holds up to its capacity of draws (the previous frame's count with room to spare); draws past it, draws
// with no match, or whose previous data did not fit the budget, have no previous data (they write zero motion).
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "motion_match.h"

namespace motion {

// what a part of a draw's data is
// (the vertex stage's only: the motion variant computes the previous position there)
enum class Part : uint8_t { VertexBlock, VertexSupport, VertexBuffer };

struct DrawData {
    struct Piece {
        Part part;
        uint8_t slot;        // the block or vertex buffer index (support: 0)
        uint32_t offset;     // in bytes; kNoBytes: only referred to (guestAddr, size: the buffer cache serves it)
        uint32_t size;
        uint32_t guestAddr;  // where the game had it (vertex buffers; 0 for uniform data)
        bool referred() const { return offset == kNoBytes; }
    };
    static constexpr uint32_t kNoBytes = 0xFFFFFFFFu;
    std::vector<uint8_t> bytes;
    std::vector<Piece> pieces;
    bool used = false;  // something was recorded this frame
    void clear() {
        bytes.clear();
        pieces.clear();
        used = false;
    }
    void add(Part part, uint8_t slot, const void* data, uint32_t size, uint32_t guestAddr = 0) {
        const uint32_t at = uint32_t(bytes.size());
        const auto* p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);  // (one block copy; the vector's room is kept from frame to frame)
        pieces.push_back({part, slot, at, size, guestAddr});
        used = true;
    }
    // a vertex buffer the buffer cache serves: its address and size only (its data stays where it is)
    void add_ref(Part part, uint8_t slot, uint32_t guestAddr, uint32_t size) {
        pieces.push_back({part, slot, kNoBytes, size, guestAddr});
        used = true;
    }
    const Piece* find(Part part, uint8_t slot) const {
        for (const Piece& p : pieces)
            if (p.part == part && p.slot == slot) return &p;
        return nullptr;
    }
};

class Store {
public:
    explicit Store(size_t budgetBytes = size_t(64) << 20) : budget_(budgetBytes) {}

    // render thread, at the frame's start: room for `capacity` draws (both eyes each)
    void begin_frame(uint32_t capacity) {
        if (cur_.size() < size_t(capacity) * 2) cur_.resize(size_t(capacity) * 2);
        for (size_t i = 0; i < size_t(capacity) * 2; ++i) cur_[i].clear();
        capacity_ = capacity;
        draws_ = 0;
        seen_.assign(capacity, Seen{});
        lookup_.assign(capacity, -1);
        byKey_.clear();
        for (int32_t i = 0; i < int32_t(prevSeen_.size()); ++i) byKey_[prevSeen_[i].key].push_back(i);
        taken_.clear();
    }
    // render thread, in order, before the draw is recorded: the key of draw `index`, and its previous frame's draw
    // (without the vertex buffer: its address moves every frame for about half the draws, docs/VR_DLSS.md D2a)
    void key(uint32_t index, DrawKey k) {
        if (index >= capacity_) {
            ++beyond_;
            return;
        }
        k.vertexAddr = k.vertexSize = 0;
        const uint64_t h = k.hash();
        seen_[index] = {h, 0};
        if (index >= draws_) draws_ = index + 1;
        auto it = byKey_.find(h);
        uint32_t& n = taken_[h];
        if (it != byKey_.end() && n < it->second.size()) lookup_[index] = it->second[n++];
    }
    // any thread, its own draw's slot only: the data `index` was drawn with for `eye`
    DrawData* slot(uint32_t index, int eye) { return index < capacity_ ? &cur_[size_t(index) * 2 + (eye ? 1 : 0)] : nullptr; }
    // any thread: draw `index`'s previous data for `eye` (null: none)
    const DrawData* previous(uint32_t index, int eye) const {
        if (index >= capacity_) return nullptr;
        const int32_t p = lookup_[index];
        if (p < 0 || uint32_t(p) >= prevDraws_) return nullptr;
        const DrawData& d = prev_[size_t(p) * 2 + (eye ? 1 : 0)];
        return d.used ? &d : nullptr;
    }
    // render thread, at the frame's end (every draw recorded): this frame becomes the previous one, within the budget
    // (in draw order: the first draws are kept); returns how this frame matched the previous one
    Stats end_frame() {
        seen_.resize(draws_);
        const Stats s = match_frames(prevSeen_, seen_);
        size_t total = 0;
        over_ = 0;
        for (size_t i = 0; i < size_t(draws_) * 2; ++i) {
            if (total + cur_[i].bytes.size() > budget_) {
                cur_[i].clear();
                ++over_;
                continue;
            }
            total += cur_[i].bytes.size();
        }
        prev_.swap(cur_);
        prevDraws_ = draws_;
        prevSeen_ = seen_;
        bytes_ = total;
        return s;
    }
    uint32_t draws() const { return draws_; }      // this frame's draws so far (the next capacity's base)
    uint64_t beyond() const { return beyond_; }    // draws past a frame's capacity, ever
    size_t bytes() const { return bytes_; }     // the previous frame's kept bytes
    uint32_t over_budget() const { return over_; }  // slots dropped for the budget at the last end_frame

private:
    size_t budget_;
    uint32_t capacity_ = 0, draws_ = 0, prevDraws_ = 0, over_ = 0;
    uint64_t beyond_ = 0;
    size_t bytes_ = 0;
    std::vector<DrawData> cur_, prev_;
    std::vector<Seen> seen_, prevSeen_;
    std::vector<int32_t> lookup_;  // this frame's draw -> the previous frame's (key)
    std::unordered_map<uint64_t, std::vector<int32_t>> byKey_;  // the previous frame's draws of each key, in order
    std::unordered_map<uint64_t, uint32_t> taken_;
};

}  // namespace motion

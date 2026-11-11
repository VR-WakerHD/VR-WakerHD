// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Draws matched across frames (docs/VR_DLSS.md, stage D2): motion vectors need, for each draw, the same draw of the
// previous displayed frame (its transforms then). A draw is known by what does not change from frame to frame for the
// same object: its shaders, vertex buffer, indices and counts (DrawKey). Draws with the same key match in order: the
// k-th draw of a key this frame is the k-th of the previous frame. No Vulkan here; tests drive it directly.
#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace motion {

struct DrawKey {
    uint32_t vs = 0, ps = 0;           // shader program addresses
    uint32_t vertexAddr = 0, vertexSize = 0;  // the first vertex buffer
    uint32_t indexAddr = 0, count = 0, indexType = 0, baseVertex = 0, instances = 0, prim = 0;
    uint64_t hash() const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : {vs, ps, vertexAddr, vertexSize, indexAddr, count, indexType, baseVertex, instances, prim}) {
            h ^= v;
            h *= 1099511628211ull;
        }
        return h;
    }
};

// one draw as seen this frame: its key and a hash of the transforms it was drawn with (its vertex uniform blocks)
struct Seen {
    uint64_t key = 0;
    uint64_t transforms = 0;
};

struct Stats {
    uint64_t draws = 0;       // this frame's draws
    uint64_t unique = 0;      // matched, the only draw of its key in both frames
    uint64_t ordered = 0;     // matched by order among several draws of the same key
    uint64_t unmatched = 0;   // no draw of the previous frame left for it (new, or more of its key than before)
    uint64_t gone = 0;        // previous frame's draws nothing matched
    uint64_t still = 0;       // matched with the same transforms (nothing moved, the camera neither)
    uint64_t countChanged = 0;  // matched although its key's count differed between the frames (order may be wrong)
    void add(const Stats& o) {
        draws += o.draws; unique += o.unique; ordered += o.ordered; unmatched += o.unmatched; gone += o.gone;
        still += o.still; countChanged += o.countChanged;
    }
};

// matches `cur` against `prev`; `match[i]` is the index in prev of cur[i]'s draw, or -1
inline Stats match_frames(const std::vector<Seen>& prev, const std::vector<Seen>& cur, std::vector<int32_t>* match = nullptr) {
    Stats s;
    s.draws = cur.size();
    std::unordered_map<uint64_t, std::vector<int32_t>> byKey;  // prev indices of each key, in order
    byKey.reserve(prev.size());
    for (int32_t i = 0; i < int32_t(prev.size()); ++i) byKey[prev[i].key].push_back(i);
    std::unordered_map<uint64_t, uint32_t> curCount, taken;
    curCount.reserve(cur.size());
    for (const Seen& d : cur) ++curCount[d.key];
    if (match) match->assign(cur.size(), -1);
    uint64_t matched = 0;
    for (size_t i = 0; i < cur.size(); ++i) {
        auto it = byKey.find(cur[i].key);
        uint32_t& k = taken[cur[i].key];
        if (it == byKey.end() || k >= it->second.size()) {
            ++s.unmatched;
            continue;
        }
        const int32_t p = it->second[k++];
        ++matched;
        const size_t prevCount = it->second.size(), nowCount = curCount[cur[i].key];
        if (prevCount == 1 && nowCount == 1) ++s.unique;
        else ++s.ordered;
        if (prevCount != nowCount) ++s.countChanged;
        if (prev[p].transforms == cur[i].transforms) ++s.still;
        if (match) (*match)[i] = p;
    }
    s.gone = prev.size() - matched;
    return s;
}

}  // namespace motion

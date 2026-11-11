// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Draws matched across frames (runtime/src/gfx/vulkan/motion_match.h): unique keys, several draws of one key matched
// in order, new and gone draws, unchanged transforms, and changed counts.
#include "gfx/vulkan/motion_match.h"
#include <cstdio>

using namespace motion;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

int main() {
    // keys: every field counts
    {
        DrawKey a{1, 2, 3, 4, 5, 6, 0, 0, 1, 4};
        DrawKey b = a;
        CHECK(a.hash() == b.hash());
        b.indexAddr = 7;
        CHECK(a.hash() != b.hash());
        b = a;
        b.vs = 9;
        CHECK(a.hash() != b.hash());
    }
    // the same frame twice: every draw matched to itself, all still
    {
        const std::vector<Seen> f{{10, 1}, {11, 2}, {12, 3}};
        std::vector<int32_t> m;
        const Stats s = match_frames(f, f, &m);
        CHECK(s.draws == 3 && s.unique == 3 && s.ordered == 0 && s.unmatched == 0 && s.gone == 0 && s.still == 3);
        CHECK(m[0] == 0 && m[1] == 1 && m[2] == 2);
    }
    // order changed between frames: unique keys still find their draws
    {
        const std::vector<Seen> prev{{10, 1}, {11, 2}, {12, 3}};
        const std::vector<Seen> cur{{12, 3}, {10, 9}, {11, 2}};
        std::vector<int32_t> m;
        const Stats s = match_frames(prev, cur, &m);
        CHECK(m[0] == 2 && m[1] == 0 && m[2] == 1);
        CHECK(s.unique == 3 && s.still == 2);  // key 10 moved
    }
    // several draws of one key (grass, the same mesh many times): matched in order
    {
        const std::vector<Seen> prev{{20, 1}, {20, 2}, {20, 3}};
        const std::vector<Seen> cur{{20, 1}, {20, 2}, {20, 3}};
        std::vector<int32_t> m;
        const Stats s = match_frames(prev, cur, &m);
        CHECK(s.ordered == 3 && s.unique == 0 && s.countChanged == 0);
        CHECK(m[0] == 0 && m[1] == 1 && m[2] == 2);
    }
    // a new draw and a gone one
    {
        const std::vector<Seen> prev{{30, 1}, {31, 1}};
        const std::vector<Seen> cur{{30, 1}, {32, 1}};
        std::vector<int32_t> m;
        const Stats s = match_frames(prev, cur, &m);
        CHECK(s.unique == 1 && s.unmatched == 1 && s.gone == 1);
        CHECK(m[0] == 0 && m[1] == -1);
    }
    // one more of a key this frame: the extra one unmatched, the others flagged as count-changed
    {
        const std::vector<Seen> prev{{40, 1}, {40, 2}};
        const std::vector<Seen> cur{{40, 1}, {40, 2}, {40, 3}};
        const Stats s = match_frames(prev, cur);
        CHECK(s.ordered == 2 && s.unmatched == 1 && s.countChanged == 2 && s.gone == 0);
    }
    // empty frames
    {
        const Stats s = match_frames({}, {{1, 1}});
        CHECK(s.unmatched == 1 && s.gone == 0);
        const Stats t = match_frames({{1, 1}}, {});
        CHECK(t.draws == 0 && t.gone == 1);
    }
    // stats add up
    {
        Stats a, b;
        a.draws = 2; a.unique = 1;
        b.draws = 3; b.unique = 2; b.gone = 1;
        a.add(b);
        CHECK(a.draws == 5 && a.unique == 3 && a.gone == 1);
    }
    if (failures) {
        std::fprintf(stderr, "motion_match_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("motion_match_test: ok\n");
    return 0;
}

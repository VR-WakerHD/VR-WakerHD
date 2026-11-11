// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The previous frame's draw data (runtime/src/gfx/vulkan/motion_store.h): slots per draw and eye, lookups through the
// match as each draw is keyed (without the vertex buffer), new draws, the capacity, the budget, and frames turning over.
#include "gfx/vulkan/motion_store.h"
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

static DrawKey key(uint32_t vs, uint32_t vertexAddr = 0x1000) {
    DrawKey k;
    k.vs = vs;
    k.ps = 1;
    k.vertexAddr = vertexAddr;
    k.vertexSize = 64;
    k.indexAddr = 0x2000;
    k.count = 36;
    k.instances = 1;
    k.prim = 4;
    return k;
}

int main() {
    Store store;
    // frame 1: two draws, each eye its own bytes
    store.begin_frame(2);
    store.key(0, key(10));
    store.key(1, key(11));
    CHECK(store.previous(0, 0) == nullptr);  // the first frame has nothing before it
    const uint8_t a0[4] = {1, 2, 3, 4}, a1[4] = {5, 6, 7, 8}, b0[2] = {9, 9};
    store.slot(0, 0)->add(Part::VertexSupport, 0, a0, 4);
    store.slot(0, 1)->add(Part::VertexSupport, 0, a1, 4);
    store.slot(1, 0)->add(Part::VertexBlock, 2, b0, 2);
    store.slot(1, 0)->add_ref(Part::VertexBuffer, 0, 0x8000, 4096);  // a cached buffer: referred to, no bytes
    store.slot(1, 1)->add(Part::VertexBuffer, 1, a1, 4, 0x9000);    // an uploaded one: copied, its address kept
    CHECK(store.slot(2, 0) == nullptr);  // only the frame's capacity has slots
    store.key(5, key(13));                // past the capacity: counted, not kept
    CHECK(store.beyond() == 1 && store.draws() == 2);
    store.end_frame();
    CHECK(store.bytes() == 14);  // (the referred-to buffer keeps no bytes)

    // frame 2: the draws in the other order, draw 10's vertex buffer at a new address, and a new draw
    store.begin_frame(3);
    store.key(0, key(11));
    store.key(1, key(10, 0x9000));  // still draw 10: the vertex buffer is not part of the match
    store.key(2, key(12));
    const DrawData* p = store.previous(1, 0);
    CHECK(p && p->find(Part::VertexSupport, 0) && p->bytes[0] == 1);
    p = store.previous(1, 1);
    CHECK(p && p->bytes[0] == 5);  // the right eye's own
    p = store.previous(0, 0);
    CHECK(p && p->find(Part::VertexBlock, 2) && !p->find(Part::VertexBlock, 1));
    {
        const DrawData::Piece* v = p ? p->find(Part::VertexBuffer, 0) : nullptr;
        CHECK(v && v->referred() && v->guestAddr == 0x8000 && v->size == 4096);
        const DrawData* r = store.previous(0, 1);
        const DrawData::Piece* u = r ? r->find(Part::VertexBuffer, 1) : nullptr;
        CHECK(u && !u->referred() && u->guestAddr == 0x9000 && r->bytes[u->offset] == 5);
    }
    CHECK(store.previous(2, 0) == nullptr);  // new draw: no previous data (zero motion)
    CHECK(store.previous(7, 0) == nullptr);
    const Stats s = store.end_frame();
    CHECK(s.unique == 2 && s.unmatched == 1);

    // frame 3: frame 2 recorded nothing, so nothing is found
    store.begin_frame(1);
    store.key(0, key(10));
    CHECK(store.previous(0, 0) == nullptr);
    store.end_frame();

    // the budget: the first draws kept, the rest dropped and counted
    {
        Store small(6);
        small.begin_frame(3);
        for (uint32_t i = 0; i < 3; ++i) {
            small.key(i, key(20 + i));
            const uint8_t bytes[4] = {};
            small.slot(i, 0)->add(Part::VertexSupport, 0, bytes, 4);
        }
        small.end_frame();
        CHECK(small.bytes() == 4 && small.over_budget() == 2);
        small.begin_frame(3);
        for (uint32_t i = 0; i < 3; ++i) small.key(i, key(20 + i));
        CHECK(small.previous(0, 0) != nullptr && small.previous(1, 0) == nullptr && small.previous(2, 0) == nullptr);
    }
    if (failures) {
        std::fprintf(stderr, "motion_store_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("motion_store_test: ok\n");
    return 0;
}

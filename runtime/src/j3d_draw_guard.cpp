// SPDX-License-Identifier: MPL-2.0
// Safety net: a J3D packet is never entered into a draw bucket twice, and a bucket whose packet list
// loops on itself anyway is cut before the game walks it (j3d_draw_guard.h). J3DDrawBuffer::drawHead
// (027F10CC), drawTail (027F1174) and frameInit (027F093C) walk every bucket until a packet's next
// link is 0; the entries (entryZSort 027F0C14, entryNonSort 027F0DA8, entryImm 027F0E04) put a packet
// at the front of its bucket without looking whether it is in one already. A packet entered twice before frameInit (an actor
// drawn twice in one frame) closes a loop, and the walk never ends: the game thread draws the same
// packets until its display list is full and keeps going (2026-10-21: the editor's scene preview hang;
// story_demo.cpp creates its actors in Link's layer now, which removed that cause). Here a second
// entry of a packet that is in a bucket already is left out (the packet is drawn once), and a loop
// that is there all the same has the link that closes it set to 0, so the frame finishes. Lists
// without a loop are left as they are; both cases are said in the log (rate-limited).
#include "j3d_draw_guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "runtime.h"
#include "guest_addr.h"
#include "gx2/gx2_cmd.h"
#ifdef WWHD_VR
#include "vr/vr_game.h"
#include "vr/vr.h"
#include "gfx/stereo.h"
#endif

extern "C" void f_027F10CC_orig(Cpu* c);  // J3DDrawBuffer::drawHead(this, context)
extern "C" void f_027F1174_orig(Cpu* c);  // J3DDrawBuffer::drawTail(this, context)
extern "C" void f_027F093C_orig(Cpu* c);  // J3DDrawBuffer::frameInit(this)
extern "C" void f_027F0C14_orig(Cpu* c);  // J3DDrawBuffer::entryZSort(this, packet)
extern "C" void f_027F0DA8_orig(Cpu* c);  // J3DDrawBuffer::entryNonSort(this, packet)
extern "C" void f_027F0E04_orig(Cpu* c);  // J3DDrawBuffer::entryImm(this, packet, bucket)

namespace {
// J3DDrawBuffer: +0 bucket count, +4 the buckets (one packet pointer each); J3DPacket +0x10: next
constexpr uint32_t kBucketCount = 0x0, kBuckets = 0x4, kNext = 0x10;
constexpr uint32_t kEntrySlot = 0x94;  // J3DPacket: the bucket it is in (0: none; frameInit and remove clear it)
constexpr uint32_t kMaxBuckets = 0x10000;  // more is not a draw buffer: left alone
std::atomic<unsigned> g_cuts{0}, g_skips{0};

bool guest_word(uint32_t address) { return address >= mem::kMem2Start && address <= mem::kMem2End - 4 && !(address & 3); }

void cut_loops(uint32_t buffer, const char* walk) {
    if (!guest_word(buffer) || !guest_word(buffer + kBuckets)) return;
    const uint32_t count = ld32(buffer + kBucketCount), buckets = ld32(buffer + kBuckets);
    if (!count || count > kMaxBuckets || !guest_word(buckets) || !guest_word(buckets + (count - 1) * 4)) return;
    // a link outside guest memory ends the walk here (the game's own walk would fault on it anyway)
    auto next = [](uint32_t packet) { return guest_word(packet + kNext) ? ld32(packet + kNext) : 0u; };
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t head = ld32(buckets + i * 4);
        const uint32_t last = j3d_draw_guard::loop_end(head, next);
        if (!last) continue;
        const uint32_t first = ld32(last + kNext);
        st32(last + kNext, 0);
        const unsigned n = ++g_cuts;
        if (n <= 4 || (n & (n - 1)) == 0)
            LOG("[j3d] draw buffer %08X bucket %u looped on itself (packet %08X entered twice; the loop closes at %08X): "
                "cut before %s so the frame goes on (%u so far)",
                buffer, i, first, last, walk, n);
    }
}
// A packet that is in a bucket already (entered earlier this frame and not yet reset by frameInit): a
// second entry would link it to the packets after it and close a loop, or cut the packets after its
// first place out of the list (they keep their bucket and the next entry of one of them asserts). It is
// queued for drawing already, so the second entry is left out. Entries of a packet in no bucket are the
// game's own.
bool entered_already(uint32_t packet, const char* entry) {
    if (!guest_word(packet + kEntrySlot)) return false;
    const uint32_t slot = ld32(packet + kEntrySlot);
    if (!slot) return false;
    const unsigned n = ++g_skips;
    if (n <= 4 || (n & (n - 1)) == 0)
        LOG("[j3d] packet %08X (in bucket %08X) entered again by %s before its buffer was reset: left out, it is "
            "drawn once (%u so far)",
            packet, slot, entry, n);
    return true;
}
}  // namespace

// (VR fork, step 2) the packets entered during Link's own draw this frame (game thread only): his, drawn bracketed;
// everything else in his lists is drawn outside the bracket (a headset run had the boat in the hands image: its draw
// enters his lists too, as Zelda's and a few others do)
namespace {
bool g_inLinkDraw = false;
std::unordered_set<uint32_t> g_linkPackets;
void note_entry(uint32_t packet) {
    if (g_inLinkDraw && packet) g_linkPackets.insert(packet);
}
}  // namespace
void j3d_draw_guard::link_draw(bool inside) { g_inLinkDraw = inside; }

extern "C" void hook_027F0C14(Cpu* c) {
    if (entered_already(c->r[4], "entryZSort")) { c->r[3] = 1; return; }
    note_entry(c->r[4]);
    f_027F0C14_orig(c);
}
extern "C" void hook_027F0DA8(Cpu* c) {
    if (entered_already(c->r[4], "entryNonSort")) { c->r[3] = 1; return; }
    note_entry(c->r[4]);
    f_027F0DA8_orig(c);
}
extern "C" void hook_027F0E04(Cpu* c) {
    if (entered_already(c->r[4], "entryImm")) { c->r[3] = 1; return; }
    note_entry(c->r[4]);
    f_027F0E04_orig(c);
}

// (VR fork, docs/VR_HANDS_LAYER_PLAN.md step 1) Link's draw lists: the buffers dComIfGd_setListP1 makes current for his
// draw (g_dComIfG_gameInfo + 0x5D58 opaque, + 0x5D60 the other; decomp d_a_player_main_01.cpp lk_setList); 1, 2, or 0
static uint32_t link_list(uint32_t buffer) {
#ifdef WWHD_VR
    // (only while the hands layer is being tried or checked: WWHD_VR_HANDS_LAYER=1 or WWHD_VR_HANDS_SKIP=1; normal play
    // draws his lists untouched)
    // (step 5: while the renderer has the layer on, vr.handsLayer at 30/45 fps; or a check run, WWHD_VR_HANDS_SKIP=1)
    static const bool skip = [] {
        const char* b = getenv("WWHD_VR_HANDS_SKIP");
        return b && *b == '1';
    }();
    // (first person only: in third person his whole body is the world's, a headset run had it in the hands layer)
    // (the steady hands test's phase 2, vr::hands_ab_mode: his lists split and bracketed with the layer off)
    if (!vr::game::hands_layer_view(skip || vr::hands_ab_mode() == 2)) return 0;
    const uint32_t info = GD(0x1046F0B0);
    if (!buffer) return 0;
    if (buffer == ld32(info + 0x5D58)) return 1;
    if (buffer == ld32(info + 0x5D60)) return 2;
    // (and his P0 list, dComIfGd_setListP0: shapes his draw enters there (l_onCupOffAupPacket2 and his masking shapes):
    // left in the eye's picture they drew a black copy of his hands and what he holds under the hands layer, its rim a
    // black outline round the sword, a headset run's pictures)
    if (buffer == ld32(info + 0x5D54)) return 3;
#else
    (void)buffer;
#endif
    return 0;
}
// one of Link's lists drawn (2026-11-14, fix attempt 4, the Tower of the Gods' black figurehead and rope with rainbow hands at the same
// moments, at 30/45 fps): walked once, in the game's own order, as J3DDrawBuffer::drawHead / drawTail walk it (buckets
// first to last / last to first, each bucket's chain, the table re-read after each bucket, the last draw's result
// returned), each run of the packets entered during his own draw bracketed by OP_VR_HANDS, the others not. Before,
// the buckets' chains were split and the list walked twice (the others, then his): the boat's 3 parts are in his
// opaque list every frame on the boat (a headset log), and the black came only with that split (a headset test with
// phases: 4 of 6 pictures with the list split and the layer off, never with no split); the two changes to how the
// split-off parts were drawn left it black. No chain is rewritten and nothing is drawn out of the game's order now
constexpr uint32_t kPacketVtable = 0x0C, kVtableDraw = 0x2C;  // (J3DPacket::__vtbl; its draw slot)
static void draw_link_list(Cpu* c, void (*orig)(Cpu*), uint32_t which, bool tail) {
    const uint32_t buffer = c->r[3], context = c->r[4];
    if (!guest_word(buffer) || !guest_word(buffer + kBuckets)) { orig(c); return; }
    uint32_t count = ld32(buffer + kBucketCount), buckets = ld32(buffer + kBuckets);
    if (!count || count > kMaxBuckets || !guest_word(buckets) || !guest_word(buckets + (count - 1) * 4)) { orig(c); return; }
    bool any = false;  // (none of his: the game's own walk, untouched)
    for (uint32_t i = 0; i < count && !any; ++i)
        for (uint32_t p = ld32(buckets + i * 4), n = 0; p && guest_word(p + kNext) && n < 100000; p = ld32(p + kNext), ++n)
            if (g_linkPackets.count(p)) { any = true; break; }
    if (!any) { orig(c); return; }
    uint32_t result = buffer, runs = 0, packets = 0, others = 0;
    bool inside = false;
    auto walk_bucket = [&](uint32_t slot) {
        for (uint32_t p = ld32(slot), n = 0; p && guest_word(p + kPacketVtable) && n < 100000; ++n) {
            const bool mine = g_linkPackets.count(p) != 0;
            if (mine != inside) {
                gx2::emit(gx2::OP_VR_HANDS, {mine ? which : 0u});
                inside = mine;
                runs += mine;
            }
            ++(mine ? packets : others);
            const uint32_t vt = ld32(p + kPacketVtable);
            const uint32_t fn = guest_word(vt + kVtableDraw) ? ld32(vt + kVtableDraw) : 0;
            if (fn) result = guest_call(c, fn, {p, context});
            p = ld32(p + kNext);  // (as the game: the link read after the draw)
        }
    };
    if (!tail) {
        for (uint32_t i = 0; i < count; ++i) {
            walk_bucket(buckets + i * 4);
            count = ld32(buffer + kBucketCount), buckets = ld32(buffer + kBuckets);  // (re-read, as the game does)
            if (count > kMaxBuckets) break;
        }
    } else {
        for (uint32_t i = count; i > 0; --i) {
            walk_bucket(buckets + (i - 1) * 4);
            buckets = ld32(buffer + kBuckets);
        }
    }
    if (inside) gx2::emit(gx2::OP_VR_HANDS, {0u});
    c->r[3] = result;
    // (proof it ran, per 10 s: his lists walked once, his runs in them, his packets and the others)
    static uint64_t lists = 0, allRuns = 0, mineN = 0, othersN = 0;
    static auto since = std::chrono::steady_clock::now();
    ++lists, allRuns += runs, mineN += packets, othersN += others;
    const auto now = std::chrono::steady_clock::now();
    if (now - since >= std::chrono::seconds(10)) {
        since = now;
        LOG("[vr hands fix] (10 s) Link's lists walked once in the game's order: %llu lists, %llu runs of his packets "
            "bracketed, %llu of his packets, %llu others (the boat's) drawn in place between them",
            (unsigned long long)lists, (unsigned long long)allRuns, (unsigned long long)mineN, (unsigned long long)othersN);
        lists = allRuns = mineN = othersN = 0;
    }
}
// (VR fork, 2026-11-14) the sky's draw lists (dComIfGd_setListSky: play +0x5D4C, +0x5D50; the sky dome daVrbox, its back
// clouds daVrbox2) drawn bracketed by OP_VR_SKY: at infinity in the headset (stereo::sky_draw). Not in desktop play
static bool sky_list(uint32_t buffer) {
#ifdef WWHD_VR
    if (!buffer || !stereo::requestedXR.load(std::memory_order_relaxed)) return false;
    static const bool on = [] {
        const char* e = getenv("WWHD_VR_SKY_INFINITY");
        const bool v = !(e && *e == '0');
        LOG("[vr] the sky's draw lists: %s", v ? "drawn at infinity (no eye offset; WWHD_VR_SKY_INFINITY=0: as the world)"
                                               : "drawn as the world (WWHD_VR_SKY_INFINITY=0)");
        return v;
    }();
    if (!on) return false;
    const uint32_t info = GD(0x1046F0B0);
    return buffer == ld32(info + 0x5D4C) || buffer == ld32(info + 0x5D50);
#else
    (void)buffer;
    return false;
#endif
}
static void draw_sky_list(Cpu* c, void (*orig)(Cpu*)) {
    gx2::emit(gx2::OP_VR_SKY, {1u});
    orig(c);
    gx2::emit(gx2::OP_VR_SKY, {0u});
}
extern "C" void hook_027F10CC(Cpu* c) {
    cut_loops(c->r[3], "drawHead");
    if (sky_list(c->r[3])) { draw_sky_list(c, f_027F10CC_orig); return; }
    if (const uint32_t l = link_list(c->r[3])) { draw_link_list(c, f_027F10CC_orig, l, false); return; }
    f_027F10CC_orig(c);
}
extern "C" void hook_027F1174(Cpu* c) {
    cut_loops(c->r[3], "drawTail");
    if (sky_list(c->r[3])) { draw_sky_list(c, f_027F1174_orig); return; }
    if (const uint32_t l = link_list(c->r[3])) { draw_link_list(c, f_027F1174_orig, l, true); return; }
    f_027F1174_orig(c);
}
extern "C" void hook_027F093C(Cpu* c) {
    cut_loops(c->r[3], "frameInit");
    if (link_list(c->r[3]) == 1) g_linkPackets.clear();  // (a new frame's entries: Link's opaque list reset)
    f_027F093C_orig(c);
}

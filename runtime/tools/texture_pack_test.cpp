// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Texture pack rules: index parsing, format table, DDS check, LOD shift, bind rule (gfx/texture_pack_core.h).
#include "gfx/texture_pack.h"
#include "gfx/texture_pack_core.h"

#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace gfx::texture_pack;

void log_msg(const char*, ...) {}   // the runtime logger; the registry only logs

static void put32(std::vector<uint8_t>& f, size_t o, uint32_t v) {
    for (int i = 0; i < 4; i++) f[o + i] = uint8_t(v >> (8 * i));
}
// DX10 DDS with `mips` levels of block size `bpb` (BC formats), zero-filled
static std::vector<uint8_t> make_dds(uint32_t w, uint32_t h, uint32_t mips, uint32_t dxgi, uint32_t bpb) {
    std::vector<uint8_t> f(148);
    put32(f, 0, 0x20534444u); put32(f, 4, 124); put32(f, 12, h); put32(f, 16, w); put32(f, 28, mips);
    put32(f, 76, 32); put32(f, 84, 0x30315844u);
    put32(f, 128, dxgi); put32(f, 132, 3); put32(f, 140, 1);
    size_t total = 0;
    for (uint32_t i = 0; i < mips; i++) total += size_t((std::max(1u, w >> i) + 3) / 4) * ((std::max(1u, h >> i) + 3) / 4) * bpb;
    f.resize(148 + total);
    return f;
}

static void write_file(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static void write_text(const std::filesystem::path& p, const std::string& t) { write_file(p, std::vector<uint8_t>(t.begin(), t.end())); }
// a 16x16 BC1 texture (format 49) with hash `h`, replaced 4x by a 64x64 BC7 DDS whose level bytes are `fill`
static std::string entry_json(uint64_t h, const char* file, uint32_t scale = 4) {
    char b[256];
    std::snprintf(b, sizeof b, R"({"format":49,"width":16,"height":16,"hash":"%016llx","scale":%u,"file":"%s"})", (unsigned long long)h, scale, file);
    return b;
}
static std::vector<uint8_t> dds_filled(uint8_t fill) {
    auto f = make_dds(64, 64, 1, 98, 16);
    std::fill(f.begin() + 148, f.end(), fill);
    return f;
}
static bool wait_for(auto&& pred) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

static void registry_tests() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::current_path() / "texture_pack_test_tmp";
    fs::remove_all(tmp);
    fs::path r1 = tmp / "a" / "textures", r2 = tmp / "b" / "textures";
    const Key k1{49, 16, 16, 1}, k2{49, 16, 16, 2}, k3{49, 16, 16, 3};

    // no roots: inactive, no thread, nothing to find; shutdown twice and without a thread is fine
    assert(!load({}, 4096) && !active() && stats().entries == 0 && lookup(k1) == nullptr);
    request(k1);
    assert(stats().requested == 0 && take_ready(1 << 20).empty());
    shutdown(); shutdown();

    // two roots with an overlapping key: the first root's file wins; one summary line per root, never per warning
    write_text(r1 / "index.json", R"({"format_version":1,"entries":[)" + entry_json(1, "a1.dds") + "," + entry_json(2, "a2.dds") + "]}");
    write_text(r2 / "index.json", R"({"format_version":1,"entries":[)" + entry_json(1, "b1.dds") + "," + entry_json(3, "b3.dds") + "," +
               R"({"format":49,"width":16,"height":16,"hash":"0000000000000009","scale":3,"file":"x.dds"})" + "]}");
    write_file(r1 / "a1.dds", dds_filled(0xA1));
    write_file(r1 / "a2.dds", dds_filled(0xA2));
    write_file(r2 / "b3.dds", dds_filled(0xB3));
    std::vector<std::pair<fs::path, std::string>> said;
    set_status_sink([&](const fs::path& r, const std::string& t) { said.emplace_back(r, t); });
    assert(load({r1, r2, tmp / "missing" / "textures"}, 4096) && active());
    assert(stats().entries == 3);
    assert(lookup(k1) && lookup(k1)->file == r1 / "a1.dds" && lookup(k3)->file == r2 / "b3.dds" && !lookup(Key{49, 16, 16, 7}));
    assert(said.size() == 3 && said[0].first == r1 && said[0].second == "texture pack: 2 entries, 0 duplicate keys, 0 skipped");
    assert(said[1].second == "texture pack: 1 entries, 1 duplicate keys, 1 skipped");
    assert(said[2].second.rfind("texture pack: index.json failed: ", 0) == 0);
    set_status_sink(nullptr);

    // has_shape: any entry of that format and size
    assert(has_shape(49, 16, 16) && !has_shape(49, 16, 17) && !has_shape(50, 16, 16) && !has_shape(49, 70000, 16));

    // request twice before completion -> one load; take_ready returns it with levels that match the file
    request(k1); request(k1);
    assert(stats().requested == 1 && stats().pending == 1);   // pending until taken
    std::vector<Loaded> got;
    assert(wait_for([&] { for (auto& l : take_ready(1 << 30)) got.push_back(std::move(l)); return !got.empty(); }));
    assert(got.size() == 1 && got[0].key == k1 && got[0].width == 64 && got[0].height == 64 && got[0].dxgi == 98 && got[0].scale == 4);
    assert(got[0].file == dds_filled(0xA1) && got[0].levels.size() == 1 && got[0].levels[0].data() == got[0].file.data() + 148 &&
           got[0].levels[0].size() == 4096 && got[0].levels[0][0] == 0xA1);
    assert(stats().loaded == 1 && stats().readyBytes == 0 && stats().bad == 0 && stats().pending == 0);
    request(k1);                                   // taken, so it may be asked for again
    assert(stats().requested == 2);
    assert(wait_for([&] { return stats().loaded == 2; }));
    assert(take_ready(1 << 30).size() == 1);

    // the ready notification: once per finished load, none after it is cleared
    {
        std::atomic<int> notified{0};
        set_ready_notify([&] { ++notified; });
        request(k2);
        assert(wait_for([&] { return notified.load() == 1; }));
        assert(take_ready(1 << 30).size() == 1 && notified.load() == 1);
        set_ready_notify(nullptr);
        request(k2);
        assert(wait_for([&] { return stats().loaded == 4; }));
        assert(take_ready(1 << 30).size() == 1 && notified.load() == 1);
    }

    // take_ready(maxBytes) returns at least one, then the rest on the next call
    request(k1); request(k2);
    assert(wait_for([&] { return stats().loaded == 6; }));
    assert(take_ready(1).size() == 1 && take_ready(1 << 30).size() == 1 && take_ready(1 << 30).empty());

    // truncated DDS (k2 truncated on disk), missing file (k3 -> b3.dds removed): nothing ready, bad once, no second load
    auto cut = dds_filled(0xA2); cut.resize(1000);
    write_file(r1 / "a2.dds", cut);
    fs::remove(r2 / "b3.dds");
    request(k2); request(k3);
    assert(wait_for([&] { return stats().bad == 2; }));
    assert(take_ready(1 << 30).empty() && stats().requested == 8);
    request(k2); request(k3);
    assert(stats().requested == 8 && stats().bad == 2);
    mark_bad(k2, "again"); mark_bad(k1, "renderer says so"); mark_bad(k1, "twice");
    assert(stats().bad == 3);
    request(k1);                                   // bad by the renderer: ignored
    assert(stats().requested == 8);

    // marked bad at any point (queued, loading, or finished and not taken): never delivered
    write_file(r1 / "a1.dds", dds_filled(0xA1));
    write_file(r1 / "a2.dds", dds_filled(0xA2));
    assert(load({r1}, 4096));
    request(k1); mark_bad(k1, "test");              // whichever stage the loader is at
    request(k2);                                    // a good key after it still loads
    std::vector<Loaded> got2;
    assert(wait_for([&] { for (auto& l : take_ready(1 << 30)) got2.push_back(std::move(l)); return stats().loaded >= 1 && !got2.empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (auto& l : take_ready(1 << 30)) got2.push_back(std::move(l));
    assert(got2.size() == 1 && got2[0].key == k2);   // k1 was marked bad and is never among them
    request(k2);                                    // finished and not taken, then marked bad
    assert(wait_for([&] { return stats().readyBytes > 0; }));
    mark_bad(k2, "after the load");
    assert(stats().readyBytes == 0 && take_ready(1 << 30).empty() && stats().bad == 2);
    // the loader waits while finished data fills the bound, and mark_bad of a finished load frees room and wakes it
    write_file(r1 / "a2.dds", dds_filled(0xA2));
    assert(load({r1}, 4096));
    set_max_ready_bytes_for_test(1);
    request(k1);
    assert(wait_for([&] { return stats().loaded == 1; }));
    request(k2);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(stats().loaded == 1);                    // k2 waits: the bound is full
    mark_bad(k1, "frees the room");
    assert(wait_for([&] { return stats().loaded == 2; }));
    auto got3 = take_ready(1 << 30);
    assert(got3.size() == 1 && got3[0].key == k2);
    // a file far larger than the entry can need is refused before it is read
    assert(load({r1}, 4096));
    auto huge = dds_filled(0); huge.resize(1 << 20);
    write_file(r1 / "a2.dds", huge);
    request(k2);
    assert(wait_for([&] { return stats().bad == 1; }));
    assert(take_ready(1 << 30).empty());
    // the size bound is tight: RGBA8 (4 B/texel), non-square full chains load; one byte over the true maximum is refused
    {
        auto rgba8 = [](uint32_t w, uint32_t h, uint32_t mips) {
            auto f = make_dds(w, h, mips, 28, 0);
            size_t total = 0;
            for (uint32_t i = 0; i < mips; i++) total += size_t(std::max(1u, w >> i)) * std::max(1u, h >> i) * 4;
            f.resize(148 + total);
            return f;
        };
        const Key kr{0x1A, 16, 16, 0x10}, kn{49, 16, 4, 0x11}, ko{49, 16, 16, 0x12}, kp{49, 16, 16, 0x13};
        // true maximum for a 16x16 key at x4: header + every level of the 64x64 chain at max(BC7, RGBA8) bytes
        size_t limit = 148;
        for (uint32_t i = 0; i < 7; i++) {
            size_t lw = std::max(1u, 64u >> i), lh = lw;
            limit += std::max(((lw + 3) / 4) * ((lh + 3) / 4) * 16, lw * lh * 4);
        }
        write_text(r1 / "index.json", std::string(R"({"format_version":1,"entries":[)") +
            R"({"format":26,"width":16,"height":16,"hash":"0000000000000010","scale":2,"file":"r.dds"},)" +
            R"({"format":49,"width":16,"height":4,"hash":"0000000000000011","scale":4,"file":"n.dds"},)" +
            R"({"format":49,"width":16,"height":16,"hash":"0000000000000012","scale":4,"file":"o.dds"},)" +
            R"({"format":49,"width":16,"height":16,"hash":"0000000000000013","scale":4,"file":"p.dds"}]})");
        write_file(r1 / "r.dds", rgba8(32, 32, 6));
        write_file(r1 / "n.dds", make_dds(64, 16, 7, 98, 16));
        auto atMax = make_dds(64, 64, 7, 98, 16); atMax.resize(limit);
        auto over = atMax; over.push_back(0);
        write_file(r1 / "o.dds", atMax);
        write_file(r1 / "p.dds", over);
        assert(load({r1}, 4096));
        request(kr); request(kn); request(ko); request(kp);
        assert(wait_for([&] { return stats().loaded + stats().bad == 4; }));
        auto ok = take_ready(1 << 30);
        assert(ok.size() == 3 && stats().bad == 1);
        for (auto& l : ok) assert(l.key != kp && l.levels.size() == (l.key == kr ? 6u : 7u));
    }

    // an unknown key is ignored
    uint64_t before = stats().requested;
    request(Key{49, 16, 16, 99});
    assert(stats().requested == before);

    // prefetch: an archive the game opens queues every key it holds, once, behind the renderer's requests
    {
        const Key p1{49, 16, 16, 0x21}, p2{49, 16, 16, 0x22}, p3{49, 16, 16, 0x23}, p4{49, 16, 16, 0x24};
        auto with_archives = [](uint64_t h, const char* file, const char* archives) {
            std::string e = entry_json(h, file);
            e.pop_back();
            return e + R"(,"archives":)" + archives + "}";
        };
        write_text(r1 / "index.json", std::string(R"({"format_version":1,"entries":[)") +
            with_archives(0x21, "p1.dds", R"(["Common/Stage/sea_Room13.szs"])") + "," +
            with_archives(0x22, "p2.dds", R"(["Common/Stage/sea_Room13.szs","Common/Pack/szs_permanent1.pack"])") + "," +
            with_archives(0x23, "p3.dds", R"(["Common/Pack/szs_permanent1.pack"])") + "," +
            entry_json(0x24, "p4.dds") + "]}");
        for (const char* f : {"p1.dds", "p2.dds", "p3.dds", "p4.dds"}) write_file(r1 / f, dds_filled(0x5A));
        assert(load({r1}, 4096) && stats().entries == 4);
        auto take_all = [](size_t n) {
            std::vector<Loaded> got;
            assert(wait_for([&] { for (auto& l : take_ready(1 << 30)) got.push_back(std::move(l)); return got.size() >= n; }));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            for (auto& l : take_ready(1 << 30)) got.push_back(std::move(l));
            assert(got.size() == n);
            return got;
        };
        auto take_one = [] {   // (while the ready bound holds one load at a time)
            std::vector<Loaded> got;
            assert(wait_for([&] { got = take_ready(1); return !got.empty(); }));
            assert(got.size() == 1);
            return std::move(got[0]);
        };
        auto has = [](const std::vector<Loaded>& got, const Key& k) {
            return std::any_of(got.begin(), got.end(), [&](const Loaded& l) { return l.key == k; });
        };
        // off until the renderer turns it on (it does so only where it keeps unattached replacements)
        assert(!prefetch_on());
        prefetch("/vol/content/Common/Stage/sea_Room13.szs");
        assert(stats().prefetched == 0 && stats().pending == 0);
        set_prefetch(true);
        assert(prefetch_on());
        assert(has_archive("/vol/content/common/STAGE/sea_room13.szs") && has_archive("Common\\Pack\\szs_permanent1.pack"));
        assert(!has_archive("Common/Stage/sea_Room14.szs") && !has_archive("/vol/save/Common/Stage/sea_Room13.szs"));
        // an archive opened twice (both path forms): its two keys once each, not counted as requests
        prefetch("/vol/content/Common/Stage/sea_Room13.szs");
        prefetch("Common/Stage/SEA_ROOM13.szs");
        auto got = take_all(2);
        assert(has(got, p1) && has(got, p2) && stats().prefetched == 2 && stats().requested == 0);
        for (auto& l : got) assert(l.prefetch != 0 && l.file == dds_filled(0x5A));
        // unknown archives, paths outside /vol/content: nothing
        prefetch("Common/Stage/sea_Room14.szs");
        prefetch("/vol/save/Common/Stage/sea_Room13.szs");
        prefetch("/vol/contentx/Common/Stage/sea_Room13.szs");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(stats().prefetched == 2 && stats().pending == 0);
        // replacements the renderer still holds are skipped; let go, they are prefetched again
        hold(p1); hold(p2); hold(p2);
        prefetch("Common/Pack/szs_permanent1.pack");
        got = take_all(1);
        assert(got[0].key == p3 && stats().prefetched == 3);
        unhold(p2);                                   // (held twice: still held)
        prefetch("Common/Pack/szs_permanent1.pack");
        got = take_all(1);
        assert(got[0].key == p3 && stats().prefetched == 4);
        unhold(p2);
        prefetch("Common/Pack/szs_permanent1.pack");
        got = take_all(2);
        assert(has(got, p2) && has(got, p3) && stats().prefetched == 6);
        // a request is never skipped for a held key, and its load is no prefetch
        request(p1);
        got = take_all(1);
        assert(got[0].key == p1 && got[0].prefetch == 0 && stats().requested == 1);
        // a key already asked for is not queued again by a prefetch (dedup with request)
        unhold(p1);
        set_max_ready_bytes_for_test(1);              // the loader stops after one finished load
        request(p4);
        assert(wait_for([&] { return stats().loaded == 8; }));
        prefetch("/vol/content/Common/Stage/sea_Room13.szs");   // p1, p2 queued behind the blocked loader
        assert(wait_for([&] { return stats().prefetched == 8; }));
        request(p2);                                  // queued by the prefetch: moved in front of it
        assert(stats().requested == 3);
        request(p2);                                  // (once)
        assert(stats().requested == 3);
        Loaded one = take_one();                      // p4 out: room for the next
        assert(one.key == p4 && one.prefetch == 0);
        one = take_one();
        assert(one.key == p2 && one.prefetch != 0);   // the request first, still known as prefetched
        one = take_one();
        assert(one.key == p1 && one.prefetch != 0);
        set_max_ready_bytes_for_test(1 << 30);
        // bad keys are skipped
        mark_bad(p3, "test");
        prefetch("Common/Pack/szs_permanent1.pack");
        got = take_all(1);
        assert(got[0].key == p2 && stats().prefetched == 9);
        // paused (the renderer's budget hint): prefetched keys not loading yet are dropped, nothing new is queued, a
        // request still loads; resumed, the archive loads again
        {
            const uint64_t loaded = stats().loaded, prefetched = stats().prefetched;
            set_max_ready_bytes_for_test(1);
            request(p4);
            assert(wait_for([&] { return stats().loaded == loaded + 1; }));
            prefetch("Common/Stage/sea_Room13.szs");               // p1, p2 queued behind the blocked loader
            assert(wait_for([&] { return stats().prefetched == prefetched + 2; }));
            set_prefetch_paused(true);
            assert(prefetch_paused());
            assert(take_one().key == p4);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            assert(stats().loaded == loaded + 1 && stats().pending == 0 && take_ready(1 << 30).empty());
            prefetch("Common/Stage/sea_Room13.szs");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(stats().prefetched == prefetched + 2 && stats().pending == 0);
            request(p1);
            assert(take_one().key == p1);
            set_prefetch_paused(false);
            set_max_ready_bytes_for_test(1 << 30);
            prefetch("Common/Stage/sea_Room13.szs");
            auto again = take_all(2);
            assert(has(again, p1) && has(again, p2) && stats().prefetched == prefetched + 4);
        }
        // off again: nothing; shutdown turns it off
        set_prefetch(false);
        prefetch("Common/Stage/sea_Room13.szs");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!prefetch_on() && stats().prefetched == 13 && stats().pending == 0);
        set_prefetch(true);
        shutdown();
        assert(!prefetch_on());
        // parking: prefetched loads stay in the loader until a request, which gets one without reading it again
        {
            const uint64_t file = dds_filled(0x5A).size();
            assert(load({r1}, 4096));
            set_prefetch(true);
            set_prefetch_park(1 << 30);
            prefetch("Common/Stage/sea_Room13.szs");                 // p1, p2
            assert(wait_for([&] { return stats().parked == 2; }));
            assert(take_ready(1 << 30).empty() && stats().pending == 0 && stats().parkedBytes == 2 * file);
            const uint64_t loaded = stats().loaded;
            request(p1);
            got = take_all(1);
            assert(got[0].key == p1 && got[0].prefetch == 0 && got[0].file == dds_filled(0x5A));
            assert(stats().loaded == loaded && stats().parkHits == 1 && stats().parked == 1 && stats().requested == 1);
            prefetch("Common/Stage/sea_Room13.szs");                 // p2 parked: not read again (p1: renderer's now)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(stats().loaded == loaded + 1 && stats().parked == 2);   // (p1 taken, so prefetched again)
            // room for one: a later archive's load takes the room of an earlier one's
            set_prefetch_park(file);                                  // (the older of p2, p1 dropped at once)
            assert(stats().parked == 1 && stats().parkDropped == 1);
            // p2 read again takes p1's room (an earlier open's); p3 finds the room held by its own open: dropped unread
            prefetch("Common/Pack/szs_permanent1.pack");
            assert(wait_for([&] { return stats().parkDropped == 3; }));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(stats().loaded == loaded + 2 && stats().parked == 1 && stats().parkedBytes == file);
            const uint64_t hits = stats().parkHits;
            request(p2);
            got = take_all(1);
            assert(got[0].key == p2 && stats().parkHits == hits + 1 && stats().parked == 0 && stats().loaded == loaded + 2);
            // a parked load's key marked bad is gone from the parking
            prefetch("Common/Stage/sea_Room13.szs");
            assert(wait_for([&] { return stats().parked == 1; }));
            const uint64_t before = stats().parkedBytes;
            mark_bad(p1, "test");
            mark_bad(p2, "test");
            assert(stats().parked == 0 && stats().parkedBytes == before - file && take_ready(1 << 30).empty());
            // off: finished prefetches are delivered again; shutdown drops the parking and turns it off
            set_prefetch_park(0);
            prefetch("Common/Pack/szs_permanent1.pack");
            got = take_all(1);
            assert(got[0].key == p3 && got[0].prefetch != 0);
            set_prefetch_park(1 << 30);
            shutdown();
            assert(stats().parked == 0 && stats().parkedBytes == 0);
        }
        // an index without archives: never on (the file-open hook stays one test)
        write_text(r1 / "index.json", std::string(R"({"format_version":1,"entries":[)") + entry_json(0x24, "p4.dds") + "]}");
        assert(load({r1}, 4096));
        set_prefetch(true);
        assert(!prefetch_on() && !has_archive("Common/Stage/sea_Room13.szs"));
        // a large archive (several of the loader's expansion slices): every key once, opened again meanwhile too;
        // holds outside any load are no-ops
        std::string big = R"({"format_version":1,"entries":[)";
        for (uint64_t h = 0x100; h < 0x100 + 200; ++h) big += (h > 0x100 ? "," : "") + with_archives(h, "big.dds", R"(["Common/Pack/big.pack"])");
        write_text(r1 / "index.json", big + "]}");
        write_file(r1 / "big.dds", dds_filled(0x77));
        assert(load({r1}, 4096));
        set_prefetch(true);
        hold(Key{49, 16, 16, 0x100});
        unhold(Key{49, 16, 16, 0x9999}); unhold(Key{49, 16, 16, 0x101});   // (unknown, never held: nothing)
        for (int i = 0; i < 5; ++i) prefetch("Common/Pack/big.pack");
        assert(wait_for([&] { take_ready(1 << 30); return stats().loaded == 199; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(stats().prefetched == 199 && stats().loaded == 199);
        take_ready(1 << 30);
    }

    // reload replaces everything; load of a package with a broken index alone is inactive
    write_text(r2 / "index.json", "{ nope");
    assert(!load({r2}, 4096) && !active() && stats().entries == 0 && stats().bad == 0 && !has_shape(49, 16, 16));
    shutdown(); shutdown();
    fs::remove_all(tmp);
}

int main() {
    // index: two entries
    const char* good = R"({"format_version":1,"generator":"t","entries":[
        {"format":49,"width":256,"height":256,"hash":"9f2c0000000000ab","scale":4,"file":"9f/a.dds","name":"a"},
        {"format":1073,"width":64,"height":32,"hash":"0123456789ABCDEF","scale":2,"file":"01/b.dds","dxgi":99}]})";
    Index idx;
    std::string err;
    std::filesystem::path root = "pack/textures";
    assert(parse_index(good, root, idx, err));
    assert(idx.entries.size() == 2 && idx.warnings.empty());
    auto a = idx.entries.find(Key{49, 256, 256, 0x9f2c0000000000abull});
    assert(a != idx.entries.end() && a->second.scale == 4 && a->second.dxgi == 0 && !a->second.bad);
    assert(a->second.file == root / "9f/a.dds");
    auto b = idx.entries.find(Key{1073, 64, 32, 0x0123456789ABCDEFull});
    assert(b != idx.entries.end() && b->second.scale == 2 && b->second.dxgi == 99);

    // same key in a second index: first wins, one warning
    const char* dup = R"({"format_version":1,"entries":[
        {"format":49,"width":256,"height":256,"hash":"9f2c0000000000ab","scale":8,"file":"zz/other.dds"}]})";
    assert(parse_index(dup, "other", idx, err));
    assert(idx.entries.size() == 2 && idx.warnings.size() == 1);
    assert(idx.entries.at(Key{49, 256, 256, 0x9f2c0000000000abull}).scale == 4);

    // bad fields: scale 3, width 0, missing file, 15-digit hash, plus an escaping file; one good entry survives
    const char* badEntries = R"({"format_version":1,"entries":[
        {"format":49,"width":16,"height":16,"hash":"0000000000000001","scale":3,"file":"a.dds"},
        {"format":49,"width":0,"height":16,"hash":"0000000000000002","scale":4,"file":"a.dds"},
        {"format":49,"width":16,"height":16,"hash":"0000000000000003","scale":4},
        {"format":49,"width":16,"height":16,"hash":"000000000000004","scale":4,"file":"a.dds"},
        {"format":49,"width":16,"height":16,"hash":"0000000000000005","scale":4,"file":"../a.dds"},
        {"format":49,"width":16,"height":16,"hash":"0000000000000006","scale":4,"file":"ok.dds"}]})";
    Index idx2;
    assert(parse_index(badEntries, root, idx2, err));
    assert(idx2.entries.size() == 1 && idx2.warnings.size() == 5);
    assert(idx2.entries.contains(Key{49, 16, 16, 6}));

    // an index over the 1 MiB manifest limit (a package of thousands of entries) is read whole
    {
        std::string big = R"({"format_version":1,"entries":[)";
        for (uint64_t h = 1; big.size() <= 2 * 1024 * 1024; h++) big += (h > 1 ? "," : "") + entry_json(h, "a.dds");
        big += "]}";
        Index bigIdx;
        assert(parse_index(big, root, bigIdx, err) && bigIdx.entries.size() > 5000);
    }

    // whole-document failures
    Index idx3;
    assert(!parse_index(R"({"format_version":2,"entries":[]})", root, idx3, err) && !err.empty());
    assert(!parse_index(R"({"entries":[]})", root, idx3, err));
    assert(!parse_index("[1]", root, idx3, err));
    assert(!parse_index("{not json", root, idx3, err));

    // game paths: /vol/content/ stripped, lowercase, forward slashes; nothing outside /vol/content
    {
        char buf[48];
        auto norm = [&](std::string_view p) { return std::string(buf, normalize_game_path(p, buf)); };
        assert(norm("/vol/content/Common/Stage/sea_Room13.szs") == "common/stage/sea_room13.szs");
        assert(norm("Common/Stage/sea_Room13.szs") == "common/stage/sea_room13.szs");          // relative to the cwd
        assert(norm("Common\\Pack\\szs_permanent1.pack") == "common/pack/szs_permanent1.pack");
        assert(norm("/VOL/Content\\Common/X.szs") == "common/x.szs");
        assert(norm("/vol/content//Common//X.szs") == "common/x.szs" && norm("./Common/X.szs") == "common/x.szs");
        assert(norm("/vol/content").empty() && norm("/vol/content/").empty() && norm("").empty());
        assert(norm("/vol/save/common/x.bin").empty() && norm("/vol/code/cking.rpx").empty() && norm("/vol/contentx/a").empty());
        assert(norm(std::string(48, 'a')).size() == 48 && norm(std::string(49, 'a')).empty());  // (longer than the buffer: none)
    }

    // archives: the optional list of the game files holding each texture, by normalized path
    {
        const char* withArchives = R"({"format_version":1,"entries":[
            {"format":49,"width":16,"height":16,"hash":"0000000000000001","scale":4,"file":"a.dds",
             "archives":["Common/Stage/sea_Room13.szs","Common/Pack/szs_permanent1.pack"]},
            {"format":49,"width":16,"height":16,"hash":"0000000000000002","scale":4,"file":"b.dds",
             "archives":["common/stage/SEA_ROOM13.szs","Common/Stage/sea_Room13.szs"]},
            {"format":49,"width":16,"height":16,"hash":"0000000000000003","scale":4,"file":"c.dds",
             "archives":["../x.szs","/vol/content/abs.szs","C:/x.szs",5,"","Common/Object/Ls.szs"]},
            {"format":49,"width":16,"height":16,"hash":"0000000000000004","scale":4,"file":"d.dds","archives":"Common/x.szs"},
            {"format":49,"width":16,"height":16,"hash":"0000000000000005","scale":4,"file":"e.dds"},
            {"format":49,"width":16,"height":16,"hash":"0000000000000001","scale":4,"file":"dup.dds","archives":["Common/Dup.szs"]}]})";
        Index ia;
        assert(parse_index(withArchives, root, ia, err));
        assert(ia.entries.size() == 5);                  // a bad archive is skipped, never its entry
        const Key a1{49, 16, 16, 1}, a2{49, 16, 16, 2}, a3{49, 16, 16, 3};
        assert(ia.archives.size() == 3);
        auto keys = [&](std::string_view p) { auto it = ia.archives.find(p); return it == ia.archives.end() ? std::vector<Key>{} : it->second; };
        assert((keys("common/stage/sea_room13.szs") == std::vector<Key>{a1, a2}));   // once each, entry order
        assert((keys("common/pack/szs_permanent1.pack") == std::vector<Key>{a1}));
        assert((keys("common/object/ls.szs") == std::vector<Key>{a3}));
        assert(keys("common/dup.szs").empty());          // (a skipped duplicate key brings no archive)
        // warnings: five bad archive names of entry 2, a non-array of entry 3, the duplicate key
        assert(ia.warnings.size() == 7);
    }

    // format table
    assert(allowed_dxgi(0x31, 98) == 98);
    assert(allowed_dxgi(0x431, 99) == 99);
    assert(allowed_dxgi(0x431, 98) == 0);
    assert(allowed_dxgi(0x235, 84) == 84);
    assert(allowed_dxgi(0x35, 84) == 0);
    assert(allowed_dxgi(0x1A, 28) == 28);
    assert(allowed_dxgi(0x1A, 98) == 98);
    assert(allowed_dxgi(0x41A, 29) == 29);
    assert(allowed_dxgi(0x19, 28) == 0);
    assert(allowed_dxgi(0x234, 81) == 81 && allowed_dxgi(0x34, 80) == 80 && allowed_dxgi(0x34, 81) == 0);

    // DDS check: BC7 64x64, 7 levels, key 16x16 scale 4
    Key key{0x31, 16, 16, 1};
    Entry e{"x.dds", 4, 98};
    auto dds = make_dds(64, 64, 7, 98, 16);
    DdsLevels out{};
    assert(check_dds(dds, key, e, 4096, out, err));
    assert(out.levels.size() == 7 && out.levels[0].size() == 4096 && out.width == 64 && out.dxgi == 98);
    assert(out.levels[1].size() == 1024 && out.levels[6].size() == 16);
    assert(out.levels[0].data() == dds.data() + 148);
    // each failure on its own
    auto no = [&](std::span<const uint8_t> f, const Key& k, const Entry& en, uint32_t maxDim) {
        DdsLevels o{};
        std::string er;
        return !check_dds(f, k, en, maxDim, o, er) && !er.empty();
    };
    assert(no(dds, Key{0x31, 16, 15, 1}, e, 4096));                  // height 60 vs 64: size mismatch
    assert(no(make_dds(80, 80, 7, 98, 16), key, Entry{"x", 5, 98}, 4096)); // scale 5
    assert(no(dds, key, Entry{"x", 4, 71}, 4096));                   // dxgi 71
    assert(no(dds, key, Entry{"x", 4, 99}, 4096));                   // e.dxgi differs from the file
    assert(no(dds, key, e, 32));                                     // above maxDim
    assert(no(std::span<const uint8_t>(dds).first(dds.size() - 1), key, e, 4096)); // truncated by one byte
    assert(no(make_dds(64, 64, 16, 98, 16), key, e, 4096));          // 16 levels
    assert(no(make_dds(64, 64, 0, 98, 16), key, e, 4096));           // 0 levels
    assert(no(std::span<const uint8_t>(dds).first(100), key, e, 4096));
    auto badMagic = dds; badMagic[0] = 'X';
    assert(no(badMagic, key, e, 4096));
    auto cube = dds; put32(cube, 136, 4);
    assert(no(cube, key, e, 4096));
    // dxgi 0 in the entry: the file's format decides
    assert(check_dds(dds, key, Entry{"x", 4, 0}, 4096, out, err) && out.dxgi == 98);
    assert(no(make_dds(64, 64, 7, 80, 8), key, Entry{"x", 4, 0}, 4096)); // BC4 file for a BC1 key
    assert(no(make_dds(64, 64, 7, 98, 16), key, Entry{"x", 4, 99}, 4096)); // entry 99, file 98
    // mip count bounded by the size: 64x64 has at most 7 levels
    assert(no(make_dds(64, 64, 8, 98, 16), key, e, 4096));
    assert(check_dds(make_dds(64, 64, 7, 98, 16), key, e, 4096, out, err));
    // BC4 and RGBA8 level sizes
    auto bc4 = make_dds(32, 32, 1, 80, 8);
    assert(check_dds(bc4, Key{0x34, 8, 8, 1}, Entry{"x", 4, 80}, 4096, out, err) && out.levels[0].size() == 512);
    std::vector<uint8_t> rgba(148 + 32 * 32 * 4);
    std::memcpy(rgba.data(), bc4.data(), 148);
    put32(rgba, 128, 28);
    assert(check_dds(rgba, Key{0x1A, 8, 8, 1}, Entry{"x", 4, 28}, 4096, out, err) && out.levels[0].size() == 4096);

    // LOD shift
    assert((replacement_lods(0, 10, true, 2) == LodRange{0, 12}));
    assert((replacement_lods(1, 10, true, 2) == LodRange{3, 12}));
    assert((replacement_lods(0, 0, false, 5) == LodRange{0, 0}));
    assert((replacement_lods(2, 7, false, 5) == LodRange{2, 7}));
    // levels a replacement adds: log2(scale) for the valid scales, 0 otherwise
    assert(lod_offset(2) == 1 && lod_offset(4) == 2 && lod_offset(8) == 3 && lod_offset(16) == 4 && lod_offset(32) == 5);
    assert(lod_offset(0) == 0 && lod_offset(1) == 0 && lod_offset(3) == 0 && lod_offset(6) == 0 && lod_offset(64) == 0);

    // default VRAM budget: min(budget / 2, budget - usage), at least 1 GiB
    constexpr uint64_t GiB = 1ull << 30;
    assert(default_budget(8 * GiB, 1 * GiB) == 4 * GiB);
    assert(default_budget(8 * GiB, 6 * GiB) == 2 * GiB);
    assert(default_budget(2 * GiB, 1932735283ull) == 1 * GiB); // 2 GiB / 1.9 GiB
    assert(default_budget(1 * GiB, 2 * GiB) == 1 * GiB);       // usage above budget: no underflow

    // creation pacing: 16 MiB and 1 ms of the render thread a frame, whichever first; the first one always goes
    constexpr uint64_t MiB = 1ull << 20, ms = 1000000;
    static_assert(kCreateBytesPerFrame == 16 * MiB && kCreateNsPerFrame == 1 * ms);
    assert(may_start_another(0, 0, 4 * MiB, 0));
    assert(may_start_another(0, 0, 64 * MiB, 0));               // a 4096^2+ one alone: never starved
    assert(may_start_another(0, 5 * ms, 64 * MiB, 0));          // (nothing made yet: time spent elsewhere does not count)
    assert(may_start_another(8 * MiB, 0, 8 * MiB, 1));          // exactly 16 MiB
    assert(!may_start_another(8 * MiB, 0, 8 * MiB + 1, 1));     // over the bytes
    assert(!may_start_another(64 * MiB, 0, 1, 1));              // the large one made: nothing after it
    assert(may_start_another(1 * MiB, ms - 1, 1 * MiB, 3));
    assert(!may_start_another(1 * MiB, ms, 1 * MiB, 3));        // the time is up

    // the budget caps: a load is made only when it fits (after releasing down to release_target), else refused
    assert(admits(0, 1 * MiB, 0));                              // no budget yet: no cap
    assert(admits(0, 256 * MiB, 256 * MiB));                    // exactly the budget
    assert(admits(200 * MiB, 56 * MiB, 256 * MiB));
    assert(!admits(200 * MiB, 56 * MiB + 1, 256 * MiB));        // one byte over
    assert(!admits(0, 256 * MiB + 1, 256 * MiB));               // alone over the budget
    assert(!admits(300 * MiB, 1, 256 * MiB));                   // already over (no underflow)
    assert(!admits(0, 64, 1));                                  // (the smoke test's tiny budget)
    assert(release_target(256 * MiB, 56 * MiB) == 200 * MiB);
    assert(release_target(256 * MiB, 256 * MiB) == 0 && release_target(1, 64) == 0);  // nothing would help
    assert(admits(release_target(256 * MiB, 56 * MiB), 56 * MiB, 256 * MiB));        // released to the target, it fits
    // a refused key: again when it fits now, or 300 frames on, never at once
    assert(!may_retry(1000, 1000, 300, false));
    assert(!may_retry(1000, 1299, 300, false));
    assert(may_retry(1000, 1300, 300, false));
    assert(may_retry(1000, 1001, 300, true));

    // whole numbers: the range is checked before the conversion (garbage, NaN, infinities and huge values are no UB)
    {
        using mods::json::Value;
        auto num = [](double d) { Value v; v.type = Value::Number; v.number = d; return v; };
        uint32_t out = 7;
        assert(detail::whole(num(16384), 1, 16384, out) && out == 16384);
        assert(!detail::whole(num(16384.5), 1, 16385, out));
        assert(!detail::whole(num(-1), 0, 10, out) && !detail::whole(num(1e300), 0, 10, out));
        assert(!detail::whole(num(-1e300), 0, 10, out) && !detail::whole(num(1e19), 0, 1e9, out));
        assert(!detail::whole(num(std::numeric_limits<double>::quiet_NaN()), 0, 10, out));
        assert(!detail::whole(num(std::numeric_limits<double>::infinity()), 0, 10, out));
        assert(!detail::whole(num(-std::numeric_limits<double>::infinity()), 0, 10, out));
        assert(out == 16384);
        Index huge;
        assert(parse_index(R"({"format_version":1,"entries":[
            {"format":49,"width":1e300,"height":16,"hash":"0000000000000001","scale":4,"file":"a.dds"},
            {"format":-1e300,"width":16,"height":16,"hash":"0000000000000002","scale":4,"file":"a.dds"}]})", root, huge, err));
        assert(huge.entries.empty() && huge.warnings.size() == 2);
        assert(!parse_index(R"({"format_version":1e300,"entries":[]})", root, huge, err));
    }

    // the upload thread's queue family: no graphics, a copy-only one first, else a compute one; none: ~0u
    {
        const uint32_t G = kQueueGraphics, C = kQueueCompute, T = kQueueTransfer;
        const uint32_t nvidia[] = {G | C | T, T, C | T}, nvidiaCounts[] = {16, 2, 8};
        assert(transfer_family(nvidia, nvidiaCounts, 0) == 1);
        const uint32_t amd[] = {G | C | T, C | T, T}, amdCounts[] = {1, 4, 2};
        assert(transfer_family(amd, amdCounts, 0) == 2);
        const uint32_t computeOnly[] = {G | C | T, C}, computeOnlyCounts[] = {1, 2};
        assert(transfer_family(computeOnly, computeOnlyCounts, 0) == 1);   // (compute implies copies)
        const uint32_t one[] = {G | C | T}, oneCount[] = {1};
        assert(transfer_family(one, oneCount, 0) == ~0u);                  // a single family: none
        const uint32_t twoGraphics[] = {G | C | T, G | T}, twoCounts[] = {1, 1};
        assert(transfer_family(twoGraphics, twoCounts, 0) == ~0u);         // another graphics family is no copy family
        const uint32_t noQueues[] = {G | C | T, T}, noQueuesCounts[] = {1, 0};
        assert(transfer_family(noQueues, noQueuesCounts, 0) == ~0u);       // a family without queues
        const uint32_t graphicsLast[] = {T, G | C | T}, graphicsLastCounts[] = {1, 1};
        assert(transfer_family(graphicsLast, graphicsLastCounts, 1) == 0);
        const uint32_t video[] = {G | C | T, 0x20}, videoCounts[] = {1, 1};
        assert(transfer_family(video, videoCounts, 0) == ~0u);             // (video decode only: no copies)
    }

    // the upload thread's staging ring
    {
        constexpr uint64_t kFull = StagingRing::kFull;
        StagingRing ring{64};
        assert(ring.empty() && ring.used() == 0);
        assert(ring.alloc(0, 16) == kFull && ring.alloc(65, 16) == kFull && ring.alloc(8, 3) == kFull && ring.alloc(8, 128) == kFull);
        assert(ring.alloc(10, 16) == 0 && ring.alloc(10, 16) == 16);       // aligned after the last
        const uint64_t first = ring.mark();                                // a batch's end
        assert(first == 26 && ring.used() == 26);
        assert(ring.alloc(30, 16) == 32);                                  // 32..62
        const uint64_t second = ring.mark();
        assert(ring.alloc(16, 16) == kFull);                               // would wrap onto the first batch's data
        ring.release_to(first);                                            // the first batch finished
        assert(ring.used() == 62 - 26);
        assert(ring.alloc(16, 16) == 0);                                   // at the start: free up to the first batch's end (26)
        assert(ring.alloc(11, 16) == kFull);                               // 16..27 would pass it
        assert(ring.alloc(10, 16) == 16);
        const uint64_t third = ring.mark();
        ring.release_to(second);
        ring.release_to(second - 10);                                      // (never backwards)
        assert(!ring.empty() && ring.used() == 28);
        assert(ring.alloc(32, 16) == kFull);                               // 32..64 would pass the second batch's end (62)
        assert(ring.alloc(30, 16) == 32);                                  // fills the rest exactly
        assert(ring.alloc(1, 16) == kFull);                                // full
        ring.release_to(third);
        ring.release_to(ring.mark());
        assert(ring.empty());
        assert(ring.alloc(64, 16) == 0);                                   // empty: the whole capacity from a boundary
        ring.release_to(ring.mark());
        assert(ring.alloc(40, 16) == 0 && ring.alloc(8, 16) == 48);
        ring.release_to(ring.mark() + 100);                                // (clamped to what was handed out)
        assert(ring.empty() && ring.alloc(60, 16) == 0);
        // positions only grow: a mark taken earlier never frees later data
        StagingRing big{kStagingRingBytes};
        uint64_t marks[3];
        for (auto& m : marks) { assert(big.alloc(20ull << 20, 16) != kFull); m = big.mark(); }
        assert(big.alloc(20ull << 20, 16) == kFull);                       // (60 MiB live: 20 more would pass the start)
        big.release_to(marks[0]);
        assert(big.alloc(20ull << 20, 16) == 0);                           // wrapped: the first batch's room
        big.release_to(marks[1]); big.release_to(marks[2]);
        assert(big.used() == 24ull << 20);
    }

    // bind rule
    BindFacts f{true, true, false, false, false, false};
    assert(may_bind(f));
    for (int i = 0; i < 6; i++) {
        BindFacts g = f;
        bool* flags[] = {&g.hasReplacement, &g.keyCurrent, &g.gpuWritten, &g.boundAsTarget, &g.needsGuestLevels, &g.artisticMips};
        *flags[i] = !*flags[i];
        assert(!may_bind(g));
    }
    // (review focus 1) a texture the game rewrote after its replacement was made: the replacement is still there and
    // the draw would take it, but the key it was made for is not the texture's now -> the original is bound
    {
        BindFacts stale{true, false, false, false, false, false};
        assert(!may_bind(stale));
        stale.keyCurrent = true;  // (the same contents again: a restore, an invalidation of unchanged bytes)
        assert(may_bind(stale));
    }

    registry_tests();

    // cross-repo pin with ZeldaWWHD Texture Studio's key hash (XXH3-64, seed 0)
    assert(XXH3_64bits("wwhd", 4) == 0x390cfa6e3c090774ull);
    assert(XXH3_64bits("", 0) == 0x2D06800538D394C2ull);

    std::puts("texture_pack_test: ok");
    return 0;
}

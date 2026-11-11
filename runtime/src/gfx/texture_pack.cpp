// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Replacement texture pack registry and loader thread (texture_pack.h).
#include "texture_pack.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// the runtime's logger (runtime.h); declared here so the registry stays free of the runtime's headers and tests can stub it
void log_msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace gfx::texture_pack {
namespace {

struct State {
    Index index;                                  // immutable between load and the next load / shutdown
    std::unordered_set<uint64_t> shapes;
    uint32_t maxDim = 0;
    std::atomic<bool> active{false};
    // made by load beside the index, immutable until the next load (lock-free from any thread): each key's slot in
    // `heldCount` (hold / unhold: the renderer's threads never wait on the loader), each archive's slot in
    // `archiveQueued` (an opened archive is queued once until the loader has expanded it: prefetch's test before its
    // push). `paused`: prefetch_paused.
    std::unordered_map<Key, uint32_t, KeyHash> keySlot;
    std::unique_ptr<std::atomic<uint32_t>[]> heldCount;
    std::unordered_map<const std::vector<Key>*, uint32_t> archiveSlot;
    std::unique_ptr<std::atomic<bool>[]> archiveQueued;
    std::atomic<bool> paused{false};

    std::mutex mu;                                // everything below
    std::condition_variable cv;
    std::thread thread;
    bool stop = false;
    uint64_t maxReady = kMaxReadyBytes;
    std::deque<Key> queue;
    std::unordered_set<Key, KeyHash> inflight;    // queued, loading, or finished and not yet taken
    std::unordered_set<Key, KeyHash> bad;
    std::deque<Loaded> ready;
    std::function<void()> readyNotify;            // (set_ready_notify)
    Stats stats{};
    // prefetch: archives opened and not yet expanded into keys (each once: archiveQueued), with their open's number
    // and slot; the front one is expanded kExpandSlice keys at a time from `archiveAt`
    struct Opened { const std::vector<Key>* keys; uint64_t gen; uint32_t slot; };
    std::deque<Opened> archiveQueue;
    size_t archiveAt = 0;
    uint64_t prefetchGen = 0;
    // keys a prefetch queued (in `prefetchQueue`, served after `queue`) and not yet loading: their open's number, and
    // whether a request moved them into `queue` too (the entry popped second is stale and skipped)
    struct Prefetched { uint64_t gen; bool promoted; };
    std::deque<Key> prefetchQueue;
    std::unordered_map<Key, Prefetched, KeyHash> prefetched;
    // parking (set_prefetch_park, 0: off): prefetched loads no request has asked for, kept here (in `inflight` too)
    // until one does, oldest first in `parkOrder` (a place whose seq is not the key's is stale); the key the loader is
    // reading for a prefetch, and whether a request came meanwhile (it is delivered, not parked)
    uint64_t parkMax = 0, parkSeq = 0;
    struct Parked { Loaded l; uint64_t seq; };
    std::unordered_map<Key, Parked, KeyHash> parked;
    std::deque<std::pair<Key, uint64_t>> parkOrder;
    Key loadingKey{};
    bool loadingPrefetch = false, loadingWanted = false;
};
// keys an archive expansion handles under the loader's lock before letting go of it (prefetch's push, the renderer's
// request and take_ready wait at most this long for an archive of thousands of keys)
constexpr size_t kExpandSlice = 32;
// leaked on purpose: a process that exits without shutdown() must not destroy the mutex under, or terminate on, the running loader thread
State& g = *new State;
} // namespace
std::atomic<bool> g_prefetch_on{false};
namespace {
std::function<void(const std::filesystem::path&, const std::string&)> g_sink;

bool verbose() {
    static const bool v = [] { const char* e = std::getenv("WWHD_TEXTURE_PACK_LOG"); return e && e[0] == '1'; }();
    return v;
}
#define TP_LOG(...) do { if (verbose()) log_msg(__VA_ARGS__); } while (0)

uint64_t shape_id(uint32_t f, uint32_t w, uint32_t h) { return uint64_t(f) << 32 | uint64_t(w) << 16 | h; }
std::string key_text(const Key& k) {
    char b[96];
    std::snprintf(b, sizeof b, "fmt %x %ux%u hash %016llx", k.format, k.width, k.height, (unsigned long long)k.hash);
    return b;
}

// g.mu held
void mark_bad_locked(const Key& key, const std::string& why) {
    g.inflight.erase(key);
    if (auto it = g.parked.find(key); it != g.parked.end()) { g.stats.parkedBytes -= it->second.l.file.size(); g.parked.erase(it); }
    for (auto it = g.ready.begin(); it != g.ready.end(); ++it)   // finished but not taken: never delivered after this
        if (it->key == key) { g.stats.readyBytes -= it->file.size(); g.ready.erase(it); g.cv.notify_all(); break; }   // the loader may be waiting for room
    if (!g.bad.insert(key).second) return;
    g.stats.bad++;
    log_msg("[texture pack] bad entry %s: %s", key_text(key).c_str(), why.c_str());
}

// Reads and checks one entry's DDS; never throws (any failure is `error`, the original texture keeps drawing).
// The file is refused before it is read if it is larger than the entry can legally be: the 148-byte header plus the
// full mip chain at key size times scale, at most 16 bytes per 4x4 block (BC7) or 4 per texel (RGBA8).
bool read_dds(const Key& key, const Entry& entry, uint32_t maxDim, std::vector<uint8_t>& bytes, DdsLevels& dds, std::string& error) {
    try {
        std::ifstream f(entry.file, std::ios::binary | std::ios::ate);
        if (!f) { error = "cannot open " + entry.file.string(); return false; }
        std::streamoff n = f.tellg();
        uint64_t w = uint64_t(key.width) * entry.scale, h = uint64_t(key.height) * entry.scale, limit = 148;
        uint32_t levels = std::min<uint32_t>(15, uint32_t(std::bit_width(std::max(w, h))));   // as check_dds allows
        for (uint32_t i = 0; i < levels; i++) {
            uint64_t lw = std::max<uint64_t>(1, w >> i), lh = std::max<uint64_t>(1, h >> i);
            limit += std::max(((lw + 3) / 4) * ((lh + 3) / 4) * 16, lw * lh * 4);          // BC7, or RGBA8
        }
        if (n < 0) { error = "cannot size the file"; return false; }
        if (uint64_t(n) > limit) { error = "file larger than the texture can need"; return false; }
        bytes.resize(size_t(n));
        f.seekg(0);
        if (n > 0 && !f.read(reinterpret_cast<char*>(bytes.data()), n)) { error = "read failed"; return false; }
        return check_dds(bytes, key, entry, maxDim, dds, error);
    } catch (const std::exception& e) {
        error = std::string("load failed: ") + e.what();
    } catch (...) {
        error = "load failed";
    }
    return false;
}

bool held(const Key& key) {
    auto it = g.keySlot.find(key);
    return it != g.keySlot.end() && g.heldCount[it->second].load(std::memory_order_acquire) != 0;
}

// g.mu held: at most kExpandSlice keys of the first opened archive onto the prefetch queue (all of them skipped while
// paused); returns whether any archive is left to expand
bool expand_slice_locked() {
    if (g.archiveQueue.empty()) return false;
    const State::Opened& a = g.archiveQueue.front();
    const bool paused = g.paused.load(std::memory_order_relaxed);
    for (size_t n = 0; g.archiveAt < a.keys->size() && n < kExpandSlice && !paused; ++n) {
        const Key& key = (*a.keys)[g.archiveAt++];
        if (g.bad.contains(key) || held(key)) continue;
        if (!g.inflight.insert(key).second) {
            if (auto it = g.prefetched.find(key); it != g.prefetched.end()) it->second.gen = a.gen;  // (wanted again now)
            if (auto it = g.parked.find(key); it != g.parked.end() && it->second.l.prefetch < a.gen) {
                it->second.l.prefetch = a.gen;  // (wanted again now: the newest place in the parking order)
                it->second.seq = ++g.parkSeq;
                g.parkOrder.emplace_back(key, g.parkSeq);
            }
            continue;
        }
        g.prefetchQueue.push_back(key);
        g.prefetched[key] = {a.gen, false};
        g.stats.prefetched++;
        TP_LOG("[texture pack] prefetch %s", key_text(key).c_str());
    }
    if (paused || g.archiveAt >= a.keys->size()) {
        g.archiveQueued[a.slot].store(false, std::memory_order_release);
        g.archiveQueue.pop_front();
        g.archiveAt = 0;
    }
    return !g.archiveQueue.empty();
}

// g.mu held: the oldest parked load's archive open number, ~0 when none is parked
uint64_t oldest_parked_gen_locked() {
    while (!g.parkOrder.empty()) {
        auto it = g.parked.find(g.parkOrder.front().first);
        if (it != g.parked.end() && it->second.seq == g.parkOrder.front().second) return it->second.l.prefetch;
        g.parkOrder.pop_front();  // (taken by a request, or bad)
    }
    return ~0ull;
}

// g.mu held: the oldest parked load dropped (its key may be prefetched again); false when none is parked
bool drop_oldest_parked_locked() {
    if (oldest_parked_gen_locked() == ~0ull) return false;
    auto it = g.parked.find(g.parkOrder.front().first);
    g.parkOrder.pop_front();
    TP_LOG("[texture pack] parked %s dropped for a later archive's", key_text(it->first).c_str());
    g.stats.parkedBytes -= it->second.l.file.size();
    g.stats.parkDropped++;
    g.inflight.erase(it->first);
    g.parked.erase(it);
    return true;
}

// g.mu held, a finished prefetched load no request asked for: parked, making room by dropping parked loads of earlier
// archive opens; dropped itself when that is not enough (the room is held by its own open's or later ones)
void park_locked(Loaded&& l) {
    const uint64_t n = l.file.size();
    while (g.stats.parkedBytes + n > g.parkMax && oldest_parked_gen_locked() < l.prefetch && drop_oldest_parked_locked()) {}
    if (g.stats.parkedBytes + n > g.parkMax) {
        TP_LOG("[texture pack] prefetch of %s dropped: parking full", key_text(l.key).c_str());
        g.stats.parkDropped++;
        g.inflight.erase(l.key);
        return;
    }
    TP_LOG("[texture pack] parked %s (%zu bytes) until a request", key_text(l.key).c_str(), l.file.size());
    g.stats.parkedBytes += n;
    const Key key = l.key;
    g.parked[key] = {std::move(l), ++g.parkSeq};   // (moving keeps the file's buffer: the level spans stay valid)
    g.parkOrder.emplace_back(key, g.parkSeq);
}

// g.mu held, room for a load: the next key to load (requests first, then prefetches), its prefetch number (0:
// a request's alone) and whether a request asked for it (`wanted`: also a prefetched key a request moved in front);
// false when none is left
bool next_key_locked(Key& key, uint64_t& prefetch, bool& wanted) {
    for (;;) {
        const bool request = !g.queue.empty();
        if (!request && g.prefetchQueue.empty()) return false;
        std::deque<Key>& from = request ? g.queue : g.prefetchQueue;
        key = from.front();
        from.pop_front();
        prefetch = 0;
        wanted = request;
        if (auto it = g.prefetched.find(key); it != g.prefetched.end()) {
            // paused, or parking full with loads of this archive open or later ones: dropped unread (a request asks
            // for it again)
            const bool parkFull = g.parkMax && g.stats.parkedBytes >= g.parkMax && oldest_parked_gen_locked() >= it->second.gen;
            if (!request && !it->second.promoted && (g.paused.load(std::memory_order_relaxed) || parkFull)) {
                if (parkFull) g.stats.parkDropped++;
                g.prefetched.erase(it);
                g.inflight.erase(key);
                continue;
            }
            prefetch = it->second.gen;
            g.prefetched.erase(it);
        } else if (!request) {
            continue;  // (moved in front by a request and loaded already)
        }
        return true;
    }
}

void loader() {
    std::unique_lock lk(g.mu);
    for (;;) {
        Key key{};
        uint64_t prefetch = 0;
        bool wanted = false;
        try {
            g.cv.wait(lk, [] {
                return g.stop || !g.archiveQueue.empty() ||
                       ((!g.queue.empty() || !g.prefetchQueue.empty()) && g.stats.readyBytes < g.maxReady);
            });
            if (g.stop) return;
            if (expand_slice_locked() && (g.stats.readyBytes >= g.maxReady || (g.queue.empty() && g.prefetchQueue.empty()))) {
                // more to expand and nothing to load meanwhile: the lock goes to its other users between slices
                lk.unlock();
                std::this_thread::yield();
                lk.lock();
                continue;
            }
            if (g.stats.readyBytes >= g.maxReady || !next_key_locked(key, prefetch, wanted)) continue;
            if (g.bad.contains(key)) continue;               // marked bad while it waited
            auto it = g.index.entries.find(key);              // the map is immutable until shutdown has joined this thread
            if (it == g.index.entries.end()) continue;
            const Entry& entry = it->second;
            uint32_t maxDim = g.maxDim;
            g.loadingKey = key;
            g.loadingPrefetch = prefetch && !wanted;
            g.loadingWanted = false;
            lk.unlock();

            std::vector<uint8_t> bytes;
            std::string error;
            DdsLevels dds{};
            bool ok = read_dds(key, entry, maxDim, bytes, dds, error);

            lk.lock();
            const bool park = g.parkMax && g.loadingPrefetch && !g.loadingWanted;   // (no request came meanwhile)
            g.loadingPrefetch = false;
            if (g.stop) return;
            if (g.bad.contains(key)) continue;               // marked bad while it loaded
            if (!ok) { mark_bad_locked(key, error); continue; }
            Loaded l{key, dds.width, dds.height, dds.dxgi, entry.scale, std::move(bytes), std::move(dds.levels), prefetch};
            g.stats.loaded++;
            TP_LOG("[texture pack] loaded %s -> %ux%u dxgi %u (%zu bytes)", key_text(key).c_str(), l.width, l.height, l.dxgi, l.file.size());
            if (park) { park_locked(std::move(l)); continue; }
            g.stats.readyBytes += l.file.size();
            g.ready.push_back(std::move(l));   // moving the vector keeps its buffer, so the level spans stay valid
            if (g.readyNotify) g.readyNotify();
        } catch (...) {
            // out of memory or the like: this key keeps its original texture, the thread lives on
            if (!lk.owns_lock()) lk.lock();
            try { mark_bad_locked(key, "loader exception"); } catch (...) {}
        }
    }
}

bool read_text(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void report(const std::filesystem::path& root, const std::string& text) {
    log_msg("[texture pack] %s: %s", root.string().c_str(), text.c_str());
    if (g_sink) g_sink(root, text);
}

} // namespace

void set_max_ready_bytes_for_test(uint64_t bytes) {
    std::lock_guard lk(g.mu);
    g.maxReady = bytes;
    g.cv.notify_all();
}

void set_prefetch_park(uint64_t maxBytes) {
    std::lock_guard lk(g.mu);
    g.parkMax = maxBytes;
    while (g.stats.parkedBytes > g.parkMax && drop_oldest_parked_locked()) {}
}

void set_status_sink(std::function<void(const std::filesystem::path&, const std::string&)> sink) { g_sink = std::move(sink); }

bool load(const std::vector<std::filesystem::path>& roots, uint32_t maxDim) {
    shutdown();  // (prefetch off)
    g.index = {};
    g.shapes.clear();
    g.bad.clear();
    g.stats = {};
    g.maxDim = maxDim;
    g.maxReady = kMaxReadyBytes;
    for (const auto& root : roots) {
        std::string text, error;
        if (!read_text(root / "index.json", text)) { report(root, "texture pack: index.json failed: cannot read it"); continue; }
        size_t entriesBefore = g.index.entries.size(), warningsBefore = g.index.warnings.size();
        if (!parse_index(text, root, g.index, error)) { report(root, "texture pack: index.json failed: " + error); continue; }
        size_t dup = 0, skipped = 0, archives = 0;
        for (size_t i = warningsBefore; i < g.index.warnings.size(); i++) {
            const std::string& w = g.index.warnings[i];
            (w.find("already replaced") != std::string::npos ? dup : w.find(": archive") != std::string::npos || w.find(": an archive") != std::string::npos ? archives : skipped)++;
            log_msg("[texture pack] %s: %s", root.string().c_str(), w.c_str());
        }
        report(root, "texture pack: " + std::to_string(g.index.entries.size() - entriesBefore) + " entries, " +
                         std::to_string(dup) + " duplicate keys, " + std::to_string(skipped) + " skipped" +
                         (archives ? ", " + std::to_string(archives) + " bad archive names" : std::string()));
    }
    for (const auto& [k, e] : g.index.entries) g.shapes.insert(shape_id(k.format, k.width, k.height));
    g.keySlot.clear();
    for (const auto& [k, e] : g.index.entries) g.keySlot.emplace(k, uint32_t(g.keySlot.size()));
    g.heldCount = std::make_unique<std::atomic<uint32_t>[]>(g.keySlot.size() + 1);
    g.archiveSlot.clear();
    for (const auto& [name, keys] : g.index.archives) g.archiveSlot.emplace(&keys, uint32_t(g.archiveSlot.size()));
    g.archiveQueued = std::make_unique<std::atomic<bool>[]>(g.archiveSlot.size() + 1);
    g.stats.entries = g.index.entries.size();
    if (g.index.entries.empty()) return false;
    g.stop = false;
    g.active = true;
    g.thread = std::thread(loader);
    return true;
}

bool active() { return g.active; }

const Entry* lookup(const Key& key) {
    auto it = g.index.entries.find(key);
    return it == g.index.entries.end() ? nullptr : &it->second;
}

bool has_shape(uint32_t format, uint32_t width, uint32_t height) {
    if (width > 0xFFFF || height > 0xFFFF) return false;
    return g.shapes.contains(shape_id(format, width, height));
}

void request(const Key& key) {
    if (!g.active || !lookup(key)) return;
    std::lock_guard lk(g.mu);
    if (g.stop || g.bad.contains(key)) return;
    if (auto it = g.parked.find(key); it != g.parked.end()) {
        // parked by a prefetch: delivered now, without reading it again (stays in `inflight` until taken)
        Loaded l = std::move(it->second.l);
        g.parked.erase(it);
        l.prefetch = 0;
        g.stats.parkedBytes -= l.file.size();
        g.stats.readyBytes += l.file.size();
        g.stats.requested++;
        g.stats.parkHits++;
        TP_LOG("[texture pack] request %s (parked: delivered)", key_text(key).c_str());
        g.ready.push_back(std::move(l));
        if (g.readyNotify) g.readyNotify();
        return;
    }
    if (g.loadingPrefetch && g.loadingKey == key) g.loadingWanted = true;   // (being read for a prefetch: delivered, not parked)
    if (!g.inflight.insert(key).second) {
        // queued by a prefetch and not loading yet: in front of the prefetches, once
        if (auto it = g.prefetched.find(key); it != g.prefetched.end() && !it->second.promoted) {
            it->second.promoted = true;
            g.queue.push_back(key);
            g.stats.requested++;
            TP_LOG("[texture pack] request %s (prefetched: moved in front)", key_text(key).c_str());
            g.cv.notify_all();
        }
        return;
    }
    g.queue.push_back(key);
    g.stats.requested++;
    TP_LOG("[texture pack] request %s", key_text(key).c_str());
    g.cv.notify_all();
}

std::vector<Loaded> take_ready(uint64_t maxBytes) {
    std::vector<Loaded> out;
    std::lock_guard lk(g.mu);
    uint64_t total = 0;
    while (!g.ready.empty()) {
        uint64_t n = g.ready.front().file.size();
        if (!out.empty() && total + n > maxBytes) break;
        total += n;
        g.stats.readyBytes -= n;
        g.inflight.erase(g.ready.front().key);
        TP_LOG("[texture pack] release %s to the renderer", key_text(g.ready.front().key).c_str());
        out.push_back(std::move(g.ready.front()));
        g.ready.pop_front();
    }
    if (!out.empty()) g.cv.notify_all();
    return out;
}

void set_prefetch(bool on) {
    std::lock_guard lk(g.mu);
    g_prefetch_on.store(on && g.active && !g.stop && !g.index.archives.empty(), std::memory_order_release);
}

void set_prefetch_paused(bool paused) { g.paused.store(paused, std::memory_order_relaxed); }
bool prefetch_paused() { return g.paused.load(std::memory_order_relaxed); }

void prefetch(std::string_view gamePath) {
    if (!prefetch_on()) return;
    char path[512];
    const size_t n = normalize_game_path(gamePath, path);
    if (!n) return;
    const auto it = g.index.archives.find(std::string_view(path, n));  // (immutable while prefetch is on)
    if (it == g.index.archives.end() || g.paused.load(std::memory_order_relaxed)) return;
    const uint32_t slot = g.archiveSlot.find(&it->second)->second;
    if (g.archiveQueued[slot].exchange(true, std::memory_order_acq_rel)) return;  // (queued and not expanded yet)
    {
        std::lock_guard lk(g.mu);  // (only the push)
        if (g.stop || !prefetch_on()) { g.archiveQueued[slot].store(false, std::memory_order_relaxed); return; }
        g.archiveQueue.push_back({&it->second, ++g.prefetchGen, slot});
    }
    g.cv.notify_all();
}

bool has_archive(std::string_view gamePath) {
    char path[512];
    const size_t n = normalize_game_path(gamePath, path);
    return n && g.index.archives.find(std::string_view(path, n)) != g.index.archives.end();
}

void hold(const Key& key) {
    if (auto it = g.keySlot.find(key); it != g.keySlot.end()) g.heldCount[it->second].fetch_add(1, std::memory_order_acq_rel);
}
void unhold(const Key& key) {
    auto it = g.keySlot.find(key);
    if (it == g.keySlot.end()) return;
    std::atomic<uint32_t>& count = g.heldCount[it->second];
    for (uint32_t c = count.load(std::memory_order_relaxed); c && !count.compare_exchange_weak(c, c - 1, std::memory_order_acq_rel);) {}
}

void set_ready_notify(std::function<void()> notify) {
    std::lock_guard lk(g.mu);
    g.readyNotify = std::move(notify);
}

void mark_bad(const Key& key, const std::string& why) {
    std::lock_guard lk(g.mu);
    mark_bad_locked(key, why);
}

Stats stats() {
    std::lock_guard lk(g.mu);
    Stats s = g.stats;
    s.pending = g.inflight.size() - g.parked.size();
    s.parked = g.parked.size();
    return s;
}

void shutdown() {
    {
        std::lock_guard lk(g.mu);
        g.stop = true;
        g.cv.notify_all();
    }
    if (g.thread.joinable()) g.thread.join();
    std::lock_guard lk(g.mu);
    g_prefetch_on.store(false, std::memory_order_release);
    g.queue.clear(); g.ready.clear(); g.inflight.clear();
    g.archiveQueue.clear(); g.archiveAt = 0; g.prefetchQueue.clear(); g.prefetched.clear();
    g.parked.clear(); g.parkOrder.clear(); g.parkMax = 0; g.stats.parkedBytes = 0; g.loadingPrefetch = false;
    for (size_t i = 0; g.heldCount && i <= g.keySlot.size(); ++i) g.heldCount[i].store(0, std::memory_order_relaxed);
    for (size_t i = 0; g.archiveQueued && i <= g.archiveSlot.size(); ++i) g.archiveQueued[i].store(false, std::memory_order_relaxed);
    g.paused.store(false, std::memory_order_relaxed);
    g.stats.readyBytes = 0;
    g.active = false;
}

} // namespace gfx::texture_pack

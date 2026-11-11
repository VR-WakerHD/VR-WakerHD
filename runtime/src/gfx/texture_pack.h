// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Replacement texture packs at run time: the registry of every enabled "textures" package's index.json (Key ->
// DDS file, texture_pack_core.h) and the background thread that reads and checks the DDS files the renderer asks for.
//
// Threading: `load` must finish before any other call and before the renderer starts asking (it also replaces any
// earlier load: it shuts the old thread down first). After that the entry and archive maps are immutable, so
// `active`, `lookup`, `has_shape`, `has_archive` and `prefetch_on` may be called from any thread without a lock. `request`, `take_ready`, `mark_bad`, `stats` and
// `shutdown` are mutex-protected and may be called from any thread. An Entry's `bad` flag is never written after
// load (it would race with lookup): a bad key is tracked inside, `request` ignores it, and `lookup` still returns
// its entry.
//
// Call shutdown() before the process exits; it is not required for safety (the state is never destroyed, so exit()
// with the loader running cannot terminate or touch freed memory).
//
// WWHD_TEXTURE_PACK_LOG=1 logs one line per request, load, bad entry and release; the renderer logs the near
// misses (a guest upload for which has_shape is true but lookup finds nothing).
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "texture_pack_core.h"

namespace gfx::texture_pack {

// Finished-not-taken data the loader may hold; beyond it the loader waits for take_ready.
constexpr uint64_t kMaxReadyBytes = 256ull << 20;

// TEST ONLY: replaces kMaxReadyBytes until the next load, so the loader's wait can be tested without 256 MiB of files.
void set_max_ready_bytes_for_test(uint64_t bytes);

struct Loaded {
    Key key;
    uint32_t width, height, dxgi, scale;
    std::vector<uint8_t> file;                        // the whole DDS file
    std::vector<std::span<const uint8_t>> levels;     // into `file`, largest first
    uint64_t prefetch = 0;   // queued by prefetch: that archive open's number (they only grow); 0: asked for by request alone
};

struct Stats {
    uint64_t entries, requested, loaded, bad, readyBytes;
    uint64_t pending;   // keys queued, loading, or finished and not yet taken (filled in by stats())
    uint64_t prefetched;   // keys queued by prefetch (an archive's keys that were not queued, loading, bad or held)
    // parking (set_prefetch_park): prefetched loads kept now (filled in by stats()) and their DDS bytes; requests
    // served from them and prefetched loads dropped for want of room (or for a later archive's), since load
    uint64_t parked, parkedBytes, parkHits, parkDropped;
};

// Called once per package root by `load` with one summary line ("texture pack: 1234 entries, 2 duplicate keys,
// 1 skipped" or "texture pack: index.json failed: <error>"). Set it before `load`; none set -> only logged.
void set_status_sink(std::function<void(const std::filesystem::path& root, const std::string& text)> sink);

// Reads <root>/index.json of each root (first root wins on a shared key); a failing package is logged and skipped.
// `maxDim` is the device's largest image side. Returns whether any entry exists; the loader thread starts only then.
bool load(const std::vector<std::filesystem::path>& roots, uint32_t maxDim);

bool active();
const Entry* lookup(const Key& key);
// true if any entry has this (format, width, height): a guest texture with that shape but another hash is a near miss
bool has_shape(uint32_t format, uint32_t width, uint32_t height);

// Queues the key's file once (not again while queued, loading or finished-not-taken); ignored for unknown or bad keys.
void request(const Key& key);
// Finished loads in order, up to `maxBytes` of DDS file data in total, but at least one when any is ready.
std::vector<Loaded> take_ready(uint64_t maxBytes);
// Called (on the loader thread, or in request when it delivers a parked load; under the registry's lock: it must not
// call back into this API) each time a load has finished and take_ready would return it; none set (the default) -> nothing. For a consumer that waits for
// loads instead of asking each frame (the renderer's upload thread). Set or clear it while the consumer may still
// be waiting only through this call (it takes the same lock).
void set_ready_notify(std::function<void()> notify);
// Loading with the area: when the game opens an archive file (hle/fs.cpp's file open), every key the index lists for
// that archive ("archives") is queued for loading, so its replacement is usually made before the game first uploads
// the texture. prefetch runs on the game's threads: a path normalization into a stack buffer, a hash lookup in the
// immutable archive map, an atomic test-and-set of the archive's "queued" flag (an archive opened again before the
// loader expanded it costs nothing more) and one push onto the loader's archive queue under its lock (only the push).
// The loader thread expands it into keys, kExpandSlice at a time under its lock (skipping those queued, loading,
// finished and not taken, bad or held), behind every request (a request of a key a prefetch queued moves it in
// front). Off (the default; load and shutdown turn it off) prefetch returns at once; the hook tests prefetch_on first,
// one acquire load (a plain load on x86). set_prefetch(true) has effect only while active with an index that names any
// archive: the renderer turns it on only where it keeps finished replacements nobody asked for yet (texture_pack_vk.cpp,
// the upload thread's path).
extern std::atomic<bool> g_prefetch_on;
inline bool prefetch_on() { return g_prefetch_on.load(std::memory_order_acquire); }
void set_prefetch(bool on);
// `gamePath` as the game passed it to the file open: /vol/content/..., or relative to its cwd (/vol/content)
void prefetch(std::string_view gamePath);
bool has_archive(std::string_view gamePath);
// The renderer's hint that the VRAM budget has no room for prefetched replacements (lock-free, any thread): while set,
// prefetch queues nothing and prefetched keys not loading yet are dropped (a request still loads them). load and
// shutdown clear it.
void set_prefetch_paused(bool paused);
bool prefetch_paused();
// Parking: a prefetched load no request has asked for is kept in the loader (system memory, its DDS bytes counted
// up to `maxBytes`) instead of being delivered by take_ready; a request of its key delivers it at once, without
// reading it again. So the renderer makes a replacement only for a texture the game draws, while the disk reads
// still happen with the area. Room is made by dropping the parked loads of earlier archive opens first; a load that
// still does not fit is dropped, and so are the queued prefetches of an open whose own parked loads fill it (read
// again when its archive is opened again). 0 (load's default): off, every finished load goes to take_ready.
void set_prefetch_park(uint64_t maxBytes);
// The renderer keeps a replacement of this key (made, attached or not): prefetch skips the key until as many unhold
// calls came. Counted; request is never affected. Lock-free (an atomic count per key, its slot fixed at load): the
// render and drawing threads that call these never wait on the loader.
void hold(const Key& key);
void unhold(const Key& key);
// Marks the key bad (logged once): request ignores it from now on.
void mark_bad(const Key& key, const std::string& why);
Stats stats();
// Stops and joins the loader thread, drops queued and finished data. Safe to call twice or without load.
void shutdown();

} // namespace gfx::texture_pack

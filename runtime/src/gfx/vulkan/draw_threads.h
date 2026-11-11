// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Locks for the renderer's shared caches while a second thread prepares draws (docs/VR_ATLAS_THREAD.md, stages 2.4
// onwards: the sun atlas recorded on a thread of its own).
//
// Until that thread exists the locks cost one predictable branch: a CacheLock locks only while second_thread_active().
// That changes only while the second thread is not running: set before it starts, cleared after it has joined (both
// by the render thread), so both threads always agree whether to lock. WWHD_VK_LOCK_ALWAYS=1 takes the locks on one
// thread too (tests: the locked paths run, and their cost shows).
//
// The mutexes are recursive: a cache's code may call back into its own entry points (a translation that looks up its
// linked vertex shader), and a second lock by the same thread must not deadlock. Order, where two are held at once:
// pipelines before shaders (a pipeline's creation reads its shaders' binaries).
#pragma once
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <source_location>
#include <string>
#include <utility>
#include "per_thread.h"
#include "draw_threads.h"

namespace gfxvk::draw_threads {

// (namespace-scope constants and flags, not function statics: tested by every lock, many a draw)
inline const bool kLockAlways = [] { const char* e = std::getenv("WWHD_VK_LOCK_ALWAYS"); return e && !std::strcmp(e, "1"); }();
inline bool g_secondThreadActive = false;
inline bool lock_always() { return kLockAlways; }
// a second thread prepares draws now (or the locks are forced on)
inline bool second_thread_active() { return g_secondThreadActive || kLockAlways; }
// the render thread, before starting and after joining the second thread
inline void set_second_thread_active(bool on) { g_secondThreadActive = on; }

// the shared caches' mutexes
inline std::recursive_mutex& shader_mutex() { static std::recursive_mutex m; return m; }
inline std::recursive_mutex& pipeline_mutex() { static std::recursive_mutex m; return m; }
inline std::recursive_mutex& sampler_mutex() { static std::recursive_mutex m; return m; }
// the eyes' surface maps and the surfaces in them (stage 2.6b)
inline std::recursive_mutex& surface_mutex() { static std::recursive_mutex m; return m; }
// the render pass and framebuffer caches of the render-pass path (devices without dynamic rendering; stage 2.7)
inline std::recursive_mutex& pass_mutex() { static std::recursive_mutex m; return m; }
// the guest buffer cache: a lookup that misses and its upload are one step, so another thread never finds an entry
// that holds no data yet (stage 2.8)
inline std::recursive_mutex& buffer_mutex() { static std::recursive_mutex m; return m; }

// the calling thread is a draw thread (draw_thread.h: the sun atlas's, eye 1's): it never submits or presents
constinit inline thread_local bool onDrawThreadTls = false;
inline bool on_draw_thread() { return per_thread::possible() && onDrawThreadTls; }
// how many CacheLocks the calling thread holds now (the render thread must never wait for the second thread while it
// holds one: that thread may be waiting for the same lock)
inline int& held_locks() { static thread_local int n = 0; return n; }
// (diagnostics) waits for a lock the other thread held: how many and how long (ns), per mutex, since the last take
struct Contention { std::atomic<uint64_t> waits{0}, ns{0}, locks{0}; };
// (diagnostics) WWHD_VK_LOCK_COUNT=1: every lock taken counted (Contention::locks)
inline const bool kLockCount = [] { const char* e = std::getenv("WWHD_VK_LOCK_COUNT"); return e && !std::strcmp(e, "1"); }();
inline Contention& contention(int which) { static Contention c[8]; return c[which & 7]; }
inline int mutex_index(const std::recursive_mutex& m);
// (diagnostics, WWHD_VK_LOCK_COUNT=1) surface locks by site
inline std::atomic<uint64_t> g_lockSites[24]{};
inline void lock_site(int i) { if (kLockCount) g_lockSites[i].fetch_add(1, std::memory_order_relaxed); }
// (diagnostics, WWHD_VK_LOCK_COUNT=1) each contended wait by where it waited and where the lock was taken by the thread
// holding it (file:line of the CacheLock), with its time: the shader and buffer locks' waits had no sites (2026-11-02)
struct Site { const char* file = nullptr; uint32_t line = 0; };
struct SiteWaits { uint64_t waits = 0, ns = 0; };
inline Site& holder_site(int which) { static Site s[8]; return s[which & 7]; }  // (racy by design: a diagnostic)
inline std::mutex& site_waits_mutex() { static std::mutex m; return m; }
inline std::map<std::pair<std::string, std::string>, SiteWaits>& site_waits() {
    static std::map<std::pair<std::string, std::string>, SiteWaits> m;
    return m;
}
inline std::string site_name(const Site& s) {
    if (!s.file) return "?";
    const char* f = s.file;
    for (const char* c = s.file; *c; ++c)
        if (*c == '/' || *c == 92) f = c + 1;  // (92: a backslash)
    return std::string(f) + ":" + std::to_string(s.line);
}
// locks m while a second thread prepares draws
class CacheLock {
public:
    __attribute__((always_inline)) explicit CacheLock(std::recursive_mutex& m,
                                                      std::source_location at = std::source_location::current()) {
        if (second_thread_active()) [[unlikely]] lock(m, at);
    }
    __attribute__((always_inline)) ~CacheLock() {
        if (lock_.owns_lock()) [[unlikely]] unlock();
    }
    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;

private:
    // (docs/VR_SCENE_THREADS.md, "the drawing threads' locks") the mutexes this thread holds through a CacheLock: one it
    // holds already is not locked again (a recursive lock's second entry protects nothing more, and costs as much as the
    // first: about 7,000 of the 17,000 surface locks a frame came from lookups inside lookups)
    static constexpr int kMaxHeld = 8;
    static inline thread_local const std::recursive_mutex* t_held[kMaxHeld] = {};
    static inline thread_local int t_heldCount = 0;
    __attribute__((noinline)) void lock(std::recursive_mutex& m, const std::source_location& at) {
        for (int i = 0; i < t_heldCount; ++i)
            if (t_held[i] == &m) return;  // (held: lock_ stays empty, the destructor does nothing)
        lock_ = std::unique_lock<std::recursive_mutex>(m, std::try_to_lock);
        if (!lock_.owns_lock()) contended(m, at);
        if (kLockCount) {
            contention(mutex_index(m)).locks.fetch_add(1, std::memory_order_relaxed);
            holder_site(mutex_index(m)) = {at.file_name(), at.line()};
        }
        if (t_heldCount < kMaxHeld) t_held[t_heldCount++] = &m;
        ++held_locks();
    }
    __attribute__((noinline)) void unlock() {
        --held_locks();
        for (int i = t_heldCount - 1; i >= 0; --i)
            if (t_held[i] == lock_.mutex()) {
                t_held[i] = t_held[--t_heldCount];
                break;
            }
        lock_.unlock();
    }
    void contended(std::recursive_mutex& m, const std::source_location& at) {
        const Site holder = kLockCount ? holder_site(mutex_index(m)) : Site{};
        const auto t0 = std::chrono::steady_clock::now();
        lock_.lock();
        auto& c = contention(mutex_index(m));
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        c.waits.fetch_add(1, std::memory_order_relaxed);
        c.ns.fetch_add(ns, std::memory_order_relaxed);
        if (kLockCount) {
            std::lock_guard<std::mutex> l(site_waits_mutex());
            auto& w = site_waits()[{site_name({at.file_name(), at.line()}), site_name(holder)}];
            ++w.waits;
            w.ns += ns;
        }
    }
    std::unique_lock<std::recursive_mutex> lock_;
};
inline int mutex_index(const std::recursive_mutex& m) {
    return &m == &shader_mutex() ? 0 : &m == &pipeline_mutex() ? 1 : &m == &sampler_mutex() ? 2
         : &m == &surface_mutex() ? 3 : &m == &pass_mutex() ? 4 : &m == &buffer_mutex() ? 5 : 6;
}

}  // namespace gfxvk::draw_threads

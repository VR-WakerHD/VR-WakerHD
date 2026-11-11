// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The renderer's hot threads on the CPU's core groups (docs/VR_SCENE_THREADS.md, "S6: the threads on the CPU's core
// groups"; on by default since 2026-11-04, WWHD_VK_PLACE_THREADS=0 turns it off; Windows and desktop Linux on x86-64
// (from 2026-11-04, the L3 groups read from sysfs), nothing elsewhere, and nothing on a CPU with one L3 for all its
// cores). In the headset (PS VR2, Forest Haven) the 4,000-draw views went
// from 84.0 to 89.5 fps, the helpers from 9.6-11 to 7.4-8.5 us a draw.
//
// A CPU whose cores share their L3 cache in groups (AMD Zen: a Ryzen 9 3900X has four groups of three cores) reads
// another group's data several times more slowly. The scene's drawing threads (A and B) share most of the renderer's
// data with the render thread, and drew at 9 to 10 us a draw against its 5. With the switch: the render thread and the
// two drawing threads each on a core of their own in one group (one logical processor a core: no two of them, and
// nothing else of ours, sharing a core), and the other busy threads (the atlas's thread, the record worker, the
// end-frame thread, the game's threads) on the other groups. Which thread draws what is unchanged: the pictures are.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)
#define WWHD_CORE_GROUPS_LINUX 1
#include <pthread.h>
#include <sched.h>
#include <cerrno>
#include <cstdio>
#include <string>
#endif
#if defined(_WIN32) || defined(WWHD_CORE_GROUPS_LINUX)
#define WWHD_CORE_GROUPS 1
#endif
#if defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
#include "perf_hint.h"
#endif

void log_msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace core_groups {
enum class Role { Scene, Other };

inline bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("WWHD_VK_PLACE_THREADS");
        return !(e && !std::strcmp(e, "0"));
    }();
    return on;
}

struct Layout {
    uint64_t scene = 0, other = 0, group = 0;
    bool ok = false;
};
// (both systems) the layout from the L3 groups' and the cores' processor masks (the first 64 logical processors) and
// the processors the process may use
inline Layout make_layout(const std::vector<uint64_t>& l3, const std::vector<uint64_t>& cores, uint64_t process) {
    Layout l;
    if (l3.size() < 2) return l;  // (one L3 for all: nothing to place)
    // the scene's group: the last (the first takes most of the system's interrupts and other programs' threads)
    for (uint64_t m : l3)
        if (m > l.group) l.group = m;
    for (uint64_t c : cores)
        if (c & l.group) l.scene |= c & (~c + 1);  // (the core's first logical processor)
    l.scene &= process;
    l.other = process & ~l.group;
    int n = 0;
    for (uint64_t m = l.scene; m; m &= m - 1) ++n;
    l.ok = n >= 3 && l.other;
    return l;
}
// a sysfs CPU list ("0-2,12-14") as a mask of the first 64 logical processors
inline uint64_t parse_cpu_list(const char* text) {
    uint64_t mask = 0;
    const char* p = text;
    while (p && *p) {
        char* end = nullptr;
        const long a = std::strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        p = end;
        if (*p == '-') {
            b = std::strtol(p + 1, &end, 10);
            if (end == p + 1) break;
            p = end;
        }
        for (long i = a; i <= b && i < 64; ++i)
            if (i >= 0) mask |= uint64_t(1) << i;
        while (*p == ',' || *p == ' ' || *p == '\n') ++p;
    }
    return mask;
}

#ifdef _WIN32
// the processor masks of one relationship (processor group 0: up to 64 logical processors)
inline std::vector<uint64_t> masks_of(LOGICAL_PROCESSOR_RELATIONSHIP rel) {
    std::vector<uint64_t> out;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(rel, nullptr, &bytes);
    if (!bytes) return out;
    std::vector<uint8_t> buf(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(rel, info, &bytes)) return out;
    for (DWORD off = 0; off < bytes;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.GroupMask.Group == 0)
            out.push_back(uint64_t(e->Cache.GroupMask.Mask));
        else if (e->Relationship == RelationProcessorCore && e->Processor.GroupMask[0].Group == 0)
            out.push_back(uint64_t(e->Processor.GroupMask[0].Mask));
        off += e->Size;
    }
    return out;
}
inline Layout system_layout() {
    DWORD_PTR process = 0, system = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    return make_layout(masks_of(RelationCache), masks_of(RelationProcessorCore), uint64_t(process));
}
#elif defined(WWHD_CORE_GROUPS_LINUX)
inline bool read_text(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    out = buf;
    return n > 0;
}
// (sysfs) each logical processor's L3 (cache/index*/level 3, shared_cpu_list) and its core's processors
// (topology/thread_siblings_list), each group once
inline Layout system_layout() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set)) return {};
    uint64_t process = 0;
    for (int cpu = 0; cpu < 64; ++cpu)
        if (CPU_ISSET(cpu, &set)) process |= uint64_t(1) << cpu;
    std::vector<uint64_t> l3, cores;
    auto add = [](std::vector<uint64_t>& v, uint64_t m) {
        if (!m) return;
        for (uint64_t x : v)
            if (x == m) return;
        v.push_back(m);
    };
    for (int cpu = 0; cpu < 64; ++cpu) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
        std::string text;
        if (!read_text(base + "/topology/thread_siblings_list", text)) continue;
        add(cores, parse_cpu_list(text.c_str()));
        for (int index = 0; index < 8; ++index) {
            const std::string cache = base + "/cache/index" + std::to_string(index);
            if (!read_text(cache + "/level", text)) break;
            if (std::atoi(text.c_str()) != 3) continue;
            if (read_text(cache + "/shared_cpu_list", text)) add(l3, parse_cpu_list(text.c_str()));
        }
    }
    return make_layout(l3, cores, process);
}
#endif

#ifdef WWHD_CORE_GROUPS
inline const Layout& layout() {
    static const Layout l = [] {
        const Layout l = system_layout();
        if (l.group)
            log_msg("[threads] core groups: the render thread and the scene's drawing threads on %016llX (one logical "
                    "processor a core of %016llX), the other busy threads on %016llX%s",
                    (unsigned long long)l.scene, (unsigned long long)l.group, (unsigned long long)l.other,
                    l.ok ? "" : " (not placed: under three cores there)");
        return l;
    }();
    return l;
}
#endif

// (at a thread's start) the thread on its role's cores (WWHD_VK_PLACE_THREADS=0: left where the system puts it)
inline void place_current_thread(Role role, const char* name) {
    if (!enabled()) return;
#ifdef WWHD_CORE_GROUPS
    const Layout& l = layout();
    if (!l.ok) return;
    const uint64_t mask = role == Role::Scene ? l.scene : l.other;
#ifdef _WIN32
    if (!SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(mask)))
        log_msg("[threads] %s: affinity %016llX not set (error %lu)", name, (unsigned long long)mask,
                (unsigned long)GetLastError());
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu)
        if (mask & (uint64_t(1) << cpu)) CPU_SET(cpu, &set);
    if (const int err = pthread_setaffinity_np(pthread_self(), sizeof set, &set))
        log_msg("[threads] %s: affinity %016llX not set (error %d)", name, (unsigned long long)mask, err);
#endif
#elif defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
    // (Steam Frame - Native, Linux on ARM, the same since 2026-11-04: its Snapdragon has fast and slow cores too)
    // (2026-11-04, asked for: the Quest's form of the core groups) Android has no shared-cache groups to choose
    // between; its cores differ in speed instead (a Quest 3: four fast, two slow). The render thread, the game thread
    // and the record worker are already on the fastest cores (perf_hint::add_current_thread); the scene's drawing
    // threads, which stand in for the render thread, join them there (a Quest 3 log: 17 to 21 us a draw on them, left
    // to the scheduler, against 7 to 10 on a PC). The other threads (the atlas's, the end-frame thread) stay where the
    // system puts them: the "other" cores here are the slow ones, which the atlas's 650 to 800 draws a frame would
    // wait on. WWHD_VK_PLACE_THREADS=0 or WWHD_FAST_CORES=0: not placed
    if (role == Role::Scene) perf_hint::pin_to_fastest_cores();
    (void)name;
#else
    (void)role;
    (void)name;
#endif
}
}  // namespace core_groups

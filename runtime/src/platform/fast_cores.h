// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The CPU's fastest cores (by their maximum clock in sysfs) and a thread placed on them: Android (a Quest, the Steam
// Frame's Lepton layer, phones) and Linux on ARM (Steam Frame - Native), whose Snapdragons have a few fast cores
// beside slower ones. Shared by perf_hint (the render thread, the game thread, the record worker) and core_groups (the
// scene's drawing threads). WWHD_FAST_CORES=0: nothing placed. Elsewhere not compiled in (Windows and x86-64 Linux use
// core_groups' cache groups).
#pragma once
#if defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
#define WWHD_FAST_CORES 1
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

void log_msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace fast_cores {
// The cores with the highest maximum clock (a Snapdragon 8 Elite has two at 4.47 GHz next to six at 3.53 GHz). At
// least two: a Snapdragon 8 Gen 3 has one 3.3 GHz core, and with every game thread on it the agl boot crash happened
// on every start (fixed in GX2CopySurface, docs/decomp-notes.md "Fixed: intermittent boot crash"); the next clock
// steps are added until there are two or more
inline cpu_set_t fastest(int& count) {
    cpu_set_t set;
    CPU_ZERO(&set);
    count = 0;
    long best = 0;
    std::vector<long> freq;
    for (int cpu = 0; cpu < CPU_SETSIZE && cpu < 64; cpu++) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        FILE* f = std::fopen(path, "r");
        if (!f) break;
        long v = 0;
        if (std::fscanf(f, "%ld", &v) != 1) v = 0;
        std::fclose(f);
        freq.push_back(v);
        best = std::max(best, v);
    }
    long floor = best;
    for (;;) {
        int n = 0;
        long next = 0;
        for (long v : freq) {
            if (v >= floor) n++;
            else next = std::max(next, v);
        }
        if (n >= 2 || next <= 0) break;
        floor = next;
    }
    for (size_t cpu = 0; cpu < freq.size(); cpu++)
        if (best > 0 && freq[cpu] >= floor) { CPU_SET(int(cpu), &set); count++; }
    return set;
}
// the calling thread on the fastest cores, only on CPUs with distinct fast cores (all at one clock: nothing to choose)
inline void pin_current_thread() {
    static const bool pin = [] { const char* e = std::getenv("WWHD_FAST_CORES"); return !e || std::atoi(e) != 0; }();
    if (!pin) return;
    int n = 0;
    cpu_set_t set = fastest(n);
    if (n > 0 && n < int(sysconf(_SC_NPROCESSORS_CONF))) {
        const bool ok = sched_setaffinity(0, sizeof set, &set) == 0;
        log_msg("[perf] thread %ld on the %d fastest cores: %s", long(syscall(SYS_gettid)), n, ok ? "yes" : "refused");
    }
}
}  // namespace fast_cores
#endif

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Core groups (runtime/src/platform/core_groups.h): sysfs CPU lists and the layout from a CPU's L3 groups, as on a
// Ryzen 9 3900X (four L3 groups of three cores, two logical processors a core) and on a CPU with one L3.
#include "../src/platform/core_groups.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>

void log_msg(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
    std::fputc('\n', stderr);
}

int main() {
    using core_groups::make_layout;
    using core_groups::parse_cpu_list;
    assert(parse_cpu_list("0-2,12-14\n") == 0x7007ull);
    assert(parse_cpu_list("5") == 0x20ull);
    assert(parse_cpu_list("0,63") == (1ull | (1ull << 63)));
    assert(parse_cpu_list("60-70") == 0xF000000000000000ull);  // (processors from 64 on: not in the mask)
    assert(parse_cpu_list("") == 0);

    // 3900X: group g has cores 3g..3g+2 (processors 3g+i and 12+3g+i)
    std::vector<uint64_t> l3, cores;
    for (int g = 0; g < 4; ++g) {
        uint64_t m = 0;
        for (int i = 0; i < 3; ++i) {
            const int c = 3 * g + i;
            cores.push_back((1ull << c) | (1ull << (12 + c)));
            m |= cores.back();
        }
        l3.push_back(m);
    }
    const uint64_t all = (1ull << 24) - 1;
    const auto l = make_layout(l3, cores, all);
    assert(l.ok);
    assert(l.group == parse_cpu_list("9-11,21-23"));
    assert(l.scene == parse_cpu_list("9-11"));  // (one logical processor a core)
    assert(l.other == parse_cpu_list("0-8,12-20"));

    // one L3 for all: nothing placed
    assert(!make_layout({all}, cores, all).ok);
    // the process kept off the last group's cores: under three there, not placed
    assert(!make_layout(l3, cores, parse_cpu_list("0-8,12-20")).ok);
    std::puts("core_groups: ok");
    return 0;
}

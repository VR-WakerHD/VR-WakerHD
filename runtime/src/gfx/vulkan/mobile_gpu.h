// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Paths for a mobile, tile-based GPU only: a standalone headset (Quest, other Android builds, the Steam Frame's native
// build). They are compiled into no other build (Windows and Linux on x64 keep their code as it was: user, 2026-11-02).
// A desktop test build may compile them in to check them (CMake -DWWHD_TEST_MOBILE_GPU_PATHS=ON, its own build folder).
#pragma once

#if defined(__ANDROID__) || defined(__aarch64__) || defined(WWHD_TEST_MOBILE_GPU_PATHS)
#define WWHD_MOBILE_GPU_PATHS 1
#endif

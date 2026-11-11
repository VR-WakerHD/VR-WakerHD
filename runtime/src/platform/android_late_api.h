// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Android functions newer than the oldest Android the game library loads on, looked up at run time instead of
// linked (docs/VR_STEAM_FRAME.md, "Steam Frame - Lepton").
//
// libmain.so used to link backtrace() (Android 13), APerformanceHint_* (13) and AThermal_getThermalHeadroom (12)
// directly: on an older Android the library then does not load at all ("cannot locate symbol"). Steam Frame's
// Lepton is Android 11 and Valve asks for Android 10, so the Lepton build targets API 29 and these come from here:
// each is found once, by name, and is null where the system does not have it; callers skip that feature. On
// Android 13 and newer (phones, Quest) they resolve to the same functions as before.
#pragma once
#ifdef __ANDROID__
#include <dlfcn.h>
#include <cstddef>
#include <cstdint>

namespace android_late_api {

// a function of a system library by name (the library is already loaded by the process: libc, libandroid)
template <class F>
inline F find(const char* library, const char* name) {
    void* lib = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen(library, RTLD_NOW);
    return lib ? reinterpret_cast<F>(dlsym(lib, name)) : nullptr;
}

// <execinfo.h> (Android 13)
using BacktraceFn = int (*)(void**, int);
using BacktraceSymbolsFdFn = void (*)(void* const*, int, int);
inline BacktraceFn backtrace_fn() {
    static const BacktraceFn f = find<BacktraceFn>("libc.so", "backtrace");
    return f;
}
inline BacktraceSymbolsFdFn backtrace_symbols_fd_fn() {
    static const BacktraceSymbolsFdFn f = find<BacktraceSymbolsFdFn>("libc.so", "backtrace_symbols_fd");
    return f;
}

// <android/thermal.h> (getThermalHeadroom: Android 12) and <android/performance_hint.h> (Android 13), from
// libandroid. The managers and sessions are opaque: plain pointers here, so nothing depends on how a given NDK's
// headers declare them at a lower API level
struct Thermal {
    using Acquire = void* (*)();
    using Headroom = float (*)(void*, int);
    Acquire acquire = find<Acquire>("libandroid.so", "AThermal_acquireManager");
    Headroom headroom = find<Headroom>("libandroid.so", "AThermal_getThermalHeadroom");
};
inline const Thermal& thermal() {
    static const Thermal t;
    return t;
}
struct PerformanceHint {
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
    using CloseSession = void (*)(void*);
    using UpdateTarget = int (*)(void*, int64_t);
    using ReportActual = int (*)(void*, int64_t);
    GetManager getManager = find<GetManager>("libandroid.so", "APerformanceHint_getManager");
    CreateSession createSession = find<CreateSession>("libandroid.so", "APerformanceHint_createSession");
    CloseSession closeSession = find<CloseSession>("libandroid.so", "APerformanceHint_closeSession");
    UpdateTarget updateTarget = find<UpdateTarget>("libandroid.so", "APerformanceHint_updateTargetWorkDuration");
    ReportActual reportActual = find<ReportActual>("libandroid.so", "APerformanceHint_reportActualWorkDuration");
    bool available() const { return getManager && createSession && closeSession && updateTarget && reportActual; }
};
inline const PerformanceHint& performance_hint() {
    static const PerformanceHint p;
    return p;
}

// <android/native_window.h> ANativeWindow_setFrameRateWithChangeStrategy (Android 12); null on Android 11, where the
// caller uses ANativeWindow_setFrameRate (Android 11) instead. The window is opaque here as above
using SetFrameRateWithStrategyFn = int32_t (*)(void* window, float rate, int8_t compatibility, int8_t strategy);
inline SetFrameRateWithStrategyFn set_frame_rate_with_strategy_fn() {
    static const SetFrameRateWithStrategyFn f =
        find<SetFrameRateWithStrategyFn>("libandroid.so", "ANativeWindow_setFrameRateWithChangeStrategy");
    return f;
}

}  // namespace android_late_api
#endif

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The platform target this build is and the device it runs on (docs/VR_STEAM_FRAME.md), for the start-up log.
//
// Steam Frame runs the game two ways, each a build target of its own:
// - Steam Frame - Lepton: the Android VR app (the APK's steamframeLepton flavor, CMake WWHD_STEAM_FRAME_LEPTON)
//   inside Lepton, Valve's Android container (LineageOS 11; ro.product.manufacturer "Valve", model "Lepton").
// - Steam Frame - Native: the Linux arm64 build (CMake WWHD_STEAM_FRAME_NATIVE) on SteamOS arm64
//   (/etc/os-release ID=steamos on aarch64), with its own OpenXR loader next to the game.
//
// Logged once at VR start, so a log says which build it came from and what it ran on; nothing else depends on it.
#pragma once
#include <cstdio>
#include <cstring>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace vr {

// the build's target
inline const char* build_target() {
#if defined(WWHD_STEAM_FRAME_LEPTON)
    return "Steam Frame - Lepton (Android VR, arm64)";
#elif defined(WWHD_STEAM_FRAME_NATIVE)
    return "Steam Frame - Native (Linux arm64, SteamOS arm64)";
#elif defined(__ANDROID__)
    return "Android VR (standalone headsets)";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "other";
#endif
}

// the CPU architecture the build is for
inline const char* build_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#else
    return "other";
#endif
}

// "Steam Frame - Lepton", "Steam Frame - Native (SteamOS arm64)", or nullptr: the device, whatever the build
inline const char* steam_frame_device() {
#if defined(__ANDROID__)
    char maker[PROP_VALUE_MAX]{}, model[PROP_VALUE_MAX]{};
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    if (!std::strcmp(model, "Lepton") || !std::strcmp(maker, "Valve")) return "Steam Frame - Lepton";
    return nullptr;
#elif defined(__linux__) && defined(__aarch64__)
    // os-release's ID line (freedesktop.org os-release): SteamOS names itself "steamos"
    FILE* f = std::fopen("/etc/os-release", "r");
    if (!f) f = std::fopen("/usr/lib/os-release", "r");
    if (!f) return nullptr;
    char line[256];
    bool steamos = false;
    while (std::fgets(line, sizeof line, f))
        if (!std::strncmp(line, "ID=", 3) &&
            (!std::strncmp(line + 3, "steamos", 7) || !std::strncmp(line + 3, "\"steamos\"", 9)))
            steamos = true;
    std::fclose(f);
    return steamos ? "Steam Frame - Native (SteamOS arm64)" : nullptr;
#else
    return nullptr;
#endif
}

}  // namespace vr

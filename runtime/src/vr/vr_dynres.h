// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR dynamic resolution (docs/VR_DYNAMIC_RESOLUTION.md): while the GPU holds the headset's frame rate back, each eye is
// drawn at a lower resolution, and higher again once there is room. It is the game's own dynamic resolution
// (dynres.h, dynres_core.h): GPU time from timestamps on every command buffer (no CPU time, no waiting for the GPU),
// judged every half second against the frame rate the headset is paced at (90 fps at 90 Hz; 45 when capped at half),
// one fixed level at a time (1, 0.875, 0.75, 0.625, 0.5 of the internal resolution, whose images are kept, so a
// change copies and does not allocate), with a hold that doubles when a step up has to be undone. A game held back by
// the CPU leaves the GPU idle and keeps its resolution. The eye picture's size per frame is what the upscalers and
// the projection layer already take (the region shown of the eye image), so nothing else changes size.
//
// The profiles: the scale's range (linear: 0.75 is 56 % of the pixels), the GPU target (the busy share that counts as
// overloaded; up again at 10 points under it, as predicted for the next level up) and the settle time (the first
// hold before a step up, doubled up to 32 s by swings):
//   Quality      0.875 .. 1    target 90 %   settle 4 s
//   Balanced     0.75  .. 1    target 85 %   settle 2 s
//   Performance  0.625 .. 1    target 80 %   settle 2 s
//   Custom       vr.dynresMin .. vr.dynresMax, vr.dynresGpuTarget, vr.dynresSettle
// The maximum is the internal resolution (Graphics): a scale over 1 is a higher internal resolution, chosen there.
//
// Standalone headsets on Qualcomm's own Adreno driver (Quest, Steam Frame - Lepton) only with multiview off: two Quest 3
// crashes in that driver came right after a change of scale that rebuilt multiview's two-eye images
// (docs/VR_ANDROID.md). Mesa's Turnip driver (Steam Frame - Native, SteamOS) is another driver: multiview may stay on
// there (2026-11-03, untested in the headset). Pure logic (vr_test checks it).
#pragma once
#include <algorithm>
#include <cstring>
#include <atomic>

namespace vr::dynres_vr {

// vr.dynamicResolution: off, quality, balanced, performance, custom
enum class Mode : int { Off = 0, Quality = 1, Balanced = 2, Performance = 3, Custom = 4 };

struct Profile {
    float minScale = 1.0f, maxScale = 1.0f;  // of the internal resolution (linear)
    float gpuTarget = 0.90f;                 // busy share counted as overloaded
    float settleSeconds = 2.0f;              // the first hold before a step up
};

struct Custom {
    float minScale = 0.75f, maxScale = 1.0f, gpuTargetPercent = 90.0f, settleSeconds = 2.0f;
};

// the profile of a mode (Off: the full range, unused)
inline Profile profile(Mode m, const Custom& c = {}) {
    switch (m) {
    case Mode::Quality: return {0.875f, 1.0f, 0.90f, 4.0f};
    case Mode::Balanced: return {0.75f, 1.0f, 0.85f, 2.0f};
    case Mode::Performance: return {0.625f, 1.0f, 0.80f, 2.0f};
    case Mode::Custom: {
        Profile p;
        p.maxScale = std::clamp(c.maxScale, 0.5f, 1.0f);
        p.minScale = std::clamp(c.minScale, 0.5f, p.maxScale);
        p.gpuTarget = std::clamp(c.gpuTargetPercent, 50.0f, 98.0f) / 100.0f;
        p.settleSeconds = std::clamp(c.settleSeconds, 0.5f, 32.0f);
        return p;
    }
    case Mode::Off:
    default: return {};
    }
}

struct Device {
    bool standalone = false;  // the headset's own GPU renders (Android, an arm64 headset such as the Frame)
    bool multiview = false;   // both eyes in one multiview pass this session
    bool mesaDriver = false;  // Mesa's driver (Turnip on Adreno), not Qualcomm's own (VkDriverId)
    // a standalone headset's fixed factor (dynres::fixed_factor: 0.75 on Quest and Steam Frame by default): the profiles
    // top out there (the user, 2026-11-03: a profile reaching 1x raised the Frame's load above its fixed 0.75x before
    // stepping down, and frame synthesis smeared meanwhile). Custom keeps its own range (vr.dynresMax)
    float fixedMax = 1.0f;
};

struct Choice {
    bool on = false;
    Profile profile;
    const char* why = "";  // for the log and the settings' status line
};

inline Choice choose(Mode m, const Custom& custom, const Device& d) {
    Choice c;
    if (m == Mode::Off) { c.why = "off (vr.dynamicResolution)"; return c; }
    if (d.standalone && d.multiview && !d.mesaDriver) {
        c.why = "off: multiview is on with Qualcomm's Adreno driver (a change of scale rebuilds its two-eye images, "
                "which preceded that driver's crashes); turn multiview off to use it";
        return c;
    }
    c.on = true;
    c.profile = profile(m, custom);
    // (standalone) no profile above the fixed factor; its minimum never above its maximum (Quality and Balanced then
    // hold the fixed factor, Performance goes 0.625 .. 0.75)
    if (d.standalone && m != Mode::Custom && d.fixedMax > 0.0f && d.fixedMax < c.profile.maxScale) {
        c.profile.maxScale = d.fixedMax;
        c.profile.minScale = std::min(c.profile.minScale, c.profile.maxScale);
    }
    c.why = m == Mode::Quality ? "Quality" : m == Mode::Balanced ? "Balanced" : m == Mode::Performance ? "Performance"
                                                                                                     : "Custom";
    return c;
}

inline const char* mode_key(Mode m) {
    switch (m) {
    case Mode::Quality: return "quality";
    case Mode::Balanced: return "balanced";
    case Mode::Performance: return "performance";
    case Mode::Custom: return "custom";
    case Mode::Off:
    default: return "off";
    }
}
// false: not a mode's key
inline bool parse_mode(const char* s, Mode& m) {
    for (Mode x : {Mode::Off, Mode::Quality, Mode::Balanced, Mode::Performance, Mode::Custom})
        if (!std::strcmp(s, mode_key(x))) { m = x; return true; }
    return false;
}

// the choice in use and why, for F1 > VR (set by the VR renderer each headset frame: a pointer to a fixed text, no
// allocation); nullptr: not in VR
inline std::atomic<const char*>& status_why() { static std::atomic<const char*> w{nullptr}; return w; }
inline std::atomic<bool>& status_on() { static std::atomic<bool> on{false}; return on; }

}  // namespace vr::dynres_vr

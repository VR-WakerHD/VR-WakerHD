// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The stereo renderer's diagnostic and experimental switches, read once at start (docs/VR_ATLAS_THREAD.md,
// docs/VR_EYE_THREAD.md). They are tested on every draw; as namespace-scope constants each test is one load of a
// constant flag, never a call or a function static's guard.
#pragma once
#include <cstdlib>
#include <cstring>
#include "platform/quest_theater.h"

namespace gfxvk::switches {
inline bool env_on(const char* name) {
    const char* e = std::getenv(name);
    return e && !std::strcmp(e, "1");
}
// a switch on by default (docs/VR_PERFORMANCE.md, "Multiview and the atlas thread by default"): off only with <name>=0.
// Android keeps them off unless asked for (=1), a Quest too: on by default there (2026-10-29, never tried in the
// headset first) the first standalone Quest 3 run showed black squares all over the picture, at the same frame rate
// as without them (27 against about 24 frames/s; docs/VR_ANDROID.md, "Standalone frame rate").
// per_thread.h kPossible follows the same rule for the atlas thread
inline bool android_default_on() {
#ifdef __ANDROID__
    return false;
#else
    return true;
#endif
}
inline bool env_default_on(const char* name) {
    const char* e = std::getenv(name);
    if (!e) return android_default_on();
    return std::strcmp(e, "0") != 0;
}
// Multiview (WWHD_VK_MULTIVIEW, WWHD_VK_MULTIVIEW_DRAWS) on its own: on by default on a Quest too (2026-10-31). The
// Quest 3 supports it (Adreno 740: up to 6 views), and without it every draw is recorded twice on the headset's CPU,
// the limit there (a Quest 3 log: 30 frames/s at most, the GPU 50 to 60 % busy). The atlas stream and thread, the
// other suspects of the black squares (above), stay off on Android. =0 turns it off, =1 on any Android device
// Off again on a Quest (2026-10-31): a Quest 3 log drew only 215 of about 3,400 eye-0 draws a frame for both eyes (the
// rest per eye, in the same frames as the multiview passes, on the same two-layer images), and the player's footage
// (the left eye) showed the black squares again: tile-shaped holes along the horizon and the far sea, with the atlas
// paths off. Multiview is the one thing on in both runs that showed them. =1 turns it on, and its log then says why
// draws fall back (docs/VR_ANDROID.md, "Multiview falls back")
// On again on a Quest (2026-11-03, the user's choice), with the Quest 3 audit's fixes for its fallback and its black
// squares (mobile GPU builds: uniform_plan.h, the multiview twin within the cap, and draw.cpp's mixed-pass guard,
// kMultiviewNoMix, on by default with it): QUEST3_IMPLEMENTATION_ROADMAP.md 2.1. Other Android devices stay off.
// WWHD_VK_MULTIVIEW=0 in env.txt turns it off on a Quest
// (2026-11-04, the user's choice) Steam Frame - Lepton takes the Quest 3's standalone settings: on there too
// (quest_theater::standalone_headset: a Quest or Lepton); phones stay off
inline bool multiview_default_on(const char* name) {
    const char* e = std::getenv(name);
    if (e) return std::strcmp(e, "0") != 0;
#ifdef __ANDROID__
    return quest_theater::standalone_headset();
#else
    return true;
#endif
}
// The sun atlas's held stream and thread (WWHD_VK_ATLAS_STREAM, WWHD_VK_ATLAS_THREAD): on by default on a Quest again
// (2026-10-31, asked for: the Quest is CPU-bound, and the thread moves the atlas's 650 to 800 draws a frame off the
// render thread). The black squares of the first run with all three on (above) are not tied to them yet: the Quest
// log after multiview came back alone showed none. =0 turns each off, =1 on any Android device
// (2026-11-04) Steam Frame - Lepton as a Quest (standalone_headset); the scene's drawing threads follow this rule
inline bool atlas_default_on(const char* name) {
    const char* e = std::getenv(name);
    if (e) return std::strcmp(e, "0") != 0;
#ifdef __ANDROID__
    return quest_theater::standalone_headset();
#else
    return true;
#endif
}
// The scene's drawing threads (WWHD_VK_SCENE_THREAD, WWHD_VK_SCENE_TWO; docs/VR_SCENE_THREADS.md): on by default on a
// Quest (standalone) too since 2026-11-04 (asked for: multiview, which the second drawing thread's span needs, is on
// there again, and the Quest is CPU-bound), as the atlas's thread; other Android devices keep them off. Never run on a
// Quest before: watch the log's scene thread lines (its cost a draw, the joins' waits). =0 turns each off, =1 on any
// Android device
inline bool scene_default_on(const char* name) { return atlas_default_on(name); }
inline const bool kAtlasStream = atlas_default_on("WWHD_VK_ATLAS_STREAM");  // the sun atlas's held stream
inline const bool kAtlasTimeline = env_on("WWHD_VK_ATLAS_TIMELINE");  // (diagnostics) one frame's atlas events
inline const bool kEyeThread = env_on("WWHD_VK_EYE_THREAD");          // eye 1's draws on a thread of their own
inline const bool kEyeSegments = env_on("WWHD_VK_EYE_SEGMENTS") || kEyeThread;  // both eyes' held segments
// (docs/VR_SCENE_THREADS.md, stages S1 to S3) the scene's parts in a held stream of their own; with the scene thread
// (S3) recorded there, one draw at a time
// (2026-10-31, the user's request: a build with S6 on) on by default, =0 off (Android: on a Quest, else off unless =1:
// scene_default_on)
inline const bool kSceneThread = scene_default_on("WWHD_VK_SCENE_THREAD");
inline const bool kSceneSplit = env_on("WWHD_VK_SCENE_SPLIT") || kSceneThread;
// (stage S5b) the scene's thread records into the renderer's own stream, published to the record worker as it goes;
// WWHD_VK_SCENE_HELD=1 keeps S5's held part stream instead
inline const bool kSceneDirect = kSceneThread && !env_on("WWHD_VK_SCENE_HELD");
// (stage S6) a second drawing thread for the main geometry run, beside the scene's thread: WWHD_VK_SCENE_TWO=1, with
// WWHD_VK_SCENE_THREAD=1 (S6.1: made, idle)
inline const bool kSceneTwo = kSceneThread && scene_default_on("WWHD_VK_SCENE_TWO");  // (on by default: above)
// (stage S7) the second drawing thread draws the front of the frame's largest two-eye pass while the render thread draws
// the rest of it itself, beside it: WWHD_VK_SCENE_FRONT=1, off by default (not with WWHD_VK_SCENE_THREAD, whose scene
// thread it replaces). WWHD_VK_SCENE_FRONT_SHARE (the thread's first share of the pass, 0.5) and
// WWHD_VK_SCENE_FRONT_MIN_DRAWS (the smallest pass split, 256)
// (each read from the environment itself: an inline constant's initializer may run before another's on Windows,
// where each is a COMDAT of its own, so one built from the others could read them still false)
inline const bool kSceneFront = !scene_default_on("WWHD_VK_SCENE_THREAD") && env_on("WWHD_VK_SCENE_FRONT");
inline const bool kSceneHooks =  // the scene split's hooks run (S1 to S7)
    env_on("WWHD_VK_SCENE_SPLIT") || scene_default_on("WWHD_VK_SCENE_THREAD") || env_on("WWHD_VK_SCENE_FRONT");
// (docs/VR_SCENE_THREADS.md, "S5b in the headset") the scene's thread costs the render thread more than it saves in the
// lighter scenes: it is used only after a frame of at least this many scene draws (WWHD_VK_SCENE_THREAD_MIN_DRAWS,
// 0 = every frame), and dropped again under nine tenths of it
inline uint32_t scene_thread_min_draws() {
    static const uint32_t n = [] {
        const char* e = std::getenv("WWHD_VK_SCENE_THREAD_MIN_DRAWS");
        // (a standalone headset from 700: a Quest 3 player's logs had the scene's thread in only 11 to 64 % of the frames
        // at 1,400, with the render thread over its time in every one; asked for, 2026-11-05: "use the second thread
        // more often". Measured there when it ran: its draws 6.68 ms a frame, the render thread waiting 6.00 ms of it)
#ifdef __ANDROID__
        if (!e && quest_theater::standalone_headset()) return 700u;
#elif defined(WWHD_STEAM_FRAME_NATIVE) || (defined(__linux__) && defined(__aarch64__))
        if (!e) return 700u;  // (Steam Frame - Native, 2026-11-07: as a Quest, its CPU holding the frame rate)
#endif
        return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 1400u;
    }();
    return n;
}
}  // namespace gfxvk::switches

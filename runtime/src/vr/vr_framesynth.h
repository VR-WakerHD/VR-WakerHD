// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR frame synthesis and depth for reprojection (docs/VR_FRAME_SYNTHESIS.md). The OpenXR runtime makes the in-between
// frames itself from each eye's motion vectors and depth while the game draws at half the refresh rate (45 fps at 90
// Hz), and reprojects with depth (head translation, not only rotation) when given depth alone. Backends, by what the
// runtime offers (extension enumeration, never the headset's name):
//   - XR_EXT_frame_synthesis (Meta's runtime on Quest 2/3/3S/Pro, SteamVR on the Steam Frame): per projection view,
//     motion vectors (RGBA16F, Vulkan NDC, current minus previous) and depth with its near/far planes in metres;
//   - XR_FB_space_warp (Meta's Application SpaceWarp, deprecated by the EXT): the same data;
//   - XR_KHR_composition_layer_depth (also SteamVR on PC): depth only, for depth-assisted reprojection.
// The motion vectors come from the motion pass (gfx/vulkan/motion_pass.h, docs/VR_DLSS.md): every depth-writing draw
// and the background drawn again with its vertex shader on both frames' data, so they hold the full 6DoF head pose,
// locomotion, Link and every animated object at each pixel's depth. Pure logic here (vr_test checks it).
#pragma once
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>

namespace vr::framesynth {

// vr.frameSynthesis: auto, off, on
enum class Mode : int { Auto = 0, Off = 1, On = 2 };
enum class Backend : int { None = 0, ExtFrameSynthesis = 1, FbSpaceWarp = 2 };

struct Caps {
    bool ext = false;         // XR_EXT_frame_synthesis offered
    bool fb = false;          // XR_FB_space_warp offered
    bool depth = false;       // XR_KHR_composition_layer_depth offered
    bool motion = true;       // the renderer can make motion vectors (the motion pass: dynamic rendering)
    bool standalone = false;  // the headset's own GPU renders (Android, an arm64 headset)
};

struct Choice {
    Backend backend = Backend::None;
    bool depth = false;  // depth for reprojection on its own (XR_KHR_composition_layer_depth) when no backend
    const char* why = "";
};

// Auto: frame synthesis on a standalone headset whose runtime offers it (the EXT first), depth alone wherever the
// runtime takes it (a PC: SteamVR uses it for reprojection); On: frame synthesis wherever offered; Off: neither
inline Choice choose(Mode mode, const Caps& c) {
    Choice r;
    if (mode == Mode::Off) {
        r.why = "off (vr.frameSynthesis)";
        return r;
    }
    const bool synthesis = (c.ext || c.fb) && c.motion && (mode == Mode::On || c.standalone);
    if (synthesis) {
        r.backend = c.ext ? Backend::ExtFrameSynthesis : Backend::FbSpaceWarp;
        r.why = c.ext ? "XR_EXT_frame_synthesis (motion vectors and depth; the game at half the refresh rate)"
                      : "XR_FB_space_warp (Application SpaceWarp; the game at half the refresh rate)";
        return r;
    }
    if (c.depth) {
        r.depth = true;
        r.why = (c.ext || c.fb) && !c.motion ? "depth only (no motion vectors here: dynamic rendering needed)"
                : (c.ext || c.fb)            ? "depth only (frame synthesis is chosen on standalone headsets; "
                                               "vr.frameSynthesis=on uses it here)"
                                             : "depth only (XR_KHR_composition_layer_depth: depth-assisted reprojection)";
        return r;
    }
    r.why = "off (the runtime takes neither frame synthesis nor depth)";
    return r;
}

// the near and far planes (positive distances, the projection's units) of a camera projection as
// stereo_projection.h camera_projection accepts it (row-major, row 3 = 0 0 -1 0): clip z = p[10] z + p[11].
// halfZ: clip z spans 0..1 (GX2 "HalfZ", DX_CLIP_SPACE_DEF), else -1..1; either way the depth image holds 0 at near
// and 1 at far (draw.cpp maps the viewport's depth range). An infinite far plane (p[10] = -1) gives infinity
inline bool planes(const float* p, bool halfZ, float& nearZ, float& farZ) {
    const float a = p[10], b = p[11];
    if (!(a <= -1.0f) || !(b < 0.0f) || !std::isfinite(a) || !std::isfinite(b)) return false;
    nearZ = halfZ ? b / a : b / (a - 1.0f);
    farZ = a == -1.0f ? std::numeric_limits<float>::infinity() : b / (a + 1.0f);
    return std::isfinite(nearZ) && nearZ > 0.0f && (farZ > nearZ);
}

// the motion pass's target holds (previous - current) * 0.5 of the game's clip-space xy (motion_glsl.cpp); the
// runtimes want current - previous in Vulkan NDC (y down). The game draws through a viewport of negative height
// (clip y up, picture y down: motion::viewport_flips_y), which turns its clip y into Vulkan NDC's -y
inline void motion_scale(bool flipsY, float& x, float& y) {
    x = -2.0f;
    y = flipsY ? 2.0f : -2.0f;
}

// whether the motion work (the motion pass, its shader twins and the previous frame's store) is wanted for frame
// synthesis this run: set by the OpenXR session when it enables a backend, before the renderer's device exists
inline std::atomic<bool>& motion_flag() { static std::atomic<bool> f{false}; return f; }
inline bool motion_wanted() { return motion_flag().load(std::memory_order_relaxed); }

// what the session chose at the start and what the frames carry, for the settings' status line
inline std::mutex& status_mutex() { static std::mutex m; return m; }
inline std::string& status_text() { static std::string s = "not in VR yet (chosen when VR starts)"; return s; }
inline void set_status(const std::string& s) { std::lock_guard<std::mutex> l(status_mutex()); status_text() = s; }
inline std::string status() { std::lock_guard<std::mutex> l(status_mutex()); return status_text(); }

}  // namespace vr::framesynth

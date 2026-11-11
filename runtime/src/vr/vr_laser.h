// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR aiming laser (docs/VR_AIMING.md): a red beam from the item in the sword hand to where the game's own sight
// lands (the bow's, the hookshot's, the boomerang's, the grappling hook's target point), and a dot there: bright with
// a white core when the game would take the target (its sight's lock), dim otherwise. Two OpenXR quad layers in
// tracking space, their picture one small image drawn once (no render pass of the game's).
//
// The game thread sets what is shown (vr_game.cpp update_laser, after each pad read); the render thread uploads the
// picture into the laser's swapchain (vulkan/vr_vulkan.cpp) and the session submits the quads (openxr_session.cpp).
// The beam's and the dot's placement are pure (vr_test.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

#include "vr_math.h"

namespace vr {

// the picture: the beam's strip, the valid target's dot, the invalid one's (pixels)
struct LaserRect { int x, y, w, h; };
constexpr uint32_t kLaserWidth = 96, kLaserHeight = 32;
inline constexpr LaserRect kLaserBeamRect{0, 0, 32, 32}, kLaserValidRect{32, 0, 32, 32}, kLaserInvalidRect{64, 0, 32, 32};

struct LaserView {
    bool visible = false;
    Vec3 from, to;  // tracking space, metres: the item's muzzle, where the sight lands
    bool valid = false;  // the game takes the target (its sight's lock)
    Vec3 eye;       // the head (the beam turns to face it)
};

// game thread: what the laser shows now
void set_laser(const LaserView& v);
// render thread: the laser as last set (false: none)
bool laser_view(LaserView& out);
// the picture (kLaserWidth x kLaserHeight, premultiplied RGBA8, rows top-down; `bgra` swaps red and blue)
const std::vector<uint8_t>& laser_pixels(bool bgra);

// ---- pure parts

constexpr float kLaserWidthMetres = 0.004f;    // the beam's width
constexpr float kLaserDotPerMetre = 0.0001875f;  // the dot's size per metre of distance from the eye (a steady size in
                                                 // view; 1/64 of the first 0.012: a quarter, three times, asked for in the
                                                 // headset)

// the beam's quad from `from` to `to`, facing `eye` as far as it can turn about its own line: its centre, its
// orientation (+Y along the beam, +Z towards the eye) and its length. False when the beam is too short or not finite
inline bool laser_beam_pose(Vec3 from, Vec3 to, Vec3 eye, Pose& pose, float& len) {
    const Vec3 d = to - from;
    len = length(d);
    if (!finite(from) || !finite(to) || !finite(eye) || !(len > 0.01f) || len > 1000.0f) return false;
    const Vec3 y = d * (1.0f / len);
    const Vec3 c = (from + to) * 0.5f;
    Vec3 z = eye - c;
    z = z - y * dot(z, y);  // towards the eye, square to the beam
    if (length(z) < 1e-4f) {  // looking along the beam: any side
        z = std::fabs(y.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        z = z - y * dot(z, y);
    }
    z = z * (1.0f / length(z));
    const Vec3 x = cross(y, z);
    pose = {from_basis(x, y, z), c};
    return true;
}
// the dot's quad at `at`, facing `eye`, its size growing with the distance so it stays a few pixels across
inline bool laser_dot_pose(Vec3 at, Vec3 eye, Pose& pose, float& size) {
    const Vec3 d = eye - at;
    const float dist = length(d);
    if (!finite(at) || !finite(eye) || !(dist > 0.01f) || dist > 1000.0f) return false;
    const Vec3 z = d * (1.0f / dist);
    const Vec3 up = std::fabs(z.y) < 0.95f ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    Vec3 x = cross(up, z);
    x = x * (1.0f / length(x));
    pose = {from_basis(x, cross(z, x), z), at};
    size = std::fmax(0.0000625f, dist * kLaserDotPerMetre);
    return true;
}

}  // namespace vr

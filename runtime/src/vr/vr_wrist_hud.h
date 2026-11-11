// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR wrist HUD (docs/VR_WRIST_HUD.md): the hearts on top of the left wrist, the item buttons underneath it, the
// magic bar on top of the right wrist, the rupees underneath it. Four OpenXR quad layers in tracking space, placed from the controllers' grip
// poses (not Link's animated arm), their picture drawn on the CPU (vr_canvas.h) only when what they show changes:
// the game's own life, magic, rupees and items on X, Y and R, read from the save each update (nothing kept apart from it).
//
// The game thread sets what is shown (vr_game.cpp update_wrist_hud); the render thread draws the picture into the
// quads' swapchain (vulkan/vr_vulkan.cpp) and the session submits the quads (openxr_session.cpp). The placement and the
// heart quarters below are pure (vr_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "vr_math.h"

namespace vr {

// kWristLabel is not on a wrist: the "Free Roam" label of the boat's Free Roam Camera (vr_sail_view.h), red text at the
// top of the view, held in the view (the headset's VIEW space), drawn from the same picture
enum WristPanel { kWristHearts = 0, kWristItems = 1, kWristRupees = 2, kWristMagic = 3, kWristLabel = 4, kWristPanels = 5 };

// the panels' parts of the one picture (pixels)
struct WristRect { int x, y, w, h; };
constexpr uint32_t kWristHudWidth = 512, kWristHudHeight = 560;
inline constexpr WristRect kWristRects[kWristPanels] = {{0, 0, 512, 96}, {0, 96, 512, 176}, {0, 272, 256, 96},
                                                        {0, 496, 512, 64}, {0, 368, 512, 128}};
// the label: this far ahead of the eyes and this far up (metres, VIEW space), this wide (its height the picture's
// aspect). Twice its first size (asked for in the headset, 2026-11-06: 0.36 m wide, 0.30 up, too small to read)
struct LabelPlacement { float ahead = 1.5f, up = 0.40f, width = 0.72f; };
// its text: this much of the label's height (the font's full height, swashes included), smaller if "Free Roam" would
// not fit its width; its strokes this many pixels bolder (0: the font as drawn; bold asked for, 2026-11-06)
constexpr float kLabelTextHeight = 0.80f, kLabelBold = 2.0f;
inline Pose label_pose(const LabelPlacement& p = {}) { return {Quat{}, {0, p.up, -p.ahead}}; }

struct WristHudView {
    bool visible[kWristPanels] = {};
    bool inView[kWristPanels] = {};          // its pose in the headset's VIEW space (the label), not the tracking space
    Pose pose[kWristPanels];                 // tracking space (or VIEW space: inView); the picture faces +Z
    float width[kWristPanels] = {}, height[kWristPanels] = {};  // metres
    // what they show (the picture is drawn again when it changes)
    int life = 0, maxLife = 0;               // quarter hearts, as the save keeps them
    int rupees = 0;
    int magic = 0, maxMagic = 0;             // magic points, as the save keeps them (its maximum 16, 32 doubled; 0 none)
    bool testMode = false;                   // the VR test environment is on: the rupees' panel shows TEST
    int items[3] = {0xFF, 0xFF, 0xFF};       // the item numbers on X, Y, R (0xFF: none)
    int icons[3] = {-1, -1, -1};             // their icon frames (BtnItemIcon_00), -1 none
};

// game thread: what the wrist HUD shows now
void set_wrist_hud(const WristHudView& v);
// render thread: the HUD as last set (false: no panel shown)
bool wrist_hud_view(WristHudView& out);
// render thread: the picture for `v` (kWristHudWidth x kWristHudHeight, premultiplied RGBA8, rows top-down), drawn
// again only when its contents changed; `bgra` swaps red and blue
const std::vector<uint8_t>& wrist_hud_pixels(const WristHudView& v, bool bgra);

// ---- pure parts

// heart `i` (0-based) of `life` quarter hearts: 0 empty .. 4 full
inline int heart_quarters(int life, int i) { return std::clamp(life - 4 * i, 0, 4); }
// the number of hearts `maxLife` quarter hearts make (a heart piece short of a heart does not add one)
inline int heart_count(int maxLife) { return std::clamp(maxLife / 4, 0, 20); }
// the magic bar: its length as a share of the panel, the whole panel once Link has magic (asked for in the headset,
// 2026-11-09: the first meter's half-length bar left half its black box empty), and how much of that length is filled
inline float magic_bar_length(int maxMagic) { return maxMagic > 0 ? 1.0f : 0.0f; }
inline float magic_bar_fill(int magic, int maxMagic) {
    return maxMagic > 0 ? std::clamp(float(magic) / float(maxMagic), 0.0f, 1.0f) : 0.0f;
}

// where a panel goes on its wrist, in the controller's grip space (OpenXR: -Z forward through the fist, +X normal
// to the palm: out of the left palm, into the right one, +Y up through the thumb's side):
//   - along: metres from the grip towards the elbow (+Z), where the wrist is;
//   - lift: metres off the grip's axis, out of the back of the hand (hearts, magic) or out of the palm (items, rupees).
// The panel faces out of that side; its up points along the forearm towards the fingers, so a row on it reads left
// to right when the wrist is turned towards the eyes (both sides right-handed: nothing mirrored)
struct WristPlacement { float along = 0.07f, lift = 0.035f; };
inline Pose wrist_panel_pose(const Pose& grip, WristPanel panel, const WristPlacement& at) {
    // the side it faces in grip space: the back of the left hand (-X), its palm (+X); the back of the right hand (+X),
    // its palm (-X)
    const Vec3 out = panel == kWristHearts || panel == kWristRupees ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
    const Vec3 up{0, 0, -1};          // along the forearm, towards the fingers
    const Vec3 right = cross(up, out);  // x = y x z: a right-handed frame
    Pose p;
    p.orientation = normalize(grip.orientation * from_basis(right, up, out));
    p.position = grip.position + rotate(grip.orientation, Vec3{0, 0, at.along} + out * at.lift);
    return p;
}
// a panel at `at` turned to the eyes and level: it faces the head, its rows horizontal (its x across the view, square to
// the world's up), so its text reads straight however the wrist is rolled. Looking straight down at it, its x is the
// head's right (`headRight`, horizontal)
inline Quat level_facing(Vec3 at, Vec3 head, Vec3 headRight) {
    const Vec3 z = normalize(head - at, {0, 0, 1});
    Vec3 x = cross(Vec3{0, 1, 0}, z);
    if (length(x) < 0.15f) x = headRight - z * dot(headRight, z);  // (nearly) from above
    x = normalize(x, {1, 0, 0});
    return from_basis(x, cross(z, x), z);
}
// whether a panel is shown: its face turned towards the eyes (within 45 degrees to show, 60 to stay) and near them
inline bool wrist_panel_facing(const Pose& panel, Vec3 head, bool wasShown) {
    const Vec3 to = head - panel.position;
    const float d = length(to);
    if (!(d > 1e-4f) || d > 0.9f) return false;
    const float c = dot(rotate(panel.orientation, Vec3{0, 0, 1}), to * (1.0f / d));
    return c > (wasShown ? 0.5f : 0.7071f);
}

}  // namespace vr

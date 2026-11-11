// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR camera and menu helpers (docs/VR_FIXES.md): the gameplay head transform, the menu camera's zoom-out,
// the GamePad panel's placement, the sticks' dead zone and the game's movement angle. Header-only, tested by
// runtime/tools/vr_test.cpp; vr_game.cpp uses them.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include "vr_locomotion.h"
#include "vr_math.h"

namespace vr {

// The head in the game world during normal play: the tracking space anchored at Link's eyes (plus the height
// offset), the head held within maxHeadOffset of the anchor horizontally. This is the transform the first
// play test found correct; the regression test pins its output, and menus never go through it.
inline Pose gameplay_head(WorldMapping& m, Vec3 anchor, const Pose& head, float heightOffset, float maxHeadOffset,
                          float unitsPerMetre) {
    m.unitsPerMetre = unitsPerMetre;
    m.anchor = anchor + Vec3{0, heightOffset * unitsPerMetre, 0};
    const Vec3 offset = limit_head_offset(head.position - m.origin, maxHeadOffset);
    return {to_game_orientation(m, head.orientation), to_game_point(m, m.origin + offset)};
}

// (VR fork) leaning back: the view at most `limit` game units behind his eyes along the way he faces (`heading`, the
// facing his drawn model has this frame: blend_heading of the two steps', so the limit turns with him every frame, not
// in 30 Hz steps). Forward and to the sides nothing changes. Returns whether the view was held
inline bool hold_lean_back(Vec3& view, Vec3 eyes, float heading, float limit) {
    const Vec3 fwd = heading_dir(heading);
    const float back = (view.x - eyes.x) * fwd.x + (view.z - eyes.z) * fwd.z;
    if (!(back < -limit)) return false;
    view = view + fwd * (-limit - back);
    return true;
}

// (VR fork) the view never under the ground Link stands on: at least `floor` high (his feet plus kViewAboveFeet). Only
// sideways was the head held; a PSVR2 that lost the room put the head 1.25 m lower, and the view sank through the floor
// (2026-10-31). A real head goes that low only lying on the floor. Returns whether the view was held
constexpr float kViewAboveFeet = 15.0f;  // game units (about 15 cm)
inline bool hold_above_floor(Vec3& view, float floor) {
    if (!(view.y < floor)) return false;
    view.y = floor;
    return true;
}

// (VR fork) in the water (his feet under its surface, swimming or wading), the lowest the view may be is also
// kViewAboveWater over the surface: swimming, his eyes are at the surface, and the view sat in it or dipped under it,
// where the game draws nothing (black). `water` beyond kWaterReach above his feet is not his water (a stale or unset
// mWaterY), nor is a surface under his feet
constexpr float kViewAboveWater = 10.0f;  // game units (about 10 cm)
constexpr float kWaterReach = 150.0f;     // game units: over his eyes' height (about 110), a surface he swims at
inline float above_water(float floor, float feet, float water) {
    if (!std::isfinite(feet) || !std::isfinite(water) || !(water > feet) || water - feet > kWaterReach) return floor;
    return std::max(floor, water + kViewAboveWater);
}

// The point the stick's turn turns Link's drawn body about between steps (docs/VR_CAMERA_AUDIT.md, "One pivot"): the
// view turns about the tracking origin, which stands at his eye point (to_game_point: the head's offset from the origin
// is what turns), so his body turns about his eye point too, horizontally; turned about his feet, his eyes, about 20
// units in front of them, left the view by that times the turn until the next step put them back
inline Vec3 turn_pivot(Vec3 feet, Vec3 eyeOffset, bool haveEyes) {
    return haveEyes && finite(eyeOffset) ? Vec3{feet.x + eyeOffset.x, feet.y, feet.z + eyeOffset.z} : feet;
}


// The screen blur in the headset (docs/VR_SCREEN_EFFECTS.md). The game lays the last frame's picture over the new one
// through a matrix on the picture's texture coordinates (u right, v down, 0..1; its centre 0.5, 0.5: dCamera_c
// ResetBlure), a slight zoom. The last frame was drawn where the head looked then, so laid over as it is it stays with
// the head and the world doubles while the head turns. This maps a coordinate of the new frame to where the same
// direction was in the last one (the view's turn between them; `f` the field of view both were drawn with), as
// u' = a[0] u + a[1] v + a[2], v' = a[3] u + a[4] v + a[5]: the map's value and slope at the middle, where you look.
// A wide view's projection is not affine, so towards the edges it moves the picture less than it moved (never past
// it); an exact map needs the pass's shader (docs/VR_SCREEN_EFFECTS.md). False for a turn too large to be one
// frame's (a cut): the game's matrix is then used as it is
inline bool blur_reprojection(Quat prevView, Quat curView, const Fov& f, float a[6]) {
    const float tx = std::tan(f.right), ty = std::tan(f.up);
    if (!(tx > 0) || !(ty > 0) || !(angle_between(prevView, curView) < 0.35f)) return false;
    const Quat rel = normalize(conjugate(prevView) * curView);
    bool ok = true;
    const auto map = [&](float u, float v, float& ou, float& ov) {
        const Vec3 d = rotate(rel, Vec3{(2 * u - 1) * tx, (1 - 2 * v) * ty, -1});
        if (!(d.z < -0.1f)) { ok = false; return; }
        ou = (d.x / -d.z / tx + 1) * 0.5f;
        ov = (1 - d.y / -d.z / ty) * 0.5f;
    };
    constexpr float h = 0.02f;
    float u0 = 0, v0 = 0, uu = 0, vu = 0, uv = 0, vv = 0;
    map(0.5f, 0.5f, u0, v0);
    map(0.5f + h, 0.5f, uu, vu);
    map(0.5f, 0.5f + h, uv, vv);
    if (!ok) return false;
    a[0] = (uu - u0) / h; a[1] = (uv - u0) / h;
    a[3] = (vu - v0) / h; a[4] = (vv - v0) / h;
    a[2] = u0 - 0.5f * (a[0] + a[1]);
    a[5] = v0 - 0.5f * (a[3] + a[4]);
    for (int i = 0; i < 6; ++i) if (!std::isfinite(a[i])) return false;
    return true;
}
// the game's 3x4 texture matrix (rows, as in guest memory) applied after the reprojection: m * T, T the affine map of
// blur_reprojection on (u, v), its other inputs left as they are
inline void blur_compose(float m[12], const float a[6]) {
    for (int r = 0; r < 3; ++r) {
        float* row = m + 4 * r;
        const float c0 = row[0], c1 = row[1];
        row[0] = c0 * a[0] + c1 * a[3];
        row[1] = c0 * a[1] + c1 * a[4];
        row[3] = c0 * a[2] + c1 * a[5] + row[3];
    }
}

// How far Link's eyes are in front of his head joint (game units), from the game's own eye point (eyePos) in his
// frame (x forward, y right, z up from the head joint): taken only while his head faces forward (the eye point well in
// front and not off to a side; he turns his head towards things he notices, and then it is not his eyes' place in
// his body). His head is a large one: the head joint is at its back, near the neck, and the eyes about 18 units ahead.
inline bool eye_forward_sample(Vec3 eyeFromHead, float& forward) {
    if (!std::isfinite(eyeFromHead.x) || !std::isfinite(eyeFromHead.y)) return false;
    if (eyeFromHead.x < 8.0f || eyeFromHead.x > 32.0f || std::fabs(eyeFromHead.y) > 6.0f) return false;
    forward = eyeFromHead.x;
    return true;
}

// Which camera the frame gets: gameplay (the head at Link's eyes), a menu scene (title, file select) or an
// event (cutscenes: the game's camera with the head's rotation).
enum class CameraKind { Gameplay, Menu, Event };
inline bool menu_stage(const char* stage) {
    return stage && (!std::strncmp(stage, "sea_T", 8) || !std::strncmp(stage, "Name", 8));
}
inline CameraKind camera_kind(bool gameplay, const char* stage) {
    if (gameplay) return CameraKind::Gameplay;
    return menu_stage(stage) ? CameraKind::Menu : CameraKind::Event;
}

// A menu camera zoomed out: the eye moved away from its target along the same line (zoom 1.5 = 50 % farther)
inline Vec3 menu_eye(Vec3 eye, Vec3 center, float zoom) { return center + (eye - center) * zoom; }

// The GamePad panel in the tracking space: straight ahead of where the head looks horizontally, `distance`
// away, `drop` below eye level, upright and facing the head (an OpenXR quad faces its +Z)
inline Pose panel_pose(const Pose& head, float distance, float drop) {
    const Quat yaw = yaw_quat(yaw_of(head.orientation));
    return {yaw, head.position + rotate(yaw, Vec3{0, 0, -distance}) + Vec3{0, -drop, 0}};
}

// the menu panel's width at vr.menuScale 1 (metres)
constexpr float kMenuBaseWidth = 0.8f;

// A radial dead zone: nothing inside `zone`, the rest rescaled to start at 0 (no jump at the edge), never longer
// than 1 (diagonals are not faster)
inline Stick dead_zone(Stick s, float zone) {
    const float len = std::sqrt(s.x * s.x + s.y * s.y);
    if (!(len > zone) || !std::isfinite(len)) return {};
    const float out = std::fmin(1.0f, (len - zone) / std::fmax(1e-3f, 1.0f - zone));
    return {s.x / len * out, s.y / len * out};
}

// dCam_getControledAngleY's value for a heading (radians, game convention). The game adds the stick's angle to
// it; `offset` is the constant between the game's angle and the camera's view heading, measured while the
// stick is released (snap_angle_offset).
inline int16_t controlled_angle(float heading, int16_t offset) {
    return int16_t(uint16_t(radians_to_s16(heading)) + uint16_t(offset));
}
// a measured offset is a quarter turn multiple (0, 0x4000, 0x8000, 0xC000) give or take the camera's easing:
// snap to it when close, keep the measurement otherwise
inline int16_t snap_angle_offset(int16_t measured) {
    const int q = int(std::lround(double(uint16_t(measured)) / 16384.0)) & 3;
    const int16_t snapped = int16_t(uint16_t(q * 0x4000));
    const int16_t diff = int16_t(uint16_t(measured) - uint16_t(snapped));
    return std::abs(int(diff)) <= 0x0800 ? snapped : measured;
}

// The zoom of the game's first-person subject camera in VR (the telescope, the Picto Boxes: docs/VR_TELESCOPE.md,
// docs/VR_PICTO_BOX.md): the headset's field of view replaces the camera's, so the game's narrowing shows as a
// magnification of both eyes' fields of view: the ratio of the tangents of the normal view's field of view (degrees)
// and the camera's, 1 to 16 (no shrinking below the headset's field; 16 is past either item's own limit)
inline float zoom_magnification(float normalFovy, float gameFovy) {
    if (!(normalFovy > 1.0f && normalFovy < 170.0f) || !(gameFovy > 1.0f && gameFovy < 170.0f)) return 1.0f;
    const float mag = std::tan(normalFovy * 0.5f * kPi / 180.0f) / std::tan(gameFovy * 0.5f * kPi / 180.0f);
    return std::isfinite(mag) ? std::fmin(std::fmax(mag, 1.0f), 16.0f) : 1.0f;
}

// Aiming by pointing (docs/VR_AIMING.md): dCamera_c::CalcSubjectAngle's pitch and yaw (s16) for a pointing direction
// in the game world. The yaw is the direction's heading (the game's convention: 0 along +Z); the pitch is the negative
// of its elevation (measured: the stick's -1 pitch input, -12743, looks up 70 degrees), held within the game's limit
// for the item (`pitchLimit`, its own output's range). False for a direction that is not finite or has no length
inline bool pointing_aim(Vec3 dir, int pitchLimit, int16_t& pitch, int16_t& yaw) {
    if (!finite(dir) || length(dir) < 1e-6f) return false;
    const float horizontal = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    yaw = radians_to_s16(std::atan2(dir.x, dir.z));
    const int p = -int(radians_to_s16(std::atan2(dir.y, horizontal)));
    const int limit = std::clamp(pitchLimit, 0, 0x3F00);
    pitch = int16_t(std::clamp(p, -limit, limit));
    return true;
}

}  // namespace vr

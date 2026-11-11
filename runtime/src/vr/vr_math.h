// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// VR math (docs/VR_ARCHITECTURE.md): vectors, quaternions and poses, and the conversion between the
// OpenXR tracking space and the game's world.
//
// Frames:
//   OpenXR space   metres, right-handed, +Y up, -Z forward (the LOCAL / STAGE reference space)
//   game world     game units (about 1 cm, VrConfig::unitsPerMetre), right-handed, +Y up. An actor with
//                  the s16 heading 0 faces +Z; heading a faces (sin a, 0, cos a) (cM_ssin / cM_scos)
//   game view      as OpenXR: +X right, +Y up, -Z forward (GX2 projections divide by -z)
// An OpenXR direction maps to the game by a turn about +Y: forward (0,0,-1) is +Z at heading 0, so the
// whole conversion is one yaw of (heading + pi). Everything here is header-only so runtime/tools/vr_test.cpp
// tests it without a headset.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vr {

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v, Vec3 fallback = {0, 0, 1}) {
    const float l = length(v);
    return l > 1e-6f ? v * (1.0f / l) : fallback;
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// unit quaternion (x, y, z, w), as XrQuaternionf
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};
inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conjugate(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat normalize(Quat q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-6f) || !std::isfinite(l)) return {};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
inline Quat axis_angle(Vec3 axis, float radians) {
    const Vec3 a = normalize(axis);
    const float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}
inline Quat yaw_quat(float radians) { return axis_angle({0, 1, 0}, radians); }
inline Vec3 rotate(Quat q, Vec3 v) {
    // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}
// rotation about +Y of q's forward (-Z) direction projected onto the floor; heading convention of the
// OpenXR frame: 0 looks along -Z, positive turns left (towards -X), as a right-handed turn about +Y
inline float yaw_of(Quat q) {
    const Vec3 f = rotate(q, {0, 0, -1});
    return std::atan2(-f.x, -f.z);
}
// q without its yaw (the head's pitch and roll relative to the floor-projected heading)
inline Quat remove_yaw(Quat q) { return yaw_quat(-yaw_of(q)) * q; }
inline Quat slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f)
        return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    const float th = std::acos(std::clamp(d, -1.0f, 1.0f)), s = std::sin(th);
    const float ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
    return {a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb};
}
// the rotation whose columns are the orthonormal axes x, y, z (Shepperd's method: the largest diagonal term
// picks the stable formula)
inline Quat from_basis(Vec3 x, Vec3 y, Vec3 z) {
    const float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return normalize(q);
}
// the orientation of a camera looking along `forward` (view convention: -Z forward, +Y up)
inline Quat look_rotation(Vec3 forward, Vec3 up) {
    const Vec3 z = -normalize(forward);
    Vec3 x = cross(up, z);
    x = length(x) > 1e-6f ? normalize(x) : normalize(cross(Vec3{0, 0, 1}, z), {1, 0, 0});
    return from_basis(x, cross(z, x), z);
}
// smallest angle between two orientations (radians)
inline float angle_between(Quat a, Quat b) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0f * std::acos(std::clamp(d, 0.0f, 1.0f));
}

struct Pose {
    Quat orientation;
    Vec3 position;
};
inline Vec3 transform(const Pose& p, Vec3 v) { return rotate(p.orientation, v) + p.position; }
inline Pose compose(const Pose& parent, const Pose& child) {
    return {normalize(parent.orientation * child.orientation), transform(parent, child.position)};
}
inline Pose inverse(const Pose& p) {
    const Quat c = conjugate(p.orientation);
    return {c, rotate(c, -p.position)};
}
// An eye as the game draws it (vr::as_rendered, docs/VR_CAMERA_AUDIT.md): the head's orientation, `eyeX` metres along
// the head's right. Both eyes come from one head pose, so they are always one snapshot
inline Pose eye_pose(const Pose& head, float eyeX) {
    return {head.orientation, head.position + rotate(head.orientation, Vec3{eyeX, 0, 0})};
}

// ---- s16 game angles (0x8000 = 180 degrees)
inline float s16_to_radians(int16_t a) { return float(a) * (kPi / 32768.0f); }
inline int16_t radians_to_s16(float r) {
    const float turns = r / (2.0f * kPi);
    const float wrapped = turns - std::floor(turns + 0.5f);  // -0.5 .. 0.5
    return int16_t(std::lround(wrapped * 65536.0f) & 0xFFFF);
}
inline float wrap_radians(float r) {
    const float t = std::fmod(r + kPi, 2.0f * kPi);
    return (t < 0 ? t + 2.0f * kPi : t) - kPi;
}
// a heading between two steps' headings at the fraction t, the short way round (blended as Link's model is)
inline float blend_heading(float from, float to, float t) { return wrap_radians(from + wrap_radians(to - from) * t); }
// heading of a game direction (the angle an actor facing it would have)
inline float game_heading(Vec3 dir) { return std::atan2(dir.x, dir.z); }
inline Vec3 heading_dir(float heading) { return {std::sin(heading), 0, std::cos(heading)}; }

// ---- OpenXR tracking space -> game world
// The anchor is the point of the game world the calibrated head origin stands at (Link's eyes), the
// heading the game heading the tracking space's -Z faces there, and unitsPerMetre the scale.
struct WorldMapping {
    Vec3 anchor;          // game units
    float heading = 0;    // radians, game heading convention
    float unitsPerMetre = 100;
    Vec3 origin;          // OpenXR metres: the tracked head position the anchor stands for (recentre)
};
inline Quat game_rotation(const WorldMapping& m) { return yaw_quat(m.heading + kPi); }
inline Vec3 to_game_dir(const WorldMapping& m, Vec3 xrDir) { return rotate(game_rotation(m), xrDir); }
inline Vec3 to_game_point(const WorldMapping& m, Vec3 xrPoint) {
    return m.anchor + to_game_dir(m, xrPoint - m.origin) * m.unitsPerMetre;
}
inline Quat to_game_orientation(const WorldMapping& m, Quat xr) { return normalize(game_rotation(m) * xr); }
inline Pose to_game_pose(const WorldMapping& m, const Pose& xr) {
    return {to_game_orientation(m, xr.orientation), to_game_point(m, xr.position)};
}
inline Vec3 to_xr_point(const WorldMapping& m, Vec3 game) {
    return rotate(conjugate(game_rotation(m)), (game - m.anchor) * (1.0f / m.unitsPerMetre)) + m.origin;
}

// Look-at the game's camera takes (eye, center, up: dCamera_c / camera_class at +0xDC, +0xE8, +0xF4)
struct LookAt {
    Vec3 eye, center, up;
};
inline LookAt look_at(const Pose& gameHead, float distance) {
    const Vec3 fwd = rotate(gameHead.orientation, {0, 0, -1});
    const Vec3 up = rotate(gameHead.orientation, {0, 1, 0});
    return {gameHead.position, gameHead.position + fwd * distance, up};
}

// ---- projection (GX2 row-major perspective, clip = P * view, clip.w = -z)
// Field of view as XrFovf: angles in radians, left and down negative.
struct Fov {
    float left = -0.9f, right = 0.9f, up = 0.9f, down = -0.9f;
};
// the smallest symmetric field of view that contains both eyes' fields of view
inline Fov symmetric_union(const Fov& a, const Fov& b) {
    const float h = std::max({std::fabs(a.left), std::fabs(a.right), std::fabs(b.left), std::fabs(b.right)});
    const float v = std::max({std::fabs(a.up), std::fabs(a.down), std::fabs(b.up), std::fabs(b.down)});
    return {-h, h, v, -v};
}
// a field of view widened by `margin` radians on every side (each edge at most just under 90 degrees)
inline Fov widen(const Fov& f, float margin) {
    constexpr float kMax = 1.55f;
    return {std::max(f.left - margin, -kMax), std::min(f.right + margin, kMax), std::min(f.up + margin, kMax),
            std::max(f.down - margin, -kMax)};
}
// The part of an image drawn with field of view `drawn` (width x height pixels, row 0 at the top) that shows `wanted`
// (the same eye pose, a field of view inside it): its pixels, rounded outwards, and the exact field of view of those
// pixels. An eye submitted so matches the display's own projection (a runtime need not reproject a symmetric image to
// an asymmetric display; SteamVR on Linux did that wrongly: docs/VR_MENU.md, "Double vision on Linux")
struct FovCrop {
    int x = 0, y = 0, width = 0, height = 0;
    Fov fov;
};
inline FovCrop fov_crop(const Fov& drawn, const Fov& wanted, int width, int height) {
    const float l = std::tan(drawn.left), r = std::tan(drawn.right), u = std::tan(drawn.up), d = std::tan(drawn.down);
    const auto column = [&](float a) { return (std::tan(a) - l) / (r - l) * float(width); };
    const auto row = [&](float a) { return (u - std::tan(a)) / (u - d) * float(height); };
    const int x0 = std::clamp(int(std::floor(column(wanted.left) + 1e-3f)), 0, width);
    const int x1 = std::clamp(int(std::ceil(column(wanted.right) - 1e-3f)), x0, width);
    const int y0 = std::clamp(int(std::floor(row(wanted.up) + 1e-3f)), 0, height);
    const int y1 = std::clamp(int(std::ceil(row(wanted.down) - 1e-3f)), y0, height);
    FovCrop c{x0, y0, x1 - x0, y1 - y0, {}};
    c.fov.left = std::atan(l + (r - l) * float(x0) / float(width));
    c.fov.right = std::atan(l + (r - l) * float(x1) / float(width));
    c.fov.up = std::atan(u - (u - d) * float(y0) / float(height));
    c.fov.down = std::atan(u - (u - d) * float(y1) / float(height));
    return c;
}
// (docs/VR_EYE_PROJECTION.md) an eye's own field of view `own` inside the symmetric `drawn` one, as the scale and offset
// of the projection's x and y rows (gfx/stereo_projection.h EyeFrustum): the eye's edges' tangents in units of the
// drawn half-field l, r, u, d give sx = 2 / (r - l), ox = (r + l) / (r - l), sy = 2 / (u - d), oy = (u + d) / (u - d)
struct EyeFrustumXY {
    float sx = 1, ox = 0, sy = 1, oy = 0;
};
inline EyeFrustumXY eye_frustum(const Fov& drawn, const Fov& own) {
    const float h = std::tan(drawn.right), v = std::tan(drawn.up);
    if (!(h > 0) || !(v > 0)) return {};
    const float l = std::tan(own.left) / h, r = std::tan(own.right) / h, u = std::tan(own.up) / v, d = std::tan(own.down) / v;
    if (!(r > l) || !(u > d)) return {};
    return {2.0f / (r - l), (r + l) / (r - l), 2.0f / (u - d), (u + d) / (u - d)};
}
// an image's shape (width / height) for a field of view drawn into it at square pixels: its tangent extents' ratio
inline float fov_image_aspect(const Fov& f) {
    const float w = std::tan(f.right) - std::tan(f.left), hgt = std::tan(f.up) - std::tan(f.down);
    return hgt > 0 ? w / hgt : 1.0f;
}
// vertical field of view in degrees and aspect (tan horizontal / tan vertical) of a symmetric Fov, as the
// game's C_MTXPerspective(fovy, aspect, ...) takes them
inline float fovy_degrees(const Fov& f) { return 2.0f * f.up * 180.0f / kPi; }
inline float fov_aspect(const Fov& f) { return std::tan(f.right) / std::tan(f.up); }
// rows x and y of the perspective matrix for an (asymmetric) field of view: p[0], p[2] and p[5], p[6]
struct ProjectionXY {
    float p0, p2, p5, p6;
};
inline ProjectionXY projection_xy(const Fov& f) {
    const float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
    return {2.0f / (r - l), (r + l) / (r - l), 2.0f / (u - d), (u + d) / (u - d)};
}
// normalised device x of a view-space point under rows x of projection_xy (tests, debug views)
inline float ndc_x(const ProjectionXY& p, Vec3 view) { return (p.p0 * view.x + p.p2 * view.z) / -view.z; }
inline float ndc_y(const ProjectionXY& p, Vec3 view) { return (p.p5 * view.y + p.p6 * view.z) / -view.z; }

// ---- filters
// Critically damped follow of a value towards a target, frame-rate independent (time constant tau, s).
inline float follow(float current, float target, float dt, float tau) {
    if (!(tau > 0) || !(dt > 0)) return target;
    return target + (current - target) * std::exp(-dt / tau);
}
inline Vec3 follow(Vec3 current, Vec3 target, float dt, float tau) {
    return {follow(current.x, target.x, dt, tau), follow(current.y, target.y, dt, tau), follow(current.z, target.z, dt, tau)};
}

// A value that holds still while its target only sways within `deadZone` (an idle animation's breathing), and follows
// it (time constant tau) once the target leaves the dead zone (a change of posture), until it has caught up
template <class T> struct Settled {
    T value{};
    bool moving = false;
    void reset(T v) { value = v; moving = false; }
    void update(T target, float deadZone, float dt, float tau) {
        if (!moving && distance(target) > deadZone) moving = true;
        if (!moving) return;
        value = follow(value, target, dt, tau);
        if (distance(target) < deadZone * 0.1f) moving = false;
    }
private:
    float distance(float t) const { return std::fabs(t - value); }
    float distance(Vec3 t) const { return length(t - value); }
};

}  // namespace vr

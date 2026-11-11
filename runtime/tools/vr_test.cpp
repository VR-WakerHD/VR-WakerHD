// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR calculations without a headset (runtime/src/vr/*.h): OpenXR <-> game conversion, projections,
// configuration parsing, bindings, locomotion, arm IK reach, swing detection and the swept blade.
#include "interp_pacing.h"
#include "vr/vr_arm.h"
#include "vr/vr_baton.h"
#include "vr/vr_tracking.h"
#include "vr/vr_ui.h"
#include "vr/vr_wrist_hud.h"
#include "vr/vr_laser.h"
#include "vr/vr_test_env.h"
#include "vr/vr_bow.h"
#include "vr/vr_sword.h"
#include "vr/vr_bindings.h"
#include "vr/vr_upscale.h"
#include "vr/vr_framesynth.h"
#include "vr/vr_camera.h"
#include "vr/vr_head.h"
#include "vr/vr_config.h"
#include "vr/vr_ik.h"
#include "vr/vr_locomotion.h"
#include "vr/vr_math.h"
#include "vr/vr_settings_file.h"
#include <filesystem>
#include "vr/vr_swing.h"
#include "vr/vr_item_wheel.h"
#include "vr/vr_sail_view.h"
#include "vr/vr_bud_view.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

using namespace vr;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return length(a - b) <= eps; }

static void test_conversion() {
    // forward in OpenXR (-Z) is the game heading's direction; heading 0 faces +Z
    WorldMapping m;
    m.unitsPerMetre = 100;
    CHECK(near(to_game_dir(m, {0, 0, -1}), {0, 0, 1}));
    CHECK(near(to_game_dir(m, {1, 0, 0}), heading_right(0)));
    CHECK(near(to_game_dir(m, {0, 1, 0}), {0, 1, 0}));
    m.heading = kPi / 2;  // facing +X
    CHECK(near(to_game_dir(m, {0, 0, -1}), {1, 0, 0}));
    CHECK(near(to_game_dir(m, {1, 0, 0}), heading_right(kPi / 2)));
    // points: anchor + scaled offset from the calibrated origin
    m.anchor = {500, 100, -300};
    m.origin = {0.2f, 1.6f, 0.1f};
    CHECK(near(to_game_point(m, m.origin), m.anchor));
    CHECK(near(to_game_point(m, m.origin + Vec3{0, 0, -1}), m.anchor + Vec3{100, 0, 0}, 1e-3f));
    CHECK(near(to_game_point(m, m.origin + Vec3{0, 0.5f, 0}), m.anchor + Vec3{0, 50, 0}, 1e-3f));
    const Vec3 p{0.7f, 1.1f, -0.4f};
    CHECK(near(to_xr_point(m, to_game_point(m, p)), p, 1e-4f));
    // orientation: a head turned 90 degrees left in OpenXR looks to the game heading + 90 degrees
    m.heading = 0;
    const Quat left = yaw_quat(kPi / 2);
    CHECK(near(rotate(to_game_orientation(m, left), {0, 0, -1}), heading_dir(kPi / 2)));
    CHECK(near(yaw_of(left), kPi / 2));
    CHECK(near(yaw_of(remove_yaw(left * axis_angle({1, 0, 0}, 0.3f))), 0, 1e-4f));
    // look rotation: the inverse of look_at's directions, for any orientation
    CHECK(angle_between(look_rotation({0, 0, -1}, {0, 1, 0}), Quat{}) < 1e-3f);
    for (const Quat q : {axis_angle({0, 1, 0}, 2.5f), axis_angle(normalize(Vec3{1, 2, -0.5f}), -1.1f),
                         axis_angle({1, 0, 0}, 3.1f), axis_angle({0, 0, 1}, 1.6f)}) {
        const Quat r = look_rotation(rotate(q, {0, 0, -1}), rotate(q, {0, 1, 0}));
        CHECK(angle_between(r, q) < 1e-3f);
    }
    // look-at the game camera takes
    const LookAt la = look_at(Pose{{}, {1, 2, 3}}, 100);
    CHECK(near(la.eye, {1, 2, 3}) && near(la.center, {1, 2, -97}) && near(la.up, {0, 1, 0}));
    // pose composition and inverse
    const Pose a{axis_angle({0, 1, 0}, 0.4f), {1, 2, 3}}, b{axis_angle({1, 0, 0}, -0.2f), {0.5f, 0, -1}};
    const Pose ab = compose(a, b);
    CHECK(near(transform(ab, {0.3f, 0.2f, 0.1f}), transform(a, transform(b, {0.3f, 0.2f, 0.1f}))));
    CHECK(near(transform(compose(inverse(a), a), {4, 5, 6}), {4, 5, 6}));
}

static void test_angles() {
    CHECK(radians_to_s16(0) == 0);
    CHECK(radians_to_s16(kPi / 2) == 0x4000);
    CHECK(radians_to_s16(-kPi / 2) == int16_t(0xC000));
    CHECK(radians_to_s16(3 * kPi / 2) == int16_t(0xC000));  // wraps
    CHECK(near(s16_to_radians(0x2000), kPi / 4));
    CHECK(near(wrap_radians(3 * kPi), kPi, 1e-4f) || near(wrap_radians(3 * kPi), -kPi, 1e-4f));
    CHECK(near(game_heading(heading_dir(1.0f)), 1.0f));
}

static void test_projection() {
    // a typical asymmetric eye: the edges of its field of view land on the edges of the picture
    const Fov f{-0.95f, 0.75f, 0.85f, -0.9f};
    const ProjectionXY p = projection_xy(f);
    const float z = -2.0f;
    CHECK(near(ndc_x(p, {std::tan(f.right) * -z, 0, z}), 1.0f));
    CHECK(near(ndc_x(p, {std::tan(f.left) * -z, 0, z}), -1.0f));
    CHECK(near(ndc_y(p, {0, std::tan(f.up) * -z, z}), 1.0f));
    CHECK(near(ndc_y(p, {0, std::tan(f.down) * -z, z}), -1.0f));
    // symmetric: no shift column
    const ProjectionXY s = projection_xy(Fov{-0.8f, 0.8f, 0.7f, -0.7f});
    CHECK(near(s.p2, 0) && near(s.p6, 0));
    // union of the two eyes, and the game's fovy / aspect for it
    const Fov l{-0.95f, 0.75f, 0.85f, -0.9f}, r{-0.75f, 0.95f, 0.85f, -0.9f};
    const Fov u = symmetric_union(l, r);
    CHECK(near(u.left, -0.95f) && near(u.right, 0.95f) && near(u.up, 0.9f) && near(u.down, -0.9f));
    CHECK(near(fovy_degrees(u), 2 * 0.9f * 180 / kPi, 1e-3f));
    CHECK(near(fov_aspect(u), std::tan(0.95f) / std::tan(0.9f)));
    // each eye submitted as its own field of view: the part of the symmetric image that shows it, exactly
    for (const Fov& eye : {l, r}) {
        const FovCrop c = fov_crop(u, eye, 3400, 3468);
        CHECK(c.width > 0 && c.width < 3400 && c.height > 0 && c.height < 3468);  // (up 0.85 of the union's 0.9)
        CHECK(c.fov.left <= eye.left + 1e-6f && c.fov.right >= eye.right - 1e-6f);   // covers it (outward rounding)
        CHECK(c.fov.up >= eye.up - 1e-6f && c.fov.down <= eye.down + 1e-6f);
        CHECK(near(c.fov.left, eye.left, 1e-3f) && near(c.fov.right, eye.right, 1e-3f));  // within a pixel
        CHECK(near(c.fov.up, eye.up, 1e-3f) && near(c.fov.down, eye.down, 1e-3f));
        CHECK(c.y + c.height == 3468);  // down -0.9: the union's bottom
        // the reported field of view is that of the pixels: tangents linear across the image
        const float t0 = std::tan(u.left), t1 = std::tan(u.right);
        CHECK(near(std::tan(c.fov.left), t0 + (t1 - t0) * float(c.x) / 3400.0f, 1e-5f));
        CHECK(near(std::tan(c.fov.right), t0 + (t1 - t0) * float(c.x + c.width) / 3400.0f, 1e-5f));
    }
    CHECK(fov_crop(u, l, 3400, 3468).x == 0 && fov_crop(u, r, 3400, 3468).x + fov_crop(u, r, 3400, 3468).width == 3400);
    // the same field of view: the whole image
    const FovCrop whole = fov_crop(u, u, 100, 80);
    CHECK(whole.x == 0 && whole.y == 0 && whole.width == 100 && whole.height == 80);
    // the margin (vr::fov_margin): drawn wider on every side, each eye submitted with the margin beyond its own field of
    // view, so there is picture past the lens's edges (no edge of the eye's crop at the image's edge any more)
    const float m = 5.0f * kPi / 180.0f;
    const Fov wide = widen(u, m);
    CHECK(near(wide.left, u.left - m) && near(wide.right, u.right + m) && near(wide.up, u.up + m) && near(wide.down, u.down - m));
    for (const Fov& eye : {l, r}) {
        const FovCrop plain = fov_crop(wide, eye, 3400, 3468), margin = fov_crop(wide, widen(eye, m), 3400, 3468);
        CHECK(plain.x > 0 && plain.x + plain.width < 3400 && plain.y + plain.height < 3468);  // the eye's own: inside
        CHECK(margin.width > plain.width && margin.height > plain.height);
        CHECK(near(margin.fov.left, eye.left - m, 1e-3f) && near(margin.fov.down, eye.down - m, 1e-3f));
    }
    CHECK(widen(Fov{-1.5f, 1.5f, 1.5f, -1.5f}, 1.0f).right < 1.5708f);  // (never 90 degrees or more)
}

// the shield mirrored onto the other hand: flipped on the axis that is neither its face (across the body: the crest
// turned to the body) nor its up (upside down)
static void test_shield_flip() {
    CHECK(mirror_flip_axis(0, 1) == 2 && mirror_flip_axis(2, 1) == 0 && mirror_flip_axis(1, 2) == 0);
    CHECK(mirror_flip_axis(1, 1) != 1);  // a degenerate up: still not the face
    // the up axis: the one most along world +Y, never the face
    CHECK(up_axis(Quat{}, 0) == 1 && up_axis(Quat{}, 1) != 1);
    CHECK(up_axis(axis_angle({1, 0, 0}, kPi / 2), 0) == 2);  // its z turned up
}

// the shield in the off hand turned by its controller (docs/VR_HELD_ITEMS.md, "The shield's orientation")
static void test_shield_orientation() {
    // its axes from the game's poses: face the most outward, top the most up of the others, with their signs; a few
    // odd poses (an attack's swing of the hand) do not change them
    ShieldAxes a = shield_axes({0.1f, -25.0f, 3.0f}, {2.0f, 4.0f, 27.0f});
    CHECK(a.valid() && a.face == 1 && a.faceSign < 0 && a.top == 2 && a.topSign > 0 && a.side() == 0);
    a = shield_axes(Vec3{0.1f, -25.0f, 3.0f} + Vec3{0.9f, 0.1f, -0.9f} * 3, Vec3{2.0f, 4.0f, 27.0f} + Vec3{0, 1, -1} * 3);
    CHECK(a.face == 1 && a.faceSign < 0 && a.top == 2 && a.topSign > 0);
    a = shield_axes({0, 0, 5}, {0, 5, 0});  // its top competing with its face: the face first
    CHECK(a.face == 2 && a.top == 1);
    // a rotation with the face and top where asked, for every axis choice and sign
    for (int face = 0; face < 3; ++face)
        for (int top = 0; top < 3; ++top) {
            if (face == top) continue;
            for (float fs : {-1.0f, 1.0f})
                for (float ts : {-1.0f, 1.0f}) {
                    ShieldAxes s;
                    s.face = face; s.top = top; s.faceSign = fs; s.topSign = ts;
                    const Vec3 f = normalize(Vec3{0.3f, -0.2f, 0.9f}), t = normalize(Vec3{0.1f, 1.0f, 0.3f});
                    const Quat q = shield_rotation(s, f, t);
                    auto e = [](int i, float sign) { return Vec3{i == 0 ? sign : 0, i == 1 ? sign : 0, i == 2 ? sign : 0}; };
                    CHECK(near(length(Vec3{q.x, q.y, q.z}) * length(Vec3{q.x, q.y, q.z}) + q.w * q.w, 1.0f, 1e-3f));
                    CHECK(near(rotate(q, e(face, fs)), f, 1e-3f));
                    const Vec3 tPerp = normalize(t - f * dot(t, f));
                    CHECK(near(rotate(q, e(top, ts)), tPerp, 1e-3f));
                    // the hand behind the face mirrored: only its component across the face changes
                    const Vec3 local{0.4f, -1.5f, 2.5f}, m = mirror_across_face(local, s);
                    for (int i = 0; i < 3; ++i) {
                        const float l = i == 0 ? local.x : i == 1 ? local.y : local.z, mi = i == 0 ? m.x : i == 1 ? m.y : m.z;
                        CHECK(near(mi, i == s.side() ? -l : l));
                    }
                }
        }
    // the left hand held level, fist forward: the face out to the left (away from the body), the top up; in the
    // right hand out to the right
    WorldMapping map;
    map.unitsPerMetre = 100;
    ShieldAxes s;
    s.face = 1; s.faceSign = -1; s.top = 2; s.topSign = 1;
    {
        const Quat q = shield_rotation(s, to_game_dir(map, shield_face_in_grip(0)), to_game_dir(map, shield_top_in_grip()));
        CHECK(near(rotate(q, {0, -1, 0}), heading_right(0) * -1.0f, 1e-3f));
        CHECK(near(rotate(q, {0, 0, 1}), {0, 1, 0}, 1e-3f));
        CHECK(near(rotate(q, {0, -1, 0}) * -1.0f,
                   to_game_dir(map, shield_face_in_grip(1)), 1e-3f));
    }
    // deterministic for a pose and continuous through every wrist turn: a full roll of the wrist, held level, pointed
    // straight up and straight down, and turned over past vertical. Each small turn of the controller turns the shield
    // as much and no more (a flip is a jump), and its face stays out of the back of the hand, its top up through the fist
    auto shield_for = [&](Quat grip) {
        return shield_rotation(s, to_game_dir(map, rotate(grip, shield_face_in_grip(0))),
                               to_game_dir(map, rotate(grip, shield_top_in_grip())));
    };
    for (const Quat base : {Quat{}, axis_angle({1, 0, 0}, kPi / 2), axis_angle({1, 0, 0}, -kPi / 2),
                            axis_angle({0, 1, 0}, 2.0f) * axis_angle({1, 0, 0}, 1.2f)})
        for (const Vec3 axis : {Vec3{0, 0, -1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
            Quat prev = shield_for(base);
            const float stepAngle = 2 * kPi / 180;
            for (int i = 1; i <= 360; ++i) {
                const Quat grip = normalize(base * axis_angle(axis, stepAngle * float(i)));
                const Quat q = shield_for(grip);
                CHECK(angle_between(q, prev) < stepAngle + 1e-3f);
                CHECK(near(rotate(q, {0, -1, 0}), to_game_dir(map, rotate(grip, {-1, 0, 0})), 1e-3f));
                CHECK(near(rotate(q, {0, 0, 1}), to_game_dir(map, rotate(grip, {0, 1, 0})), 1e-3f));
                prev = q;
            }
        }
}

// conducting with the Wind Waker (vr_baton.h, docs/VR_WIND_WAKER.md)
static void test_baton() {
    using D = BatonDir;
    // one source's direction: none in the dead zone, each of the four, an exact diagonal to the side, hysteresis
    {
        BatonFilter f;
        CHECK(f.update(0.3f, 0.2f, 0.5f, 0.35f) == D::None);  // drift
        CHECK(f.update(0, 0.6f, 0.5f, 0.35f) == D::Up);
        CHECK(f.update(0, 0.4f, 0.5f, 0.35f) == D::Up);      // eased back, still beyond its exit: held
        CHECK(f.update(0, 0.3f, 0.5f, 0.35f) == D::None);    // released
        CHECK(f.update(0, -0.7f, 0.5f, 0.35f) == D::Down);
        f.dir = D::None;
        CHECK(f.update(-0.8f, 0, 0.5f, 0.35f) == D::Left);
        f.dir = D::None;
        CHECK(f.update(0.8f, 0, 0.5f, 0.35f) == D::Right);
        f.dir = D::None;
        CHECK(f.update(0.6f, 0.6f, 0.5f, 0.35f) == D::Right);   // an exact diagonal: to the side, always
        f.dir = D::None;
        CHECK(f.update(-0.6f, -0.6f, 0.5f, 0.35f) == D::Left);
        // along a diagonal a held direction stays until the other axis is clearly larger
        f.dir = D::None;
        CHECK(f.update(0, 0.8f, 0.5f, 0.35f) == D::Up);
        CHECK(f.update(0.7f, 0.65f, 0.5f, 0.35f) == D::Up);     // the side a little larger: still up
        CHECK(f.update(0.9f, 0.6f, 0.5f, 0.35f) == D::Right);   // a quarter larger: turned
        CHECK(f.update(-0.8f, 0, 0.5f, 0.35f) == D::Left);      // straight across to the other side
        CHECK(f.update(NAN, 0, 0.5f, 0.35f) == D::None);
        float x = 9, y = 9;
        baton_stick(D::Up, x, y);
        CHECK(x == 0 && y == 1);
        baton_stick(D::Left, x, y);
        CHECK(x == -1 && y == 0);
        baton_stick(D::None, x, y);
        CHECK(x == 0 && y == 0);
    }
    // a hand: held out from where it settled when conducting began, in the head's heading of that moment
    {
        const float dt = 0.1f, r = 0.12f;
        BatonHand h;
        // H(pos): one read 0.1 s after the last; settle(pos): still there long enough for its centre
        auto H = [&](Vec3 p, float yaw = 0, bool recentre = false) { return h.update(true, p, yaw, r, recentre, dt); };
        auto settle = [&](BatonHand& hand, Vec3 p, float yaw) {
            for (int i = 0; i < 5; ++i) CHECK(hand.update(true, p, yaw, r, false, dt) == D::None);
            CHECK(hand.centred());
        };
        const Vec3 c{0.2f, 1.1f, -0.3f};
        // still moving (coming forward from the item wheel): no centre, no direction, however far it goes
        CHECK(H(c + Vec3{0, 0.3f, 0}) == D::None);
        for (int i = 1; i <= 6; ++i) CHECK(H(c + Vec3{0, 0.3f - 0.05f * float(i), 0}) == D::None && !h.centred());
        settle(h, c, 0);  // at rest: centred where it rests
        CHECK(H(c + Vec3{0.01f, -0.02f, 0.015f}) == D::None);  // jitter
        CHECK(H(c + Vec3{0, 0.05f, 0}) == D::None);
        CHECK(H(c + Vec3{0, 0.13f, 0}) == D::Up);
        CHECK(H(c + Vec3{0, 0.11f, 0}) == D::Up);   // within its radius, beyond its exit (90 %)
        CHECK(H(c + Vec3{0, 0.05f, 0}) == D::None); // back: neutral
        CHECK(H(c + Vec3{0, 0.14f, 0}) == D::Up);   // the same direction again
        CHECK(H(c + Vec3{0, 0.02f, 0}) == D::None);
        CHECK(H(c + Vec3{0, -0.15f, 0}) == D::Down);
        CHECK(H(c + Vec3{-0.02f, 0, 0}) == D::None);
        CHECK(H(c + Vec3{-0.15f, 0, 0}) == D::Left);
        CHECK(H(c) == D::None);
        CHECK(H(c + Vec3{0.15f, 0, 0}) == D::Right);
        // the neutral circle and its four quadrants: a diagonal reach counts once it leaves the circle, though
        // neither axis alone reaches the radius (down and to the right had given nothing)
        CHECK(H(c) == D::None);
        CHECK(H(c + Vec3{0.05f, -0.09f, 0}) == D::None);   // 10.3 cm out: inside the circle (its centre follows the rest)
        CHECK(H(c + Vec3{0.07f, -0.11f, 0}) == D::Down);   // 13 cm out, mostly down
        CHECK(H(c) == D::None);
        CHECK(H(c + Vec3{0.11f, -0.07f, 0}) == D::Right);  // 13 cm out, mostly to the right
        CHECK(H(c + Vec3{0.11f, -0.12f, 0}) == D::Right);  // past the diagonal, not clearly: still right
        CHECK(H(c + Vec3{0.08f, -0.13f, 0}) == D::Down);   // clearly down now
        CHECK(H(c + Vec3{-0.09f, -0.09f, 0}) == D::Down);  // round to the lower left diagonal: the held axis keeps it
        CHECK(H(c) == D::None);
        CHECK(H(c + Vec3{-0.10f, -0.09f, 0}) == D::Left);  // from neutral: the larger axis
        CHECK(H(c) == D::None);
        CHECK(H(c + Vec3{0.15f, 0, 0}) == D::Right);
        CHECK(H(c + Vec3{0.15f, 0, -0.2f}) == D::Right);       // forward and back: no direction
        CHECK(H(c + Vec3{0.15f, 0, -0.2f}, 1.2f) == D::Right); // the head turned since: what the hand means is the same
        // reads within one frame (no time between them) neither settle nor unsettle it
        CHECK(h.update(true, c + Vec3{0.15f, 0, -0.2f}, 0, r, false, 0) == D::Right);
        // tracking lost: nothing; tracked again far from the centre: it settles there first, no note
        CHECK(h.update(false, c, 0, r, false, dt) == D::None && !h.centred());
        CHECK(H(c + Vec3{0, 0.3f, 0}) == D::None);
        settle(h, c + Vec3{0, 0.3f, 0}, 0);
        CHECK(H(c + Vec3{0, 0.32f, 0}) == D::None);  // from the new centre
        CHECK(H(c + Vec3{0, 0.3f + 0.13f, 0}) == D::Up);
        // a jump of the pose (a tracking glitch): settled again where it landed, no note
        CHECK(H(c + Vec3{0.5f, 0.9f, 0}) == D::None && !h.centred());
        settle(h, c + Vec3{0.5f, 0.9f, 0}, 0);
        CHECK(H(c + Vec3{0.5f, 0.9f + 0.13f, 0}) == D::Up);
        // a recentre: settled again
        CHECK(H(c + Vec3{0.5f, 0.9f + 0.13f, 0}, 0, true) == D::None && !h.centred());
        // the beat's hand: left or right only
        BatonHand b;
        settle(b, c, 0);
        CHECK(b.update(true, c + Vec3{0, 0.15f, 0}, 0, r, false, dt, true) == D::None);
        CHECK(b.update(true, c + Vec3{0.1f, 0.2f, 0}, 0, r, false, dt, true) == D::None);  // not yet out to the side
        CHECK(b.update(true, c + Vec3{0.14f, 0.2f, 0}, 0, r, false, dt, true) == D::Right); // up does not hold it back
        // a centre taken facing left (yaw +90 degrees, forward -X): its right is -Z, its left +Z
        BatonHand t;
        settle(t, c, kPi / 2);
        CHECK(t.update(true, c + Vec3{0, 0, -0.15f}, kPi / 2, r, false, dt) == D::Right);
        CHECK(t.update(true, c, 0, r, false, dt) == D::None);
        CHECK(t.update(true, c + Vec3{0, 0, 0.15f}, 0, r, false, dt) == D::Left);  // a later head heading changes nothing
    }
    // the notes from the right hand (the wand's, BatonInput::noteHand 1), the beat from the left; its zone mirrored:
    // the right note out at 3 cm, the left at 3 too
    {
        Baton b;
        BatonInput in;
        in.dt = 0.1f;
        in.noteHand = 1;
        const Vec3 lc{-0.2f, 1.1f, -0.3f}, rc{0.2f, 1.1f, -0.3f};
        in.tracked[0] = in.tracked[1] = true;
        in.hand[0] = lc;
        in.hand[1] = rc;
        in.conducting = true;
        BatonOutput o;
        for (int i = 0; i < 6; ++i) o = b.update(in);
        CHECK(o.dir[0] == D::None && o.dir[1] == D::None);
        in.hand[1] = rc + Vec3{0.12f, 0, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::Right && o.dir[1] == D::None);  // a note, not the beat
        in.hand[1] = rc;
        for (int i = 0; i < 4; ++i) o = b.update(in);  // (a note is held kFlickHold at least)
        CHECK(o.dir[0] == D::None);
        in.hand[1] = rc + Vec3{-0.07f, 0, 0};  // 7 cm inward: not yet (10.2)
        o = b.update(in);
        CHECK(o.dir[0] == D::None);
        in.hand[1] = rc + Vec3{-0.12f, 0, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::Left);
        in.hand[1] = rc + Vec3{0, -0.12f, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::Down);
        in.hand[1] = rc;
        for (int i = 0; i < 4; ++i) o = b.update(in);  // (a note is held kFlickHold at least)
        in.hand[0] = lc + Vec3{-0.14f, 0, 0};  // the left hand left: the beat (3/4)
        o = b.update(in);
        CHECK(o.dir[1] == D::Left && o.dir[0] == D::None);
    }
    // the middle follows the resting hand: a hand that drifts 6 cm while no direction is held still comes back to the
    // middle where it now rests, and a flick from there gives its direction (BatonHand::kFollow)
    {
        BatonHand t;
        const Vec3 c0{0.2f, 1.1f, -0.3f};
        const float dt = 0.1f;
        for (int i = 0; i < 6; ++i) t.update(true, c0, 0, 0.09f, false, dt);
        CHECK(t.centred());
        for (int i = 1; i <= 30; ++i) t.update(true, c0 + Vec3{0.002f * float(i), 0, 0}, 0, 0.09f, false, dt);  // drifts 6 cm
        const Vec3 rest = c0 + Vec3{0.06f, 0, 0};
        for (int i = 0; i < 40; ++i) t.update(true, rest, 0, 0.09f, false, dt);
        CHECK(length(t.offset()) < 0.01f);  // the middle where it rests now
        CHECK(t.update(true, rest + Vec3{0, 0.12f, 0}, 0, 0.09f, false, dt) == D::Up);
        CHECK(t.update(true, rest + Vec3{0.01f, 0.01f, 0}, 0, 0.09f, false, dt) == D::None);  // back near it: the middle
    }
    // the wand hand's middle held (BatonInput::middle): no direction from it, wherever it is; let go, its direction again
    {
        Baton b;
        BatonInput in;
        in.dt = 0.1f;
        in.noteHand = 1;
        const Vec3 lc{-0.2f, 1.1f, -0.3f}, rc{0.2f, 1.1f, -0.3f};
        in.tracked[0] = in.tracked[1] = true;
        in.hand[0] = lc;
        in.hand[1] = rc;
        in.conducting = true;
        BatonOutput o;
        for (int i = 0; i < 6; ++i) o = b.update(in);
        in.hand[1] = rc + Vec3{0.15f, 0, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::Right);
        in.middle[1] = true;
        o = b.update(in);
        CHECK(o.dir[0] == D::None && o.handDir[0] == D::None);
        in.middle[1] = false;
        o = b.update(in);
        CHECK(o.dir[0] == D::Right);
    }
    // a direction held with the hand still for BatonHand::kRestTime: the middle is taken there (the hand had rested
    // 18-30 cm below a middle taken with the wand raised, "down" held for good); a move on from there gives directions
    {
        BatonHand t;
        const Vec3 c0{0.2f, 1.1f, -0.3f};
        const float dt = 0.1f;
        for (int i = 0; i < 6; ++i) t.update(true, c0, 0, 0.09f, false, dt);
        CHECK(t.centred());
        const Vec3 low = c0 + Vec3{0, -0.2f, 0};
        CHECK(t.update(true, c0 + Vec3{0, -0.1f, 0}, 0, 0.09f, false, dt) == D::Down);
        CHECK(t.update(true, low, 0, 0.09f, false, dt) == D::Down);
        for (int i = 0; i < 5; ++i) CHECK(t.update(true, low, 0, 0.09f, false, dt) == D::Down);  // half a second: still down
        for (int i = 0; i < 25; ++i) CHECK(t.update(true, low, 0, 0.09f, false, dt) == D::Down);  // 3 s: still down (a
        // note held two beats, a big pose held)
        D last = D::Down;
        for (int i = 0; i < 12 && t.rests() == 0; ++i) last = t.update(true, low, 0, 0.09f, false, dt);
        CHECK(last == D::None && t.rests() == 1);  // 4 s still: the middle is here now
        CHECK(t.update(true, low + Vec3{0.12f, 0, 0}, 0, 0.09f, false, dt) == D::Right);
    }
    // the wand's strokes (BatonHand, flicks on): 8 cm of continuous movement any way gives that way, a wrist's flick or
    // a whole arm's move alike; 6 cm gives nothing; coming straight back gives the middle, never the opposite note,
    // unless it goes on past the middle's zone; back and to one side gives that side
    {
        BatonHand t;
        const Vec3 c0{0.2f, 1.1f, -0.3f};
        const float dt = 0.011f;  // (90 reads a second)
        auto go = [&](BatonHand& h, Vec3 from, Vec3 to, int n) {
            D d = D::None;
            for (int i = 1; i <= n; ++i) d = h.update(true, from + (to - from) * (float(i) / float(n)), 0, 0.102f, false, dt, false, {}, true);
            return d;
        };
        auto stay = [&](BatonHand& h, Vec3 at, int n) {
            D d = D::None;
            for (int i = 0; i < n; ++i) d = h.update(true, at, 0, 0.102f, false, dt, false, {}, true);
            return d;
        };
        stay(t, c0, 40);
        CHECK(t.centred());
        // 6 cm up: nothing; on to 9 cm: up, held while the hand stays there
        CHECK(go(t, c0, c0 + Vec3{0, 0.06f, 0}, 12) == D::None);
        CHECK(go(t, c0 + Vec3{0, 0.06f, 0}, c0 + Vec3{0, 0.09f, 0}, 6) == D::Up);
        CHECK(stay(t, c0 + Vec3{0, 0.09f, 0}, 60) == D::Up);
        // straight back down to the middle: never down, the middle once there
        bool sawDown = false;
        for (int i = 1; i <= 18; ++i) {
            const D d = t.update(true, c0 + Vec3{0, 0.09f - 0.005f * float(i), 0}, 0, 0.102f, false, dt, false, {}, true);
            sawDown = sawDown || d == D::Down;
        }
        CHECK(!sawDown && stay(t, c0, 40) == D::None);
        // a quick 9 cm flick in towards the body: left; out: right
        CHECK(go(t, c0, c0 + Vec3{-0.09f, 0, 0}, 4) == D::Left);
        stay(t, c0 + Vec3{-0.09f, 0, 0}, 30);
        go(t, c0 + Vec3{-0.09f, 0, 0}, c0, 4);
        CHECK(stay(t, c0, 40) == D::None);
        CHECK(go(t, c0, c0 + Vec3{0.09f, 0, 0}, 4) == D::Right);
        stay(t, c0 + Vec3{0.09f, 0, 0}, 30);
        go(t, c0 + Vec3{0.09f, 0, 0}, c0, 4);
        CHECK(stay(t, c0, 40) == D::None);
        // up 10 cm, then over to the left and down a little (up to left): left
        CHECK(go(t, c0, c0 + Vec3{0, 0.10f, 0}, 10) == D::Up);
        CHECK(go(t, c0 + Vec3{0, 0.10f, 0}, c0 + Vec3{-0.10f, 0.03f, 0}, 12) == D::Left);
        // up then straight down through the middle and 11 cm past it: down
        BatonHand u;
        stay(u, c0, 40);
        CHECK(go(u, c0, c0 + Vec3{0, 0.12f, 0}, 10) == D::Up);
        CHECK(go(u, c0 + Vec3{0, 0.12f, 0}, c0 + Vec3{0, -0.11f, 0}, 18) == D::Down);  // (past the 10.2 cm zone)
        // a slow move: 9 cm right at 0.1 m/s (0.9 s): right
        BatonHand q;
        stay(q, c0, 40);
        CHECK(go(q, c0, c0 + Vec3{0.09f, 0, 0}, 82) == D::Right);
        // slower still, 9 cm up at 0.06 m/s with a 0.1 s pause half-way: up
        BatonHand g;
        stay(g, c0, 40);
        go(g, c0, c0 + Vec3{0, 0.045f, 0}, 68);
        stay(g, c0 + Vec3{0, 0.045f, 0}, 9);
        CHECK(go(g, c0 + Vec3{0, 0.045f, 0}, c0 + Vec3{0, 0.09f, 0}, 68) == D::Up);
        // a creep (0.02 m/s, under the stroke's 0.05) of 9 cm: nothing
        BatonHand k;
        stay(k, c0, 40);
        CHECK(go(k, c0, c0 + Vec3{0.09f, 0, 0}, 410) == D::None && k.flicks() == 0);
    }
    // long arm motions (Wind Waker Test 5): whole-arm moves of 50 cm give their directions, a raise that sets off
    // sideways is up, a pose held 2 s stays, and the arm brought back roughly (13 cm off) to the middle is the middle
    {
        const float dt = 0.011f;
        const Vec3 c0{0.2f, 1.1f, -0.3f};
        auto settle = [&](BatonHand& h) { for (int i = 0; i < 40; ++i) h.update(true, c0, 0, 0.102f, false, dt, false, {}, true); };
        auto move = [&](BatonHand& h, Vec3 from, Vec3 to, int n) {
            D d = D::None;
            for (int i = 1; i <= n; ++i) d = h.update(true, from + (to - from) * (float(i) / float(n)), 0, 0.102f, false, dt, false, {}, true);
            return d;
        };
        auto hold = [&](BatonHand& h, Vec3 at, int n) {
            D d = D::None;
            for (int i = 0; i < n; ++i) d = h.update(true, at, 0, 0.102f, false, dt, false, {}, true);
            return d;
        };
        BatonHand a;
        settle(a);
        CHECK(a.centred());
        // the arm sets off outward fast (a flick right), then rises all the way: 10 cm out, then 50 cm up
        const Vec3 side = c0 + Vec3{0.10f, 0.02f, 0};
        const Vec3 top = c0 + Vec3{0.12f, 0.50f, 0};
        D d = move(a, c0, side, 6);
        d = move(a, side, top, 25);
        CHECK(d == D::Up);
        CHECK(hold(a, top, 180) == D::Up && a.rests() == 0);  // held 2 s: still up, the middle not moved
        // back down to about the middle, 13 cm off it, and held: the middle
        const Vec3 nearMid = c0 + Vec3{0.10f, -0.08f, 0};
        move(a, top, nearMid, 25);
        CHECK(hold(a, nearMid, 40) == D::None);
        CHECK(a.radius() > 0.12f);  // (the zone has grown with the moves)
        // up again all the way, then straight back down: up held while the arm is still out past the zone, the
        // middle once it is back in it
        const Vec3 high = c0 + Vec3{0.02f, 0.45f, 0};
        CHECK(move(a, nearMid, high, 25) == D::Up);
        bool upHeld = true;
        for (int i = 1; i <= 25; ++i) {
            const Vec3 at = high + (c0 - high) * (float(i) / 25.0f);
            const D dd = a.update(true, at, 0, 0.102f, false, dt, false, {}, true);
            if (at.y - c0.y > 0.20f && dd != D::Up) upHeld = false;
        }
        CHECK(upHeld);
        CHECK(hold(a, c0, 40) == D::None);
        // a long sweep out to the right, back, then down all the way
        const Vec3 right = c0 + Vec3{0.50f, 0.05f, 0};
        CHECK(move(a, c0, right, 25) == D::Right);
        hold(a, right, 30);
        move(a, right, c0, 25);
        CHECK(hold(a, c0, 40) == D::None);
        const Vec3 low = c0 + Vec3{-0.03f, -0.45f, 0};
        CHECK(move(a, c0, low, 25) == D::Down);
        CHECK(hold(a, low, 30) == D::Down);
        // across the body, left, from down: left
        const Vec3 across = c0 + Vec3{-0.45f, -0.05f, 0};
        CHECK(move(a, low, across, 30) == D::Left);
        // then small wrist flicks right after the big moves (the zone still grown): each plays, by its own measure
        hold(a, across, 30);
        move(a, across, c0, 25);
        CHECK(hold(a, c0, 40) == D::None);
        CHECK(a.radius() > 0.12f);
        CHECK(move(a, c0, c0 + Vec3{0, 0.09f, 0}, 16) == D::Up);  // 9 cm up at 0.5 m/s: a short stroke
        CHECK(hold(a, c0 + Vec3{0, 0.09f, 0}, 20) == D::Up);
        move(a, c0 + Vec3{0, 0.09f, 0}, c0, 16);
        CHECK(hold(a, c0, 40) == D::None);
        CHECK(move(a, c0, c0 + Vec3{0.09f, 0, 0}, 4) == D::Right);  // a quick 9 cm flick out
        hold(a, c0 + Vec3{0.09f, 0, 0}, 20);
        move(a, c0 + Vec3{0.09f, 0, 0}, c0, 4);
        CHECK(hold(a, c0, 40) == D::None);
        CHECK(move(a, c0, c0 + Vec3{-0.09f, 0, 0}, 16) == D::Left);  // 9 cm in towards the body
        // and a big swing again straight after: down
        move(a, c0 + Vec3{-0.09f, 0, 0}, c0, 16);
        hold(a, c0, 40);
        CHECK(move(a, c0, c0 + Vec3{0.02f, -0.45f, 0}, 25) == D::Down);
        // small pointer moves alone keep the set zone: a new hand, 6 cm moves, the zone 10.2 cm
        BatonHand p;
        settle(p);
        move(p, c0, c0 + Vec3{0.06f, 0, 0}, 20);
        hold(p, c0 + Vec3{0.06f, 0, 0}, 20);
        CHECK(p.radius() < 0.1025f);
    }
    // the hands swapped (BatonInput::swapHands), each keeping its calibration: the right hand (the wand's) gives the right
    // stick's channel, still with the wand's zone (3 cm); the left gives the left stick's, with the full reach
    {
        Baton b;
        BatonInput in;
        in.dt = 0.1f;
        in.noteHand = 1;
        in.swapHands = true;
        const Vec3 lc{-0.2f, 1.1f, -0.3f}, rc{0.2f, 1.1f, -0.3f};
        in.tracked[0] = in.tracked[1] = true;
        in.hand[0] = lc;
        in.hand[1] = rc;
        in.conducting = true;
        BatonOutput o;
        for (int i = 0; i < 6; ++i) o = b.update(in);
        in.hand[1] = rc + Vec3{0.12f, 0, 0};
        o = b.update(in);
        CHECK(o.dir[1] == D::Right && o.dir[0] == D::None);  // the right hand: the right stick, at the wand's 10.2 cm
        in.hand[1] = rc;
        for (int i = 0; i < 4; ++i) o = b.update(in);  // (a note is held kFlickHold at least)
        in.hand[0] = lc + Vec3{-0.05f, 0, 0};  // the left hand 5 cm: not yet (its zone the full 12 cm)
        o = b.update(in);
        CHECK(o.dir[0] == D::None && o.dir[1] == D::None);
        in.hand[0] = lc + Vec3{-0.14f, 0, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::Left && o.dir[1] == D::None);  // the left hand: the left stick
    }
    // both channels: the stick wins over its hand, a neutral source never cancels the other, each side its own
    {
        Baton b;
        BatonInput in;
        in.dt = 0.1f;
        const Vec3 lc{-0.2f, 1.1f, -0.3f}, rc{0.2f, 1.1f, -0.3f};
        in.tracked[0] = in.tracked[1] = true;
        in.hand[0] = lc;
        in.hand[1] = rc;
        BatonOutput o = b.update(in);  // not conducting: nothing, whatever the hands and sticks do
        CHECK(!b.conducting() && o.dir[0] == D::None && o.dir[1] == D::None && !o.started);
        in.stick[0][1] = 1;
        in.hand[0] = lc + Vec3{0, 0.2f, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::None && o.handDir[0] == D::None);
        in.stick[0][1] = 0;
        in.hand[0] = lc;
        in.conducting = true;
        o = b.update(in);  // conducting begins: the hands settle where they are
        CHECK(o.started && o.dir[0] == D::None && o.dir[1] == D::None);
        for (int i = 0; i < 5; ++i) o = b.update(in);
        CHECK(o.dir[0] == D::None && o.dir[1] == D::None);
        {
            Vec3 at;  // where the body faces while conducting: between the two hands' settled places
            CHECK(b.hands_centre(at) && length(at - (lc + rc) * 0.5f) < 1e-5f);
        }
        in.hand[0] = lc + Vec3{-0.04f, 0, 0};  // the left hand 4 cm to the left: not yet (a stroke registers from 5.1 cm)
        o = b.update(in);
        CHECK(o.handDir[0] == D::None);
        in.hand[0] = lc + Vec3{-0.12f, 0, 0};  // 12 cm: the left note (its zone 10.2 cm every side: kNoteShape, kNoteReachShare)
        o = b.update(in);
        CHECK(o.handDir[0] == D::Left);
        in.hand[0] = lc;
        for (int i = 0; i < 4; ++i) o = b.update(in);  // (a note is held kFlickHold at least)
        in.hand[0] = lc + Vec3{0.07f, 0, 0};  // 7 cm to the right: not yet (10.2, as its left)
        o = b.update(in);
        CHECK(o.handDir[0] == D::None);
        in.hand[0] = lc + Vec3{0.12f, 0, 0};
        o = b.update(in);
        CHECK(o.handDir[0] == D::Right);
        in.hand[0] = lc + Vec3{0, 0.12f, 0};  // up: its up at 10.2 cm
        o = b.update(in);
        CHECK(o.handDir[0] == D::Up);
        in.hand[1] = rc + Vec3{-0.09f, 0, 0};  // the right hand 9 cm to the left: not yet (the beat's circle is centred)
        o = b.update(in);
        CHECK(o.handDir[1] == D::None);
        in.hand[0] = lc;
        in.hand[1] = rc;
        for (int i = 0; i < 4; ++i) o = b.update(in);  // (a note is held kFlickHold at least)
        CHECK(o.dir[0] == D::None && o.dir[1] == D::None);
        in.hand[0] = lc + Vec3{0, 0.15f, 0};  // the left hand up: the note
        o = b.update(in);
        CHECK(o.dir[0] == D::Up && o.handDir[0] == D::Up && o.dir[1] == D::None);  // the right side untouched
        in.stick[0][0] = -0.9f;  // the left stick left as well: the stick's direction
        o = b.update(in);
        CHECK(o.dir[0] == D::Left && o.stickDir[0] == D::Left && o.handDir[0] == D::Up);
        in.stick[0][0] = 0;  // the stick back to neutral: the hand's direction again (not cancelled)
        o = b.update(in);
        CHECK(o.dir[0] == D::Up);
        in.hand[0] = lc;  // the hand back too: neutral
        for (int i = 0; i < 4; ++i) o = b.update(in);  // (a note is held kFlickHold at least)
        CHECK(o.dir[0] == D::None);
        in.stick[0][1] = -0.8f;  // the stick down with the hand neutral: the stick's (the hand does not cancel it)
        o = b.update(in);
        CHECK(o.dir[0] == D::Down && o.handDir[0] == D::None);
        in.stick[0][1] = 0;
        in.stick[1][0] = 0.8f;  // the right stick right: the beat (6/4), the notes untouched
        o = b.update(in);
        CHECK(o.dir[1] == D::Right && o.dir[0] == D::None);
        in.stick[1][0] = 0;
        in.stick[1][1] = 0.9f;  // the right stick up: the C-stick up (the notes untouched)
        o = b.update(in);
        CHECK(o.dir[1] == D::Up && o.dir[0] == D::None);
        in.stick[1][1] = 0;
        in.hand[1] = rc + Vec3{-0.14f, 0, 0};  // the right hand left: the beat (3/4)
        o = b.update(in);
        CHECK(o.dir[1] == D::Left && o.dir[0] == D::None);
        in.tracked[1] = false;  // its tracking lost: nothing from it
        o = b.update(in);
        CHECK(o.dir[1] == D::None);
        in.tracked[1] = true;
        for (int i = 0; i < 6; ++i) o = b.update(in);  // tracked again where it is: settles there, no beat
        CHECK(o.dir[1] == D::None);
        // the hands switched off: only the sticks
        in.hands = false;
        in.hand[0] = lc + Vec3{0, 0.2f, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::None && o.handDir[0] == D::None);
        in.hands = true;
        // conducting ends: nothing; the next song settles the hands again where they are then
        in.conducting = false;
        o = b.update(in);
        CHECK(o.stopped && o.dir[0] == D::None && o.dir[1] == D::None);
        in.conducting = true;
        in.hand[0] = lc + Vec3{0, 0.4f, 0};
        o = b.update(in);
        CHECK(o.started && o.dir[0] == D::None);
        for (int i = 0; i < 5; ++i) o = b.update(in);
        CHECK(o.dir[0] == D::None);
        in.hand[0] = lc + Vec3{0, 0.4f, 0.0f} + Vec3{0.14f, 0, 0};
        o = b.update(in);
        CHECK(o.dir[0] == D::Right);
    }
    // the settings
    {
        Config c;
        CHECK(c.batonHands && near(c.batonHandReach, 0.12f));
        CHECK(from_kv(c, "vr.batonHands", "0") && !c.batonHands);
        CHECK(from_kv(c, "vr.batonHandReach", "0.9") && near(c.batonHandReach, 0.4f));  // clamped
    }
    // replacement texture packs: on by default, the VRAM budget automatic (0) by default, never negative
    {
        Config c;
        CHECK(c.texturePacks && from_kv(c, "vr.texturePacks", "0") && !c.texturePacks);
        CHECK(c.texturePackBudgetMB == 0 && from_kv(c, "vr.texturePackBudgetMB", "3072") && c.texturePackBudgetMB == 3072 &&
              from_kv(c, "vr.texturePackBudgetMB", "-5") && c.texturePackBudgetMB == 0);
    }
}

// the stick (docs/VR_HELD_ITEMS.md, "The stick"): held, tracked and swung as the sword, with its own attack
static void test_melee_items() {
    CHECK(melee_item(0x103) && melee_item(0x101));                            // the sword, a picked-up weapon
    for (uint16_t item : {0x100, 0x102, 0x104, 0x105, 0x108, 0x109}) CHECK(!melee_item(item));
    CHECK(melee_attack(0x103, 0) == 1 && melee_attack(0x103, 0x10000000) == 1);  // the sword's slash, always
    CHECK(melee_attack(0x101, 0x10000000) == 2);                              // the stick's own, from its actor
    CHECK(melee_attack(0x101, 0) == 0);                                       // its actor gone: no attack
    CHECK(melee_attack(0x100, 0x10000000) == 0 && melee_attack(0x108, 0) == 0);
    CHECK(swing_presses_do_button(0x101) && !swing_presses_do_button(0x103) && !swing_presses_do_button(0x100));  // A swings it, B throws it
}

// VR archery (vr_bow.h) and which hand holds, aims and uses each aimed item (vr_sword.h)
static void test_bow_and_hands() {
    // the hands: the grappling hook in the off hand, fired by its trigger; the bow in the sword hand, by no trigger
    CHECK(off_hand_item(kItemGrapplingHook) && off_hand_item(kItemHookshot) && off_hand_item(kItemBoomerang) &&
          !off_hand_item(0x27) && !off_hand_item(0x36));
    CHECK(fired_by_off_trigger(kItemGrapplingHook) && fired_by_off_trigger(kItemBoomerang) && !fired_by_off_trigger(kItemHookshot));
    CHECK(fired_by_both_triggers(kItemHookshot) && !fired_by_both_triggers(kItemGrapplingHook) && !fired_by_both_triggers(kItemBoomerang));
    // the same physical hands in either handedness: the bow right, the hookshot and grappling hook left, the rest by it
    CHECK(fixed_item_side(0x27) == 1 && fixed_item_side(0x36) == 1 && fixed_item_side(kItemHookshot) == 0 &&
          fixed_item_side(kItemGrapplingHook) == 0 && fixed_item_side(kItemBoomerang) == 0 && fixed_item_side(0x33) == -1);
    CHECK(fixed_item_side(kItemDekuLeaf) == 0 && fired_by_left_trigger(kItemDekuLeaf) && fired_by_trigger(kItemDekuLeaf));
    CHECK(fired_by_left_trigger(kItemGrapplingHook) && fired_by_left_trigger(kItemBoomerang) && !fired_by_left_trigger(kItemHookshot));
    CHECK(!fired_by_trigger(0x27) && !fired_by_trigger(0x35) && !fired_by_trigger(0x36));
    CHECK(fired_by_trigger(kItemHookshot) && fired_by_trigger(kItemBoomerang) && fired_by_trigger(kItemGrapplingHook));
    CHECK(bow_in_hand(0x27) && bow_in_hand(0x35) && bow_in_hand(0x36) && !bow_in_hand(0x2F));
    // the bow: the hand at the bow puts the arrow on (the button held: drawn); either trigger shoots; the next arrow on
    // at the bow again, no sooner than kRenockSeconds
    BowMachine m;
    auto in = [](bool aiming, bool mainT, bool offT, float z, float dt = 0.011f, bool grip = true) {
        BowInput b;
        b.aiming = aiming;
        b.mainTrigger = mainT;
        b.offTrigger = offT;
        b.stringGrip = grip;
        b.bowHand = {0, 0, 0};
        b.stringHand = {0, 0, z};
        b.dt = dt;
        return b;
    };
    BowOutput o = m.update(in(true, false, false, 0.5f));
    CHECK(!o.onBow && !o.fire);                           // taken out: relaxed, no arrow
    CHECK(!m.update(in(true, true, false, 0.5f)).fire);   // a trigger with no arrow on: nothing
    m.update(in(true, false, false, 0.5f));
    o = m.update(in(true, false, false, 0.2f));
    CHECK(o.nocked && o.onBow);                           // the hand at the bow: on, drawn
    CHECK(m.update(in(true, false, false, 0.6f)).onBow);  // stays on, the hand moved away
    o = m.update(in(true, true, false, 0.6f));
    CHECK(o.fire && !o.onBow);                            // the sword hand's trigger: shot, relaxed
    m.update(in(true, false, false, 0.6f));
    CHECK(!m.update(in(true, false, false, 0.1f)).onBow); // at the bow again at once: not yet
    o = m.update(in(true, false, false, 0.1f, 0.6f));
    CHECK(o.nocked && o.onBow);                           // half a second later: the next arrow on
    CHECK(m.update(in(true, false, true, 0.1f)).fire);    // the off hand's trigger
    m.update(in(true, false, true, 0.1f, 0.6f));          // (on again, the trigger still held)
    CHECK(!m.update(in(true, false, true, 0.1f)).fire);   // held: no second shot
    CHECK(!m.update(in(false, false, false, 0.1f)).onBow);  // put away: reset
    CHECK(!m.update(in(true, true, false, 0.1f)).fire);   // up again with a trigger held: no shot
    // taking the string with the grip: none without it; let go drawn back: shot; let go at the bow: still on
    BowMachine g;
    CHECK(!g.update(in(true, false, false, 0.2f, 0.011f, false)).onBow);  // at the bow, the grip open: nothing
    CHECK(g.update(in(true, false, false, 0.2f, 0.6f, true)).nocked);     // squeezed at the bow: on
    BowOutput e = g.update(in(true, false, false, 0.2f, 0.011f, false));  // let go at the bow: not drawn, dropped
    CHECK(e.fire && e.drop && !e.onBow);
    CHECK(g.update(in(true, false, false, 0.2f, 0.6f, true)).nocked);     // the next arrow taken
    g.update(in(true, false, false, 0.5f, 0.011f, true));                 // drawn back
    BowOutput r = g.update(in(true, false, false, 0.5f, 0.011f, false));  // let go drawn back: shot
    CHECK(r.fire && !r.drop && !r.onBow);
    // a short pull from where the string was taken (kMinPullMetres) shoots too
    CHECK(g.update(in(true, false, false, 0.12f, 0.6f, true)).nocked);    // taken with the hands 0.12 m apart
    g.update(in(true, false, false, 0.21f, 0.011f, true));                // 9 cm back
    BowOutput sp = g.update(in(true, false, false, 0.21f, 0.011f, false));
    CHECK(sp.fire && !sp.drop);
}

// the Forsaken Fortress test environment's stage machine (docs/VR_TEST_ENVIRONMENT.md)
static void test_test_env() {
    using namespace vr::test_env;
    using vr::test_env::Action;
    const Facts title{false, false, false}, home{true, true, false}, busy{true, false, false}, fort{true, true, true};
    Machine m;
    Events none, enter, done, failed, reset, refill, leave;
    enter.enter = true;
    done.done = true;
    failed.failed = true;
    reset.reset = true;
    refill.refill = true;
    leave.exit = true;
    auto run = [&](const Facts& f, int frames) {  // frames with no events: the first action seen
        for (int i = 0; i < frames; ++i) {
            const Step s = m.update(f, none);
            if (s.action != Action::None) return s.action;
        }
        return Action::None;
    };
    // nothing happens in normal play, or on the title screen when asked
    CHECK(run(home, 100) == Action::None && m.phase == Phase::Off);
    CHECK(m.update(title, enter).action == Action::None && run(title, 100) == Action::None && m.phase == Phase::Off);
    // asked before a save is in play: entered once one is, after Link has been in control a while
    CHECK(run(busy, 100) == Action::None);
    CHECK(run(home, Machine::kSettleFrames - 1) == Action::None);
    CHECK(run(home, 2) == Action::Isolate && m.phase == Phase::SavingReturn);
    // the return state refused (a scene change): tried again later, at most kTries times, then abandoned
    CHECK(m.update(home, failed).action == Action::Abandon && m.phase == Phase::Off && m.wantEnter);
    CHECK(run(home, Machine::kSettleFrames + 1) == Action::Isolate);
    CHECK(m.update(home, done).action == Action::Warp && m.phase == Phase::Warping);
    // the stage change started: the stage counts only after it settles, in control in the fortress
    CHECK(m.update(home, done).action == Action::None && m.phase == Phase::Arriving);
    CHECK(run(busy, 200) == Action::None);
    CHECK(run(fort, Machine::kSettleFrames - 1) == Action::None);
    CHECK(run(fort, 2) == Action::GiveLoadout && m.phase == Phase::Loadout);
    CHECK(run(fort, Machine::kLoadoutFrames + 1) == Action::SaveFixture && m.phase == Phase::SavingFixture);
    // the reset state refused once (the stage's start still running): tried again after settling
    CHECK(m.update(fort, failed).action == Action::None && m.phase == Phase::Arriving && !m.fixtureSaved);
    CHECK(run(fort, Machine::kChangeFrames + Machine::kSettleFrames + 2) == Action::GiveLoadout);
    CHECK(run(fort, Machine::kLoadoutFrames + 1) == Action::SaveFixture);
    CHECK(m.update(fort, done).action == Action::None && m.phase == Phase::Ready && m.fixtureSaved);
    // refill: the loadout again, here; reset: the fixture loaded, then the loadout
    CHECK(m.update(fort, refill).action == Action::GiveLoadout);
    CHECK(run(fort, Machine::kLoadoutFrames + 1) == Action::None && m.phase == Phase::Ready);
    CHECK(m.update(fort, reset).action == Action::LoadFixture && m.phase == Phase::Resetting);
    CHECK(run(fort, 100) == Action::None);  // waits for the load
    CHECK(m.update(fort, done).action == Action::None && m.phase == Phase::Arriving);
    CHECK(run(fort, Machine::kChangeFrames + Machine::kSettleFrames + 2) == Action::GiveLoadout);
    CHECK(run(fort, Machine::kLoadoutFrames + 1) == Action::None && m.phase == Phase::Ready);
    // entering again while in: ignored; leaving: the return state, then off
    CHECK(m.update(fort, enter).action == Action::None && m.phase == Phase::Ready);
    CHECK(m.update(fort, leave).action == Action::Restore && m.phase == Phase::Leaving);
    CHECK(m.update(home, done).action == Action::None && m.phase == Phase::Off && !m.fixtureSaved && !m.wantEnter);
    CHECK(run(home, 200) == Action::None);
    // a refused stage change goes back; a reset state that will not load warps again
    Machine w;
    w.phase = Phase::Warping;
    CHECK(w.update(home, failed).action == Action::Restore && w.phase == Phase::Leaving);
    Machine r;
    r.phase = Phase::Resetting;
    CHECK(r.update(fort, failed).action == Action::Warp && r.phase == Phase::Warping);
    // all five tries refused: not entered, and not tried again until asked
    Machine t;
    t.update(home, enter);
    for (int i = 0; i < Machine::kTries; ++i) {
        for (int k = 0; k < Machine::kSettleFrames + 1 && t.phase == Phase::Off; ++k) t.update(home, none);
        CHECK(t.phase == Phase::SavingReturn);
        t.update(home, failed);
    }
    CHECK(t.phase == Phase::Off && !t.wantEnter);
    for (int k = 0; k < 100; ++k) t.update(home, none);
    CHECK(t.phase == Phase::Off);
    CHECK(std::string(phase_name(Phase::Ready)) == "ready" && std::string(kStage) == "MajyuE");
}

// the Skull Hammer (docs/VR_SKULL_HAMMER.md): held and swung by the sword hand, its attacks the game's
static void test_skull_hammer() {
    CHECK(melee_item(kItemSkullHammer));
    CHECK(melee_attack(kItemSkullHammer, 0) == 0 && melee_attack(kItemSkullHammer, 0x10000000) == 0);  // no physical cut
    CHECK(!swing_presses_do_button(kItemSkullHammer));  // (its own item button: hammer_press)
    // the button it is on
    const uint8_t onY[3] = {0x20, 0x33, 0xFF}, onR[3] = {0xFF, 0x2F, 0x33}, none[3] = {0x20, 0xFF, 0x2F};
    CHECK(item_button(onY, 0x33) == 1 && item_button(onR, 0x33) == 2 && item_button(none, 0x33) == -1);
    // a horizontal swing pushes the stick its way (the game's side swing), a downward one does not (the front swing)
    CHECK(hammer_swing_stick(true, 1) == 1.0f && hammer_swing_stick(true, -1) == -1.0f);
    CHECK(hammer_swing_stick(false, 1) == 0.0f && hammer_swing_stick(false, 0) == 0.0f);
    // the windup skipped: the front swing's windup ended, the side swing moved to just before its hits, nothing else
    WindupSkip k = hammer_windup_skip(kProcHammerFrontReady, 5.0f);
    CHECK(k.apply && k.endAnimation);
    k = hammer_windup_skip(kProcHammerSideSwing, 6.0f);
    CHECK(k.apply && !k.endAnimation && k.frame == kHammerSideHitFrame);
    CHECK(!hammer_windup_skip(kProcHammerSideSwing, 30.0f).apply);  // already hitting: left alone
    CHECK(!hammer_windup_skip(kProcHammerSideSwing, std::nanf("")).apply);
    CHECK(!hammer_windup_skip(kProcHammerFrontSwing, 1.0f).apply && !hammer_windup_skip(kProcHammerFrontEnd, 1.0f).apply);
    CHECK(!hammer_windup_skip(0x00, 1.0f).apply && !hammer_windup_skip(0x103, 1.0f).apply);
    CHECK(hammer_swing_proc(0x51) && hammer_swing_proc(0x54) && !hammer_swing_proc(0x50) && !hammer_swing_proc(0x55));
    // the side swing's hits start at frame 25 (procHammerSideSwing): the skip lands before them, after its start (4)
    CHECK(kHammerSideHitFrame < 25.0f && kHammerSideHitFrame > 4.0f);
    // the tracked collision from the head's centre to the face: from l_execute_hammer_top (160, 70, 0) and root
    // (8, 0, 0), the head's centre on the shaft at the face's distance along it
    const Vec3 face{160, 70, 0}, root{8, 0, 0}, head{kHammerHeadAlong, 0, 0};
    CHECK(near(dot(normalize(head - root), normalize(face - head)), 0.0f));  // the face square to the shaft
    CHECK(length(face - head) < length(face - root) * 0.5f);                 // much shorter than the hammer
    // the hammer's roll setting
    Config c;
    CHECK(c.hammerRollDegrees == 180.0f);  // turned over: the flat end down (asked for in the headset)
    CHECK(from_kv(c, "vr.hammerRollDegrees", "45") && near(c.hammerRollDegrees, 45.0f));
    CHECK(from_kv(c, "vr.hammerRollDegrees", "400") && near(c.hammerRollDegrees, 180.0f));  // clamped
}

// the VR view's UI (vr_ui.h, docs/VR_UI.md): the gameplay HUD not shown in VR, the action prompt and dialogue kept
static void test_vr_ui() {
    using namespace vr::ui;
    using UiAction = vr::ui::Action;  // (vr::Action is the bindings')
    // a pane and its parents up to the root
    const Link heart[] = {{"P_Heart_03", false, false}, {"L_HeartAll_00", false, false}, {"N_Default_00", true, true}};
    CHECK(classify(heart, 3) == Area::Hud);  // inside a HUD pane: the hearts
    const Link rupee[] = {{"L_Rupy_00", true, true}};
    CHECK(classify(rupee, 1) == Area::Hud);
    const Link button[] = {{"N_X_00", false, false}, {"N_All_00", true, true}};
    CHECK(classify(button, 2) == Area::Hud);
    const Link prompt[] = {{"T_Command_00", false, false}, {"L_CommandA_00", true, true}};
    CHECK(classify(prompt, 2) == Area::ActionPrompt && classify(prompt + 1, 1) == Area::ActionPrompt);
    const Link box[] = {{"T_Msg_00", false, false}, {"N_Msg_00", false, false}, {"N_Position_00", true, true}};
    CHECK(classify(box, 3) == Area::MessageAnchor && classify(box + 2, 1) == Area::MessageAnchor);
    const Link menu[] = {{"T_Item_00", false, false}, {"N_Menu_00", true, false}};
    CHECK(classify(menu, 2) == Area::Other && classify(nullptr, 0) == Area::Other);
    const Link deep[] = {{"L_CommandA_00", false, false}, {"N_TV_00", true, true}};  // not the root's child: not the prompt
    CHECK(classify(deep, 2) == Area::Hud);
    // no VR session: everything as the game draws it
    const State off{};
    for (Area a : {Area::Hud, Area::ActionPrompt, Area::MessageAnchor, Area::Other}) CHECK(action(a, true, off) == UiAction::Game);
    // VR play: the HUD not shown, the rest as the game draws it
    const State play{true, true, false};
    CHECK(action(Area::Hud, false, play) == UiAction::Transparent);
    CHECK(action(Area::ActionPrompt, true, play) == UiAction::Game && action(Area::Other, false, play) == UiAction::Game);
    {  // a bomb in the hand: its "A Throw" prompt hidden (the hand throws it)
        State bomb = play;
        bomb.bombInHand = true;
        CHECK(action(Area::ActionPrompt, true, bomb) == UiAction::Transparent);
    }
    CHECK(action(Area::MessageAnchor, true, play) == UiAction::Game);
    // events and transitions (layouts shrunk): the dialogue's anchor at its 16:9 place, its contents as they are
    const State event{true, true, true};
    CHECK(action(Area::MessageAnchor, true, event) == UiAction::NativePlace);
    CHECK(action(Area::MessageAnchor, false, event) == UiAction::Game);
    CHECK(action(Area::Hud, false, event) == UiAction::Transparent);
    // the telescope's overlay: only while looking through it (not while the game fades it out after)
    const Link mask[] = {{"W_Wipe_00", false, false}, {"N_Mode_00", false, false}, {"N_All_00", true, false}};
    CHECK(classify(mask, 3, true) == Area::Telescope && classify(mask, 3, false) == Area::Other);
    CHECK(action(Area::Telescope, false, State{true, true, false, true}) == UiAction::Game);
    CHECK(action(Area::Telescope, false, State{true, true, true, false}) == UiAction::Transparent);
    CHECK(action(Area::Telescope, false, State{}) == UiAction::Game);
    // the 2D particle groups (the HUD's sparkles) not drawn in VR; the world's and the menus' are
    for (uint32_t g : {7u, 8u}) CHECK(!draws_particle_group(g, play) && draws_particle_group(g, off));
    for (uint32_t g : {0u, 2u, 6u, 9u, 10u, 11u, 12u}) CHECK(draws_particle_group(g, play));
    // the heat haze (the projection group, 4): drawn in play, never while dialogue or a cutscene is on in VR (it bent
    // the text), in first and third person, whatever the text's zoom (shrunk or not); the desktop always draws it; the
    // world's other groups stay in events
    State talk = play, talkThird{true, false, false}, talkUnshrunk = play;
    talk.shrunk = talk.event = true;
    talkThird.event = true;
    talkUnshrunk.event = true;  // (vr.textZoomOut 1: the layouts not shrunk)
    CHECK(draws_particle_group(4, play) && draws_particle_group(4, event));  // (shrunk alone: a menu scene, not an event)
    CHECK(!draws_particle_group(4, talk) && !draws_particle_group(4, talkThird) && !draws_particle_group(4, talkUnshrunk));
    State deskTalk{};
    deskTalk.event = true;  // (state() never sets it without a session; the rule draws it on the desktop regardless)
    CHECK(draws_particle_group(4, off) && draws_particle_group(4, deskTalk));
    for (uint32_t g : {0u, 1u, 2u, 3u, 5u, 6u, 9u, 10u}) CHECK(draws_particle_group(g, talk));
    // Disable heat haze (vr.heatHaze off, the default): the projection group not drawn in play either; the world's other
    // groups are, and the desktop keeps it
    State noHaze = play;
    noHaze.noHeatHaze = true;
    CHECK(!draws_particle_group(4, noHaze));
    for (uint32_t g : {0u, 1u, 2u, 3u, 5u, 6u, 9u, 10u}) CHECK(draws_particle_group(g, noHaze));
    State deskNoHaze{};
    deskNoHaze.noHeatHaze = true;  // (state() never sets it without a session)
    CHECK(draws_particle_group(4, deskNoHaze));
    CHECK(!Config{}.heatHaze);
    {
        Config h;
        CHECK(from_kv(h, "vr.heatHaze", "1") && h.heatHaze);
    }
    // third person: the game's HUD again, with its sparkles
    const State third{true, false, false};
    CHECK(action(Area::Hud, false, third) == UiAction::Game && !draws_particle_group(7, third) && !draws_particle_group(8, third));  // (its sparkles: stars in the view)
    CHECK(action(Area::Hud, false, State{true, false, true}) == UiAction::Game);  // in its events too
    // the boat's views (the Red Lion's, the Free Roam Camera): none of the game's HUD, not the action prompt either;
    // menus, dialogue and the rest as always
    State freeRoam = third;
    freeRoam.boatView = true;
    CHECK(action(Area::Hud, false, freeRoam) == UiAction::Transparent);
    CHECK(action(Area::ActionPrompt, true, freeRoam) == UiAction::Transparent);
    CHECK(action(Area::Other, false, freeRoam) == UiAction::Game && action(Area::MessageAnchor, false, freeRoam) == UiAction::Game);
    CHECK(action(Area::ActionPrompt, true, third) == UiAction::Game);  // (third person: the prompt as before)
    // its label: a part of the picture of its own, held in the view, at the top, ahead of the eyes
    {
        const WristRect& r = kWristRects[kWristLabel];
        CHECK(r.y >= kWristRects[kWristRupees].y + kWristRects[kWristRupees].h && r.y + r.h <= int(kWristHudHeight) &&
              r.x + r.w <= int(kWristHudWidth));
        const Pose p = label_pose();
        CHECK(p.position.z < -1.0f && p.position.y > 0.1f && near(p.position.x, 0));
        CHECK(near(rotate(p.orientation, Vec3{0, 0, 1}), Vec3{0, 0, 1}));  // facing the eyes
    }
}

static void test_config() {
    Config c;
    CHECK(c.enabled && c.turn == Turn::Smooth && c.detachedArms && !c.matchLinkBody);  // the tuned defaults
    c.enabled = false;
    CHECK(from_kv(c, "vr.enabled", "1") && c.enabled);
    CHECK(from_kv(c, "vr.turn", "smooth") && c.turn == Turn::Smooth);
    CHECK(!from_kv(c, "vr.turn", "spin") && c.turn == Turn::Smooth);      // unknown: kept
    CHECK(from_kv(c, "vr.snapDegrees", "500") && c.snapDegrees == 90);    // clamped
    CHECK(!from_kv(c, "vr.renderScale", "abc") && c.renderScale == 1);    // unreadable: default stays
    CHECK(!from_kv(c, "vr.renderScale", "nan") && c.renderScale == 1);
    CHECK(!from_kv(c, "vr.renderScale", "1.5x") && c.renderScale == 1);
    CHECK(!from_kv(c, "gyro.source", "controller"));                      // not ours
    // the swing thresholds keep their order
    Config d = from_saved({{"vr.swingStartSpeed", "2"}, {"vr.swingEndSpeed", "3"}});
    CHECK(d.swingEndSpeed < d.swingStartSpeed);
    // round trip
    Config e;
    e.leftHanded = true;
    e.unitsPerMetre = 120;
    e.moveDirection = MoveDirection::LeftHand;
    e.posture = Posture::Seated;
    const Config f = from_saved(to_kv(e));
    CHECK(f.leftHanded && f.unitsPerMetre == 120 && f.moveDirection == MoveDirection::LeftHand &&
          f.posture == Posture::Seated);
    CHECK(keys().size() == to_kv(Config{}).size());
    // the refresh rate (vr.refreshRate): one of five, 90 Hz by default; and the frame rate (vr.frameRate), by meaning,
    // Auto Mode by default (VR fork; docs/VR_FRAME_RATE.md)
    {
        CHECK(Config{}.refreshRate == 90 && Config{}.frameRate == FrameRate::Auto);
        CHECK(frame_rate_name(Config{}.frameRate, int(Config{}.refreshRate)) == "Auto Mode");
        // each rate's four choices, in order, named after it; the half cap computed, never 45 at every rate
        struct Row { int hz; const char* full; const char* half; int fullFps, halfFps; };
        for (const Row& r : {Row{60, "Capped 60 FPS", "Capped 30 FPS", 60, 30}, Row{72, "Capped 72 FPS", "Capped 36 FPS", 72, 36},
                             Row{80, "Capped 80 FPS", "Capped 40 FPS", 80, 40}, Row{90, "Capped 90 FPS", "Capped 45 FPS", 90, 45},
                             }) {
            const auto ch = frame_rate_choices(r.hz);
            CHECK(ch.size() == 4);
            CHECK(ch[0].rate == FrameRate::Unlocked && ch[0].name == "Unlocked FPS" && ch[0].fps == 0);
            CHECK(ch[1].rate == FrameRate::Full && ch[1].name == r.full && ch[1].fps == r.fullFps);
            CHECK(ch[2].rate == FrameRate::Half && ch[2].name == r.half && ch[2].fps == r.halfFps);
            CHECK(ch[3].rate == FrameRate::Auto && ch[3].name == "Auto Mode" && ch[3].fps == 0);
            CHECK(refresh_rate_name(r.hz) == std::to_string(r.hz) + " Hz");
        }
        // 120 Hz: Capped 60 FPS and Auto Mode only (VR fork, for now); chosen, Auto Mode; unlocked or full saved there:
        // Auto Mode
        {
            const auto ch = frame_rate_choices(120);
            CHECK(ch.size() == 2 && ch[0].rate == FrameRate::Half && ch[0].name == "Capped 60 FPS" && ch[0].fps == 60 &&
                  ch[1].rate == FrameRate::Auto && ch[1].name == "Auto Mode");
            for (FrameRate m : {FrameRate::Unlocked, FrameRate::Full, FrameRate::Half, FrameRate::Auto}) {
                Config h;
                h.frameRate = m;
                choose_refresh_rate(h, 120);
                CHECK(h.frameRate == FrameRate::Auto && frame_rate_name(h.frameRate, 120) == "Auto Mode");
                Config s;
                s.refreshRate = 120;
                s.frameRate = m;
                validate(s);
                CHECK(s.frameRate == (m == FrameRate::Half ? FrameRate::Half : FrameRate::Auto));
            }
        }
        CHECK(std::size(kRefreshRates) == 5);
        CHECK(int(FrameRate::Unlocked) == 0 && int(FrameRate::Half) == 1 && int(FrameRate::Auto) == 3 && int(FrameRate::Full) == 4);  // (= interp_pacing.h VrFrameMode)
        // a refresh rate chosen (VR fork): the default frame rate, Auto Mode (120 Hz: Capped 60); the same rate again
        // keeps the choice
        Config rc;
        rc.frameRate = FrameRate::Half;
        choose_refresh_rate(rc, 120);
        CHECK(rc.refreshRate == 120 && rc.frameRate == FrameRate::Auto);
        rc.frameRate = FrameRate::Half;
        choose_refresh_rate(rc, 72);
        CHECK(rc.refreshRate == 72 && rc.frameRate == FrameRate::Auto && frame_rate_name(rc.frameRate, 72) == "Auto Mode");
        rc.frameRate = FrameRate::Full;
        choose_refresh_rate(rc, 72);
        CHECK(rc.frameRate == FrameRate::Full);
        choose_refresh_rate(rc, 144);  // (not offered: nothing changes)
        CHECK(rc.refreshRate == 72 && rc.frameRate == FrameRate::Full);
        for (FrameRate m : {FrameRate::Unlocked, FrameRate::Full, FrameRate::Half}) {
            rc.frameRate = m;
            choose_refresh_rate(rc, rc.refreshRate == 60 ? 90 : 60);
            CHECK(rc.frameRate == FrameRate::Auto);
        }
        // saved and read back: every rate and every meaning
        for (int hz : kRefreshRates)
            for (FrameRate m : {FrameRate::Unlocked, FrameRate::Full, FrameRate::Half, FrameRate::Auto}) {
                if (hz == kHalfOnlyRefreshRate && m != FrameRate::Half && m != FrameRate::Auto) continue;  // (120 Hz)
                Config sc;
                sc.refreshRate = float(hz);
                sc.frameRate = m;
                const Config back = from_saved(to_kv(sc));
                CHECK(back.refreshRate == float(hz) && back.frameRate == m);
            }
        // older and unusable values: the default, Auto Mode (also unlocked from before format 8); 45 and 30 the
        // half cap; format 8's fixed 60, 90, 120 the full cap; a refresh rate not offered (0, the runtime's choice
        // of before; 144; 61), 90 Hz; one the runtime reports (89.9, 119.88) the menu's; other settings untouched
        CHECK(from_saved({{"vr.armIK", "1"}}).frameRate == FrameRate::Auto);
        // format 16: the title and file select's old default distance (2) goes to the new (3); another value stays
        CHECK(Config{}.menuZoomOut == 3.0f);
        CHECK(from_saved({{"vr.format", "15"}, {"vr.menuZoomOut", "2"}}).menuZoomOut == 3.0f);
        CHECK(from_saved({{"vr.format", "15"}, {"vr.menuZoomOut", "1.5"}}).menuZoomOut == 1.5f);
        CHECK(from_saved({{"vr.format", "16"}, {"vr.menuZoomOut", "2"}}).menuZoomOut == 2.0f);
        CHECK(from_saved({{"vr.format", "16"}, {"vr.menuZoomOut", "4"}}).menuZoomOut == 4.0f);
        CHECK(from_saved({{"vr.format", "7"}, {"vr.frameRate", "unlocked"}}).frameRate == FrameRate::Auto);
        CHECK(from_saved({{"vr.format", "9"}, {"vr.frameRate", "unlocked"}}).frameRate == FrameRate::Unlocked);
        for (const char* v : {"45", "30", "half"}) CHECK(from_saved({{"vr.frameRate", v}}).frameRate == FrameRate::Half);
        for (const char* v : {"60", "90", "120", "full"}) CHECK(from_saved({{"vr.frameRate", v}}).frameRate == FrameRate::Full);
        CHECK(from_saved({{"vr.frameRate", "fast"}}).frameRate == FrameRate::Auto);
        for (const char* v : {"0", "144", "61", "-5"}) CHECK(from_saved({{"vr.refreshRate", v}}).refreshRate == 90);
        CHECK(from_saved({{"vr.refreshRate", "89.9"}}).refreshRate == 90 && from_saved({{"vr.refreshRate", "119.88"}}).refreshRate == 120);
        CHECK(from_saved({{"vr.refreshRate", "72"}}).refreshRate == 72 && from_saved({{"vr.refreshRate", "80"}}).refreshRate == 80);
        const Config kept = from_saved({{"vr.refreshRate", "0"}, {"vr.leftHanded", "1"}, {"vr.snapDegrees", "45"}});
        CHECK(kept.leftHanded && kept.snapDegrees == 45 && kept.refreshRate == 90);
        Config orig;
        orig.frameRate = FrameRate::Original;
        validate(orig);
        CHECK(orig.frameRate == FrameRate::Half);
        // Detect Headset: the rate it runs selected, with the default frame rate, Auto Mode (whatever was chosen
        // before); 89.9 Hz is 90
        Config dc;
        dc.refreshRate = 72;
        dc.frameRate = FrameRate::Half;
        DetectedRefresh d = apply_detected_refresh(dc, 89.9f, {});
        CHECK(d.applied && d.hz == 90 && dc.refreshRate == 90 && dc.frameRate == FrameRate::Auto);
        CHECK(frame_rate_name(dc.frameRate, int(dc.refreshRate)) == "Auto Mode");
        dc.frameRate = FrameRate::Full;
        d = apply_detected_refresh(dc, 120.0f, {72, 80, 90, 120});
        CHECK(d.applied && dc.refreshRate == 120 && dc.frameRate == FrameRate::Auto && frame_rate_name(dc.frameRate, 120) == "Auto Mode");
        // a rate the menu has none for: one the runtime offers (the fastest), else nothing changes and nothing is
        // claimed; detection failed (no rate): nothing changes
        Config uc;
        uc.refreshRate = 72;
        uc.frameRate = FrameRate::Half;
        d = apply_detected_refresh(uc, 144.0f, {});
        CHECK(!d.applied && uc.refreshRate == 72 && uc.frameRate == FrameRate::Half && !d.message.empty());
        d = apply_detected_refresh(uc, 144.0f, {90, 120, 144});
        CHECK(d.applied && d.hz == 120 && uc.refreshRate == 120 && uc.frameRate == FrameRate::Auto);
        Config fc;
        fc.refreshRate = 80;
        fc.frameRate = FrameRate::Auto;
        d = apply_detected_refresh(fc, 0.0f, {});
        CHECK(!d.applied && fc.refreshRate == 80 && fc.frameRate == FrameRate::Auto);
    }
    // detached arms (vr.detachedArms): on by default, so also in a file from before it (no key); saved and read
    // back either way; an unreadable value keeps what was there
    CHECK(Config{}.detachedArms && from_saved({{"vr.armIK", "1"}}).detachedArms);
    Config da;
    da.detachedArms = false;
    CHECK(!from_saved(to_kv(da)).detachedArms && from_saved(to_kv(Config{})).detachedArms);
    Config db;
    CHECK(from_kv(db, "vr.detachedArms", "1") && db.detachedArms);
    CHECK(!from_kv(db, "vr.detachedArms", "maybe") && db.detachedArms);
    CHECK(from_kv(db, "vr.detachedArms", "0") && !db.detachedArms);
}

static void test_bindings() {
    Bindings b;
    CHECK(b.buttons(1u << int(Action::OffTrigger)) == pad::kZL);
    CHECK(b.buttons((1u << int(Action::MainSecondary)) | (1u << int(Action::Menu))) == (pad::kB | pad::kPlus));
    CHECK(b.from_kv("vr.bind.offTrigger", "L") && b.buttons(1u << int(Action::OffTrigger)) == pad::kL);
    CHECK(!b.from_kv("vr.bind.offTrigger", "Q") && b.button[int(Action::OffTrigger)] == pad::kL);
    CHECK(!b.from_kv("vr.bind.nothing", "A"));
    CHECK(b.from_kv("vr.bind.menu", "") && b.buttons(1u << int(Action::Menu)) == 0);  // unbound
    Bindings c;
    for (const auto& [k, v] : b.to_kv()) c.from_kv(k, v);
    for (int i = 0; i < kActionCount; ++i) CHECK(c.button[i] == b.button[i]);
    RecenterGesture g;
    CHECK(!g.update(true, false, 1.0f));
    CHECK(!g.update(true, true, 0.3f));
    CHECK(g.update(true, true, 0.4f));   // 0.7 s >= hold
    CHECK(!g.update(true, true, 0.4f));  // once per hold
    CHECK(!g.update(false, true, 0.1f) && !g.holding());
}

static void test_locomotion() {
    // with the game camera looking where the head looks, the stick is unchanged
    Stick s = remap_stick({0.3f, 0.8f}, 1.0f, 1.0f);
    CHECK(near(s.x, 0.3f) && near(s.y, 0.8f));
    // head 90 degrees left of the game camera: stick forward must become stick left for the game
    s = remap_stick({0, 1}, kPi / 2, 0);
    CHECK(near(s.x, -1.0f) && near(s.y, 0));
    s = remap_stick({1, 0}, kPi, 0);  // head facing the camera: right becomes left
    CHECK(near(s.x, -1.0f) && near(s.y, 0));
    // the stick's length is kept
    s = remap_stick({0.6f, 0.6f}, 0.7f, -2.1f);
    CHECK(near(s.x * s.x + s.y * s.y, 0.72f, 1e-4f));
    // snap turning: one turn per push, right lowers the heading
    Config c;
    c.turn = Turn::Snap;  // (smooth is the default)
    Turner t;
    CHECK(near(t.update(1.0f, 0.016f, c), -30 * kPi / 180));
    CHECK(t.update(1.0f, 0.016f, c) == 0);  // still held
    CHECK(t.update(0.0f, 0.016f, c) == 0);  // released
    CHECK(near(t.update(-0.9f, 0.016f, c), 30 * kPi / 180));
    // smooth turning: proportional, frame-rate independent
    c.turn = Turn::Smooth;
    Turner u;
    const float a = u.update(1.0f, 0.01f, c), b = u.update(1.0f, 0.02f, c);
    CHECK(near(b, 2 * a) && a < 0);
    CHECK(u.update(0.1f, 0.02f, c) == 0);  // dead zone
    // every frame its share at every speed the setting offers (20 to 360 degrees a second), full and partial stick: one
    // second of 90 Hz frames turns exactly the set speed, each frame the same step (vr_game.cpp CameraOverride turns
    // each frame by the time between the frames' display times)
    for (const float speed : {20.0f, 60.0f, 120.0f, 240.0f, 360.0f})
        for (const float stick : {1.0f, -0.6f}) {
            Config sc;
            sc.turn = Turn::Smooth;
            sc.smoothDegreesPerSecond = speed;
            Turner st;
            const float share = (std::fabs(stick) - Turner::kDeadZone) / (1.0f - Turner::kDeadZone);
            const float step = share * speed * kPi / 180.0f / 90.0f;
            float total = 0;
            for (int f = 0; f < 90; ++f) {
                const float d = st.update(stick, 1.0f / 90.0f, sc);
                CHECK(near(std::fabs(d), step, 1e-5f));
                total += d;
            }
            CHECK(near(std::fabs(total), share * speed * kPi / 180.0f, 1e-3f));
            CHECK((total < 0) == (stick > 0));  // right lowers the heading
        }
    // the head may not leave the body: horizontal offset limited, height free
    const Vec3 o = limit_head_offset({0.6f, -0.3f, 0.8f}, 0.5f);
    CHECK(near(std::sqrt(o.x * o.x + o.z * o.z), 0.5f) && near(o.y, -0.3f));
    CHECK(near(limit_head_offset({0.1f, 0, 0.1f}, 0.5f), {0.1f, 0, 0.1f}));
}

static void test_ik() {
    const ArmChain arm{0.3f, 0.25f};
    const Vec3 shoulder{0, 1.4f, 0}, pole{0, -1, 0};
    // reachable: the wrist is at the target and both bones keep their lengths
    ArmSolution s = solve_arm(shoulder, {0.3f, 1.2f, -0.2f}, pole, arm);
    CHECK(!s.clamped && near(s.wrist, {0.3f, 1.2f, -0.2f}));
    CHECK(near(length(s.elbow - s.shoulder), arm.upper, 1e-4f));
    CHECK(near(length(s.wrist - s.elbow), arm.lower, 1e-4f));
    CHECK(s.elbow.y < (s.shoulder.y + s.wrist.y) * 0.5f);  // bends towards the pole (down)
    // out of reach: clamped onto the reach limit along the same direction, never stretched
    s = solve_arm(shoulder, {0, 1.4f, -3}, pole, arm);
    CHECK(s.clamped);
    CHECK(near(length(s.wrist - shoulder), (arm.upper + arm.lower) * kMaxReach, 1e-4f));
    CHECK(near(normalize(s.wrist - shoulder), {0, 0, -1}));
    CHECK(near(length(s.elbow - s.shoulder), arm.upper, 1e-4f) && near(length(s.wrist - s.elbow), arm.lower, 1e-3f));
    // inside the shoulder: pushed out to the minimum
    s = solve_arm(shoulder, shoulder + Vec3{0, 0, -0.001f}, pole, arm);
    CHECK(s.clamped && length(s.wrist - shoulder) >= std::fabs(arm.upper - arm.lower));
    CHECK(finite(s.elbow));
    // target exactly on the pole line: still a finite elbow
    s = solve_arm(shoulder, shoulder + Vec3{0, -0.4f, 0}, pole, arm);
    CHECK(finite(s.elbow) && near(length(s.elbow - s.shoulder), arm.upper, 1e-4f));
    // the body's facing follows the head: a 30 degree threshold, all the way to within 5 degrees, the short way round
    // (docs/VR_BODY_FOLLOW.md)
    {
        const float deg = kPi / 180.0f, enter = 30 * deg, dt = 1.0f / 90.0f;
        auto settle = [&](BodyFollow& b, float head, float seconds, float step) {
            for (float t = 0; t < seconds; t += step) b.update(head, step, enter);
        };
        for (float sign : {1.0f, -1.0f}) {
            for (float turn : {10.0f, 20.0f, 29.0f}) {  // inside the threshold: the body stays
                BodyFollow b;
                settle(b, sign * turn * deg, 2, dt);
                CHECK(b.yaw == 0 && !b.following);
            }
            for (float turn : {31.0f, 45.0f, 90.0f, 179.0f}) {  // beyond it: smoothly round to within 5 degrees
                BodyFollow b;
                b.update(sign * turn * deg, dt, enter);
                CHECK(b.following && std::fabs(b.yaw) < turn * deg);  // not a snap
                CHECK(b.yaw * sign > 0);  // the head's side
                settle(b, sign * turn * deg, 2, dt);
                CHECK(!b.following && std::fabs(wrap_radians(sign * turn * deg - b.yaw)) <= BodyFollow::kExit + 1e-4f);
                const float rest = b.yaw;
                settle(b, sign * turn * deg, 5, dt);  // the head still: no drift
                CHECK(b.yaw == rest);
                // hysteresis: small head moves round the new facing do not start it again
                for (int i = 0; i < 200; ++i) b.update(sign * turn * deg + 0.15f * std::sin(i * 0.3f), dt, enter);
                CHECK(b.yaw == rest);
            }
        }
        // most of a 90 degree turn within half a second, all of it within a second
        BodyFollow q;
        settle(q, 90 * deg, 0.5f, dt);
        CHECK(std::fabs(90 * deg - q.yaw) < 20 * deg);
        settle(q, 90 * deg, 0.5f, dt);
        CHECK(!q.following);
        // across +-180: the short way (170 to -170 is 20 degrees, turned positive)
        BodyFollow w;
        w.set(170 * deg);
        w.update(-135 * deg, dt, enter);
        CHECK(w.following && w.yaw > 170 * deg - 1e-4f);  // moving towards +180, not back through 0
        settle(w, -135 * deg, 2, dt);
        CHECK(std::fabs(wrap_radians(-135 * deg - w.yaw)) <= BodyFollow::kExit + 1e-4f);
        // frame rate independent: 30, 90 and 144 Hz updates are within a few degrees of each other after 0.2 s
        float at[3];
        const float rates[3] = {30, 90, 144};
        for (int i = 0; i < 3; ++i) {
            BodyFollow f;
            settle(f, 90 * deg, 0.2f, 1.0f / rates[i]);
            at[i] = f.yaw;
        }
        CHECK(std::fabs(at[0] - at[1]) < 6 * deg && std::fabs(at[1] - at[2]) < 3 * deg);
        // Link's facing: the view's turn into the game plus the body's yaw. A snap or smooth turn changes only the
        // first, so Link turns by exactly that once; following the head after it changes nothing more
        const float map = 1.0f, body = 0.4f;
        CHECK(near(link_facing(map, body), 1.4f, 1e-5f));
        CHECK(near(link_facing(map + 30 * deg, body) - link_facing(map, body), 30 * deg, 1e-5f));
        CHECK(near(body_yaw_of(link_facing(map, body), map), body, 1e-5f));  // the game's heading back: the same body
        CHECK(near(link_facing(3.0f, 0.5f), wrap_radians(3.5f), 1e-5f));       // wrapped
        // Detached Arms: the body's target turns towards the hands the farther out they reach (forward is -z)
        CHECK(body_follow_yaw(0.3f, false, {1, 0, 0}) == 0.3f);                        // no hands: the head's
        CHECK(body_follow_yaw(0.3f, true, {0.05f, -0.4f, -0.05f}) == 0.3f);            // hands near the body
        CHECK(near(body_follow_yaw(0, true, {-0.6f, 0, 0}), kPi / 2, 1e-4f));          // reaching far left: left
        CHECK(near(body_follow_yaw(0, true, {0, 0, -0.5f}), 0, 1e-4f));                // ahead: ahead
        const float half = body_follow_yaw(0, true, {0.275f, 0, 0});                   // halfway out to the right
        CHECK(half < -0.2f && half > -kPi / 2 + 0.2f);
        CHECK(std::fabs(body_follow_yaw(3.0f, true, {0.6f * std::sin(-3.0f), 0, -0.6f * std::cos(-3.0f)})) > 2.9f);  // across +-180
        // the game keeps Link's facing in everything but standing (wait 4, free wait 5)
        CHECK(body_follow_proc(4) && body_follow_proc(5));
        for (uint32_t proc : {6u, 7u, 8u, 9u, 0x36u, 0x76u, 0x83u}) CHECK(!body_follow_proc(proc));
        // ladders: the view's base turns to Link (docs/VR_BODY_FOLLOW.md, "Ladders")
        CHECK(rig_follows_link_proc(0x38) && rig_follows_link_proc(0x3A) && rig_follows_link_proc(0x3C));
        CHECK(!rig_follows_link_proc(0x37) && !rig_follows_link_proc(0x3D) && !rig_follows_link_proc(4));
        CHECK(near(link_facing(rig_heading_for(2.0f, 0.7f), 0.7f), 2.0f, 1e-5f));  // the inverse of link_facing
        // the turn: the short way across +-180, no overshoot, exactly there at the end, frame-rate independent
        float y = 170 * deg;
        y = turn_towards(y, -170 * deg, 0.01f);
        CHECK(y > 170 * deg);  // towards +180, not back through 0
        for (int i = 0; i < 400; ++i) y = turn_towards(y, -170 * deg, 0.01f);
        CHECK(near(wrap_radians(y + 170 * deg), 0, 1e-5f));
        float a30 = 0, a90 = 0;
        for (int i = 0; i < 15; ++i) a30 = turn_towards(a30, kPi * 0.99f, 1.0f / 30.0f);
        for (int i = 0; i < 45; ++i) a90 = turn_towards(a90, kPi * 0.99f, 1.0f / 90.0f);
        CHECK(std::fabs(a30 - a90) < 6 * deg);
        float o = 0;
        for (int i = 0; i < 1000; ++i) { o = turn_towards(o, 0.5f, 0.016f); CHECK(o <= 0.5f + 1e-5f); }
        // a descent: walking up to the top (the game turns him, the body is his heading), then he turns round to the
        // ladder; the view's base turns until his facing is the body's, the head looking round changes nothing, and
        // leaving the ladder leaves the base where it is (no snap back, nothing kept)
        {
            float map = 0.3f;
            BodyFollow body;
            const float walking = 1.0f;
            body.set(body_yaw_of(walking, map));
            const float onLadder = wrap_radians(walking + kPi);  // procLadderDownStart_init: shape_angle.y - 0x8000
            for (int i = 0; i < 90; ++i) map = turn_towards(map, rig_heading_for(onLadder, body.yaw), 1.0f / 90.0f);
            CHECK(std::fabs(wrap_radians(link_facing(map, body.yaw) - onLadder)) < 5 * deg);  // within a second
            for (int i = 0; i < 180; ++i) map = turn_towards(map, rig_heading_for(onLadder, body.yaw), 1.0f / 90.0f);
            CHECK(near(wrap_radians(link_facing(map, body.yaw) - onLadder), 0, 1e-4f));
            const float settled = map;
            for (int i = 0; i < 90; ++i) map = turn_towards(map, rig_heading_for(onLadder, body.yaw), 1.0f / 90.0f);
            CHECK(map == settled);  // stays put: no drift while he climbs
            CHECK(near(map, wrap_radians(0.3f + kPi), 1e-4f));  // exactly the half turn, once
        }
        // lock-on (docs/VR_BODY_FOLLOW.md, "Lock-on"): locked onto an actor (8, 9) starts it; Z with nothing to lock
        // onto (7), moving, standing and attacks do not; side steps, the back jump and the guard keep it once begun
        CHECK(lock_on_proc(8) && lock_on_proc(9));
        for (uint32_t proc : {4u, 5u, 6u, 7u, 0x0Au, 0x22u, 0x41u}) CHECK(!lock_on_proc(proc));
        for (uint32_t proc : {8u, 9u, 0x0Au, 0x0Bu, 0x0Cu, 0x22u, 0x23u}) CHECK(lock_on_keeps(proc));
        for (uint32_t proc : {4u, 6u, 7u, 0x41u}) CHECK(!lock_on_keeps(proc));
        for (uint32_t proc : {0x5Du, 0x60u, 0x62u, 0x64u}) CHECK(lock_on_keeps(proc) && !lock_on_proc(proc));  // (a parry's moves)
        {
            LockOnView v;
            CHECK(!v.update(0x0A, 0.4f) && !v.active);              // a side step alone is no lock-on
            CHECK(v.update(8, 0.4f) && near(v.anchor, 0.4f, 1e-6f));  // begun: the head's yaw then
            CHECK(v.update(0x0A, 1.5f) && v.update(9, -2.0f) && near(v.anchor, 0.4f, 1e-6f));  // head turns: the same
            CHECK(!v.update(7, 0.0f) && !v.active);                 // the target lost (Z still held): ended
            CHECK(v.update(9, -1.0f) && near(v.anchor, -1.0f, 1e-6f));  // a new lock-on: the head's yaw now
            v.reset();
            CHECK(!v.active);
        }
        // the view during a lock-on: walking (the body Link's heading), then locked onto a target to his right; the
        // base turns until his facing (the game's, to the target) maps onto the anchor, the head looking round
        // meanwhile changes nothing, a moving target and a switch are followed, and the end leaves the base (no snap)
        for (const float rate : {30.0f, 90.0f, 144.0f}) {
            float map = 0.3f;
            BodyFollow body;
            const float walking = 1.0f;
            body.set(body_yaw_of(walking, map));
            LockOnView v;
            const float headAtLock = 0.2f;
            CHECK(v.update(8, headAtLock));
            float target = wrap_radians(walking + 1.4f);  // the game turns him to the target
            const auto step = [&](float head) {
                v.update(9, head);
                map = turn_towards(map, rig_heading_for(target, v.anchor), 1.0f / rate);
                body.set(body_yaw_of(target, map));
            };
            for (int i = 0; i < int(rate); ++i) step(headAtLock + 0.8f * std::sin(i * 0.2f));  // a second, looking round
            CHECK(std::fabs(wrap_radians(link_facing(map, v.anchor) - target)) < 5 * deg);
            for (int i = 0; i < int(rate); ++i) step(-1.0f);
            CHECK(near(wrap_radians(link_facing(map, v.anchor) - target), 0, 1e-4f));  // in front of where you looked
            CHECK(near(wrap_radians(body.yaw - headAtLock), 0, 1e-4f));               // the shoulders face it too
            const float settled = map;
            for (int i = 0; i < int(rate); ++i) step(2.5f);  // the head alone: the view's base stays put
            CHECK(map == settled);
            for (int i = 0; i < 2 * int(rate); ++i) {         // the target moves round him at 40 degrees a second
                target = wrap_radians(target + 40 * deg / rate);
                step(0);
            }
            CHECK(std::fabs(wrap_radians(link_facing(map, v.anchor) - target)) < 12 * deg);  // kept up, a little behind
            target = wrap_radians(target - 2.0f);              // switched to another target behind him
            for (int i = 0; i < 2 * int(rate); ++i) step(0);
            CHECK(near(wrap_radians(link_facing(map, v.anchor) - target), 0, 1e-4f));
            const float atEnd = map;
            CHECK(!v.update(4, 0));                            // the lock ends: the base stays where it is
            CHECK(map == atEnd);
        }
        // ledges (docs/VR_BODY_FOLLOW.md, "Ledges"): every hang action, nothing next to them
        for (uint32_t proc = 0x2B; proc <= 0x31; ++proc) CHECK(hang_proc(proc) && rig_follows_link_proc(proc));
        CHECK(!hang_proc(0x2A) && !hang_proc(0x32) && !rig_follows_link_proc(0x2A) && !rig_follows_link_proc(0x32));
        CHECK(!hang_proc(0x27) && !rig_follows_link_proc(0x27));  // falling off: the view is free again
        // sidling along a wall (docs/VR_BODY_FOLLOW.md, "Walls"): its four actions, the base following his animated
        // head's yaw (from head joint to eye point), not his model's or the way he slides; flat or broken: the model's
        for (uint32_t proc = 0x13; proc <= 0x16; ++proc) CHECK(sidle_proc(proc) && rig_follows_link_proc(proc));
        CHECK(!sidle_proc(0x12) && !sidle_proc(0x17) && !rig_follows_link_proc(0x12) && !rig_follows_link_proc(0x17));
        CHECK(link_frame_angle(0x15, int16_t(0x1000 + 0x4000), 0x1000) == 0x1000);  // his frame: not the slide's
        {
            const float out = 1.0f;                                   // his model faces away from the wall
            const Vec3 lookLeft = Vec3{std::sin(out + 1.2f), 0.3f, std::cos(out + 1.2f)} * 18.0f;  // head along it
            CHECK(near(rig_facing(0x15, out, lookLeft), wrap_radians(out + 1.2f), 1e-4f));
            CHECK(near(rig_facing(0x15, out, Vec3{lookLeft.x, -9.0f, lookLeft.z}), wrap_radians(out + 1.2f), 1e-4f));  // pitch ignored
            const Vec3 lookRight = Vec3{std::sin(out - 1.2f), 0, std::cos(out - 1.2f)} * 18.0f;
            CHECK(near(rig_facing(0x16, out, lookRight), wrap_radians(out - 1.2f), 1e-4f));
            CHECK(rig_facing(0x15, out, Vec3{0.5f, 18, 0.5f}) == out);          // no clear facing: the model's
            CHECK(rig_facing(0x15, out, Vec3{NAN, 0, 1}) == out);
            CHECK(rig_facing(0x2F, out, lookLeft) == out && rig_facing(0x3C, out, lookLeft) == out);  // ledge, ladder
            // the base turns to the head's yaw and settles, sliding either way; leaving keeps it (no snap back)
            float map = 0.2f;
            BodyFollow body;
            body.set(body_yaw_of(out, map));
            for (const Vec3& look : {lookLeft, lookRight, lookLeft}) {
                const float target = rig_facing(0x15, out, look);
                for (int i = 0; i < 270; ++i) map = turn_towards(map, rig_heading_for(target, body.yaw), 1.0f / 90.0f);
                CHECK(near(wrap_radians(link_facing(map, body.yaw) - target), 0, 1e-4f));
            }
        }
        // his frame on a ledge is his model's facing, not the way he slides (procHangMove_init: shape_angle.y +- 0x4000);
        // everywhere else the way he moves, as before
        CHECK(link_frame_angle(0x2F, int16_t(0x1000 + 0x4000), 0x1000) == 0x1000);
        CHECK(link_frame_angle(0x2F, int16_t(0x1000 - 0x4000), 0x1000) == 0x1000);
        CHECK(link_frame_angle(0x2E, 0x1000, 0x1000) == 0x1000);
        CHECK(link_frame_angle(6, 0x2000, 0x1000) == 0x2000);     // moving: as before
        CHECK(link_frame_angle(7, int16_t(0x1000 + 0x8000), 0x1000) == 0x1000);  // stepping back (Z): his model's facing
        CHECK(link_frame_angle(0x3C, 0x2000, 0x1000) == 0x2000);  // a ladder: as before
        // stepping off an edge (he turns round to the wall), then sliding left, right and back: the view's base turns
        // to his facing once and stays there while he slides either way, whatever the head does; letting go leaves it
        {
            float map = -0.4f;
            BodyFollow body;
            const float walking = 2.5f;
            body.set(body_yaw_of(walking, map));
            const int16_t wall = int16_t(int(walking / kPi * 32768.0f) - 0x8000);
            const float facing = s16_to_radians(wall);
            for (int i = 0; i < 270; ++i) map = turn_towards(map, rig_heading_for(facing, body.yaw), 1.0f / 90.0f);
            CHECK(near(wrap_radians(link_facing(map, body.yaw) - facing), 0, 1e-4f));
            const float settled = map;
            for (int16_t travel : {int16_t(wall + 0x4000), int16_t(wall - 0x4000), int16_t(wall + 0x4000), wall}) {
                const float frame = s16_to_radians(link_frame_angle(0x2F, travel, wall));
                CHECK(near(wrap_radians(frame - facing), 0, 1e-6f));  // the frame never swings a quarter round
                for (int i = 0; i < 90; ++i)
                    map = turn_towards(map, rig_heading_for(s16_to_radians(wall), body.yaw), 1.0f / 90.0f);
                CHECK(map == settled);
            }
            // falling off (proc 0x27): no longer Link's; the body's facing is still his, so nothing jumps
            CHECK(!rig_follows_link_proc(0x27));
            CHECK(near(wrap_radians(body_yaw_of(facing, map) - body.yaw), 0, 1e-4f));
        }
        // set (the game turned him) stops any follow at once
        BodyFollow g;
        g.update(90 * deg, dt, enter);
        g.set(1.0f);
        CHECK(!g.following && near(g.yaw, 1.0f, 1e-5f));
    }
    // an item moved to Link's other hand (the shield from his right to his left): mirrored with his hands' mirror,
    // the item's axis across his body turned back. Held upright with its face forward in the right hand, it is upright
    // with its face forward in the left, mirrored across his body; applied unchanged (the bug) it was not
    {
        const Vec3 right{1, 0, 0};                                   // Link's right; he faces +Z
        const Quat R = axis_angle(normalize(Vec3{1, 2, 3}), 0.7f);   // his right hand's frame
        const Vec3 sign{1, 1, -1};                                   // his rig's hand mirror (a rotation again)
        auto reflect = [&](Vec3 v) { return v - right * (2.0f * dot(v, right)); };
        const Vec3 lc[3] = {reflect(rotate(R, {1, 0, 0})) * sign.x, reflect(rotate(R, {0, 1, 0})) * sign.y,
                            reflect(rotate(R, {0, 0, 1})) * sign.z};
        const Quat L = normalize(from_basis(lc[0], lc[1], lc[2]));   // his left hand's frame
        CHECK(near(dot(cross(lc[0], lc[1]), lc[2]), 1.0f, 1e-4f));
        const Quat S = from_basis({1, 0, 0}, {0, 1, 0}, {0, 0, 1});  // the shield: x across, y its top, z its face
        const Vec3 handPos{0.2f, 1.0f, 0.1f}, shieldPos{0.25f, 0.95f, 0.18f};
        const Quat localRot = normalize(conjugate(R) * S);
        const Vec3 localPos = rotate(conjugate(R), shieldPos - handPos);
        const int across = across_axis(S, right);
        CHECK(across == 0);
        Quat mr;
        Vec3 mp;
        mirror_held(localRot, localPos, sign, across, mr, mp);
        const Quat W = normalize(L * mr);
        const Vec3 leftHand = reflect(handPos);
        CHECK(dot(rotate(W, {0, 1, 0}), {0, 1, 0}) > 0.999f);  // its top up
        CHECK(dot(rotate(W, {0, 0, 1}), {0, 0, 1}) > 0.999f);  // its face forward, not backward
        CHECK(length(leftHand + rotate(L, mp) - reflect(shieldPos)) < 1e-4f);  // in the left hand, mirrored
        const Quat old = normalize(L * localRot);  // the right hand's attachment applied unchanged
        CHECK(dot(rotate(old, {0, 1, 0}), {0, 1, 0}) < 0.99f || dot(rotate(old, {0, 0, 1}), {0, 0, 1}) < 0.99f);
        // a wrist turn: the hand and the shield turn together, nothing flips
        const Quat twist = axis_angle({0, 0, 1}, 1.2f);
        const Quat Wt = normalize(normalize(twist * L) * mr);
        CHECK(near(dot(rotate(Wt, {0, 1, 0}), rotate(twist, rotate(W, {0, 1, 0}))), 1.0f, 1e-4f));
        // the sword's case: the blade's axis is never the one turned back
        CHECK(across_axis(S, right, {1, 0, 0}) != 0);
    }
    // shoulders turn with the body's yaw, not the head's pitch
    const Vec3 r = shoulder_position(Pose{axis_angle({1, 0, 0}, 0.8f), {0, 1.6f, 0}}, 0, 1);
    CHECK(r.x > 0 && r.y < 1.6f);
}

// a blade swung through an arc about the shoulder: the arm carries the hand (the blade's root) round with it
static Blade arc_blade(float angle) {
    const Vec3 root{0.45f * std::sin(angle), 1.2f, -0.45f * std::cos(angle)};
    const Vec3 dir = normalize(Vec3{std::sin(angle), 0, -std::cos(angle)});
    return {root, root + dir * 0.85f};
}

// the Deku Leaf's quick swings up or down (vr_swing.h FlapDetector)
static void test_flap() {
    Config c;
    FlapDetector f;
    f.configure(c);
    const float dt = 1.0f / 90.0f;
    auto run = [&](Vec3 from, Vec3 velocity, int frames) {
        int got = 0, n = 0;
        for (int i = 0; i <= frames; ++i)
            if (const int r = f.update(from + velocity * (float(i) * dt), dt)) got = r, ++n;
        return n == 1 ? got : n ? 99 : 0;
    };
    CHECK(run({0, 1, -0.4f}, {0, 0.3f, 0}, 60) == 0);    // slow: none
    f.reset();
    CHECK(run({0, 1, -0.4f}, {0, 3.2f, 0}, 12) == 1);    // quick, up: once
    f.reset();
    CHECK(run({0, 1, -0.4f}, {0, -3.2f, 0}, 12) == -1);  // quick, down: once
    f.reset();
    CHECK(run({0, 1, -0.4f}, {3.2f, 0.3f, 0}, 12) == 0);  // quick, across: none
    f.reset();
    CHECK(run({0, 1, -0.4f}, {0, -3.2f, 0}, 3) == 0);    // quick, but not far enough
    // a second swing only after the hand has slowed
    f.reset();
    run({0, 1, -0.4f}, {0, -3.2f, 0}, 12);
    for (int i = 0; i < 10; ++i) f.update({0, 0.57f, -0.4f}, dt);  // still, but too soon (kCooldown)
    CHECK(run({0, 0.57f, -0.4f}, {0, 3.2f, 0}, 12) == 0);
    for (int i = 0; i < 90; ++i) f.update({0, 0.99f, -0.4f}, dt);  // still for longer (kCooldown)
    CHECK(run({0, 0.99f, -0.4f}, {0, -3.2f, 0}, 12) == -1);
}
static void test_swing() {
    Config c;
    SwingDetector d(c);
    const float dt = 1.0f / 90.0f;
    // idle movement: slow, never a swing
    for (int i = 0; i < 90; ++i) {
        const SwingEvent e = d.update(arc_blade(0.2f * std::sin(i * 0.05f)), dt, 0);
        CHECK(!e.started);
    }
    CHECK(d.phase() == SwingPhase::Idle);
    // velocity: the hand (the blade's root) moving 1 cm per 1/90 s is 0.9 m/s; the tip alone moving is none (a wrist turn)
    d.reset();
    d.update({{0, 1, 0}, {0, 1, -0.85f}}, dt, 0);
    d.update({{0.01f, 1, 0}, {0.01f, 1, -0.85f}}, dt, 0);
    CHECK(near(d.speed(), 0.9f, 1e-3f) && near(d.tip_velocity(), {0.9f, 0, 0}, 1e-3f));
    d.update({{0.01f, 1, 0}, {0.3f, 1, -0.8f}}, dt, 0);
    CHECK(d.speed() == 0);
    // a wrist turn alone (the blade swung 120 degrees about the hand, fast): never a swing, never an attack
    {
        SwingDetector w(c);
        int any = 0;
        for (int i = 0; i < 30; ++i) {
            const float a = -1.0f + 0.07f * float(i);
            const Vec3 root{0.2f, 1.1f, -0.3f};
            const SwingEvent s = w.update({root, root + normalize(Vec3{std::sin(a), 0, -std::cos(a)}) * 0.85f}, dt, 0);
            any += s.started + s.full;
        }
        CHECK(any == 0);
    }
    // a fast horizontal swing to the right: starts after swingMinTime, classified, ends with hysteresis
    d.reset();
    float angle = -1.2f;
    int started = 0, ended = 0, startedAt = -1;
    SwingKind kind = SwingKind::Vertical;
    int side = 0;
    for (int i = 0; i < 60; ++i) {
        const float rate = i < 25 ? 8.0f : 0.5f;  // rad/s: a swing, then nearly still
        angle += rate * dt;
        const SwingEvent e = d.update(arc_blade(angle), dt, 0);
        if (e.started) { ++started; startedAt = i; kind = e.kind; side = e.side; }
        if (e.ended) ++ended;
    }
    CHECK(started == 1 && ended == 1);
    // not on the first fast frame. (This arc starts moving mostly forward, so since 2026-11-05 it starts as a stab
    // would: after a quarter of swingMinTime, vr_swing.h kStabForward; still not on the first fast frame)
    CHECK(startedAt >= 2 && startedAt >= int(0.25f * c.swingMinTime / dt) - 1);
    CHECK(kind == SwingKind::Horizontal && side == 1);
    // cooldown: a second swing the same way does not start until the cooldown is over
    {
        SwingDetector e(c);
        float a = -1.0f;
        int starts = 0;
        for (int i = 0; i < 12; ++i) { a += 8.0f * dt; starts += e.update(arc_blade(a), dt, 0).started; }
        for (int i = 0; i < 3; ++i) e.update(arc_blade(a), dt, 0);  // stop: ends, cooldown starts
        for (int i = 0; i < 6; ++i) { a += 8.0f * dt; starts += e.update(arc_blade(a), dt, 0).started; }
        CHECK(starts == 1);  // within 0.25 s of the end
        CHECK(e.phase() != SwingPhase::Active);
    }
    // back the other way (TPVR's reversal re-arm): after a stop within the cooldown, and in a fast back and forth that
    // never slows down, each way is a swing
    {
        SwingDetector e(c);
        float a = -1.0f;
        int starts = 0, ends = 0;
        for (int i = 0; i < 12; ++i) { a += 8.0f * dt; starts += e.update(arc_blade(a), dt, 0).started; }
        for (int i = 0; i < 3; ++i) ends += e.update(arc_blade(a), dt, 0).ended;
        for (int i = 0; i < 12; ++i) { a -= 8.0f * dt; starts += e.update(arc_blade(a), dt, 0).started; }
        CHECK(starts == 2 && ends == 1);
        SwingDetector f(c);
        float b = -0.6f;
        starts = ends = 0;
        for (int k = 0; k < 4; ++k)  // four sweeps at 8 rad/s, turning at the ends without stopping
            for (int i = 0; i < 14; ++i) {
                b += (k % 2 ? -8.0f : 8.0f) * dt;
                const SwingEvent s = f.update(arc_blade(b), dt, 0);
                starts += s.started;
                ends += s.ended;
            }
        CHECK(starts == 4 && ends == 3);
    }
    // a full swing: once the blade has turned through swingFullDegrees (35) or the tip carried swingFullDistance (0.3 m),
    // once per swing, across or down; a short flick, a thrust, an upward swing and a wind-up never are
    {
        SwingDetector f(c);
        float a = -0.6f;
        int fulls = 0, fullAt = -1, startAt = -1, side = 0;
        for (int i = 0; i < 30; ++i) {  // 8 rad/s for 30 frames: 2.7 rad of blade turn, tip 0.85 m out
            a += 8.0f * dt;
            const SwingEvent s = f.update(arc_blade(a), dt, 0);
            if (s.started) startAt = i;
            if (s.full) { ++fulls; fullAt = i; side = s.fullSide; CHECK(s.fullKind == SwingKind::Horizontal); }
        }
        CHECK(fulls == 1 && side == 1 && startAt >= 0);
        // by swingAcrossDistance of the hand's travel (0.45 m from the shoulder at 8 rad/s: 3.6 m/s) from the swing's start
        CHECK(fullAt - startAt <= int(c.swingAcrossDistance / (3.6f * dt)) + 2);
        SwingDetector g(c);  // a short swing: 9 degrees about the shoulder, 0.07 m of the hand's travel
        float b = 0;
        fulls = 0;
        for (int i = 0; i < 2; ++i) { b += 8.0f * dt; fulls += g.update(arc_blade(b), dt, 0).full; }
        for (int i = 0; i < 30; ++i) fulls += g.update(arc_blade(b), dt, 0).full;
        CHECK(fulls == 0);
        // up and down in the body's frame: the blade turning about the body's x axis
        auto pitched = [](float angle) {  // the arm (0.45 m) carries the hand round the shoulder
            const Vec3 dir = normalize(Vec3{0, std::sin(angle), -std::cos(angle)});
            const Vec3 root = Vec3{0.2f, 1.4f, -0.1f} + dir * 0.45f;
            return Blade{root, root + dir * 0.85f};
        };
        SwingDetector h(c), u(c);
        float p = 1.2f, q = -1.2f;
        int down = 0, up = 0;
        for (int i = 0; i < 30; ++i) {
            p -= 8.0f * dt;
            q += 8.0f * dt;
            const SwingEvent sd = h.update(pitched(p), dt, 0);
            if (sd.full) { ++down; CHECK(sd.fullKind == SwingKind::Vertical); }
            up += u.update(pitched(q), dt, 0).full;
        }
        CHECK(down == 1 && up == 0);
        // the arm carries the blade across without turning it (0.6 m to the left): full, to the left; straight forward
        // along the blade (a thrust): full, a thrust; straight back (a wind-up), or forward with the blade upright (a
        // reach): never
        auto carried = [](Vec3 offset) {
            const Vec3 root = Vec3{0.2f, 1.0f, -0.3f} + offset;
            return Blade{root, root + Vec3{0, 0.3f, -0.8f}};
        };
        SwingDetector arm(c), thrust(c), back(c), reach(c);
        int armFull = 0, armSide = 0, thrustFull = 0, backFull = 0, reachFull = 0;
        for (int i = 0; i <= 20; ++i) {
            const float k = float(i) / 20.0f;
            const SwingEvent s = arm.update(carried({-0.6f * k, 0, 0}), dt, 0);
            if (s.full) { ++armFull; armSide = s.fullSide; CHECK(s.fullKind == SwingKind::Horizontal); }
            const SwingEvent t = thrust.update(carried({0, 0, -0.6f * k}), dt, 0);
            if (t.full) { ++thrustFull; CHECK(t.fullKind == SwingKind::Thrust); }
            backFull += back.update(carried({0, 0.1f * k, 0.6f * k}), dt, 0).full;
            const Vec3 root = Vec3{0.2f, 1.0f, -0.3f} + Vec3{0, 0, -0.6f * k};
            reachFull += reach.update(Blade{root, root + Vec3{0, 0.85f, -0.1f}}, dt, 0).full;
        }
        CHECK(armFull == 1 && armSide == -1 && thrustFull == 1 && backFull == 0 && reachFull == 0);
        // locked on, an overhead chop (2026-11-06, "vertical slice still misses": 43 of 55 were taken for thrusts): the
        // hand first carried 0.15 m forward with the blade raised, then 0.45 m down as the blade comes down. The
        // vertical slash, never a thrust; a level stab locked on is still a thrust
        {
            SwingDetector chop(c), stab(c);
            chop.set_locked_on(true);
            stab.set_locked_on(true);
            int chopFull = 0, chopVertical = 0, stabFull = 0, stabThrust = 0;
            for (int i = 0; i <= 20; ++i) {
                const float k = float(i) / 20.0f;
                const float f = std::min(k * 3.0f, 1.0f), d = std::max(0.0f, (k - 1.0f / 3.0f) * 1.5f);
                const Vec3 root = Vec3{0.2f, 1.6f, -0.2f} + Vec3{0, -0.45f * d, -0.15f * f - 0.05f * d};
                const float a = 1.9f - 2.7f * k;  // the blade from over the head (109 degrees) to 46 degrees down
                const SwingEvent s = chop.update(Blade{root, root + Vec3{0, 0.9f * std::sin(a), -0.9f * std::cos(a)}}, dt, 0);
                if (s.full) { ++chopFull; chopVertical += s.fullKind == SwingKind::Vertical; }
                const SwingEvent t = stab.update(carried({0, 0, -0.6f * k}), dt, 0);
                if (t.full) { ++stabFull; stabThrust += t.fullKind == SwingKind::Thrust; }
            }
            CHECK(chopFull == 1 && chopVertical == 1 && stabFull == 1 && stabThrust == 1);
        }
        // locked on, a PSVR2 player's chop (2026-11-06: 18 of 22 taken for thrusts): a flat push 0.3 m forward while the
        // blade comes down from 80 to 30 degrees up, then 0.6 m down and 0.4 m back as it turns on to 80 degrees down.
        // The vertical slash. A thrust with the blade held at 25 degrees up is still a thrust, and so is one not locked on
        {
            SwingDetector chop(c), stab(c), loose(c);
            chop.set_locked_on(true);
            stab.set_locked_on(true);
            int chopFull = 0, chopVertical = 0, stabFull = 0, stabThrust = 0, looseThrust = 0;
            const float deg = kPi / 180.0f;
            for (int i = 0; i <= 24; ++i) {
                const float k = float(i) / 24.0f;
                const float push = std::min(k / 0.4f, 1.0f), drop = std::max(0.0f, (k - 0.4f) / 0.6f);
                const Vec3 root = Vec3{0.2f, 1.5f, -0.2f} + Vec3{0, -0.6f * drop, -0.3f * push + 0.4f * drop};
                const float a = (k < 0.4f ? 80.0f - 50.0f * push : 30.0f - 110.0f * drop) * deg;
                const SwingEvent s = chop.update(Blade{root, root + Vec3{0, 0.9f * std::sin(a), -0.9f * std::cos(a)}}, dt, 0);
                if (s.full) { ++chopFull; chopVertical += s.fullKind == SwingKind::Vertical; }
                const Vec3 sr = Vec3{0.2f, 1.2f, -0.2f} + Vec3{0, 0, -0.5f * k};
                const Blade held{sr, sr + Vec3{0, 0.9f * std::sin(25 * deg), -0.9f * std::cos(25 * deg)}};
                const SwingEvent t = stab.update(held, dt, 0);
                if (t.full) { ++stabFull; stabThrust += t.fullKind == SwingKind::Thrust; }
                const SwingEvent u = loose.update(held, dt, 0);
                if (u.full) looseThrust += u.fullKind == SwingKind::Thrust;
            }
            CHECK(chopFull == 1 && chopVertical == 1 && stabFull == 1 && stabThrust == 1 && looseThrust == 1);
        }
        // (2026-11-06, "make it easier to do the slice") a chop further forward than down (0.6 m and 0.5, the blade from 60
        // degrees up to 60 down), locked on and not: the vertical slash, where it was no attack. A slow chop straight
        // down (1.3 m/s, 0.4 m) not locked on: the vertical slash too. A stab aimed 30 degrees down with the blade along
        // it: a thrust... never the slash; and the hand only lowered at 0.6 m/s: nothing
        {
            const float deg = kPi / 180.0f;
            SwingDetector diagLocked(c), diagLoose(c), slow(c), downStab(c), lower(c);
            diagLocked.set_locked_on(true);
            int diagLockedV = 0, diagLooseV = 0, slowV = 0, slowFull = 0, downStabSlash = 0, lowerFull = 0;
            for (int i = 0; i <= 24; ++i) {
                const float k = float(i) / 24.0f;
                const Vec3 root = Vec3{0.2f, 1.5f, -0.2f} + Vec3{0, -0.5f * k, -0.6f * k};  // (his: 0.59 down, 0.64 forward)
                const float a = (60.0f - 120.0f * k) * deg;
                const Blade bl{root, root + Vec3{0, 0.9f * std::sin(a), -0.9f * std::cos(a)}};
                const SwingEvent s = diagLocked.update(bl, dt, 0);
                if (s.full) diagLockedV += s.fullKind == SwingKind::Vertical;
                const SwingEvent u = diagLoose.update(bl, dt, 0);
                if (u.full) diagLooseV += u.fullKind == SwingKind::Vertical;
                const Vec3 dr = Vec3{0.2f, 1.3f, -0.2f} + Vec3{0, -0.15f * k, -0.26f * k};
                const SwingEvent t = downStab.update(Blade{dr, dr + Vec3{0, -0.9f * std::sin(30 * deg), -0.9f * std::cos(30 * deg)}}, dt, 0);
                if (t.full && t.fullKind == SwingKind::Vertical) ++downStabSlash;
            }
            // slow: 1.3 m/s down for 0.4 m (about 28 frames at 90 Hz), the blade turning from up to forward
            const int slowFrames = int(0.4f / (1.3f * dt));
            for (int i = 0; i <= slowFrames + 4; ++i) {
                const float k = std::min(float(i) / float(slowFrames), 1.0f);
                const Vec3 root = Vec3{0.2f, 1.6f, -0.3f} + Vec3{0, -0.4f * k, 0};
                const float a = (70.0f - 80.0f * k) * deg;
                const SwingEvent s = slow.update(Blade{root, root + Vec3{0, 0.9f * std::sin(a), -0.9f * std::cos(a)}}, dt, 0);
                if (s.full) { ++slowFull; slowV += s.fullKind == SwingKind::Vertical; }
            }
            const int lowerFrames = int(0.4f / (0.6f * dt));
            for (int i = 0; i <= lowerFrames; ++i) {
                const Vec3 root = Vec3{0.2f, 1.6f, -0.3f} + Vec3{0, -0.4f * float(i) / float(lowerFrames), 0};
                lowerFull += lower.update(Blade{root, root + Vec3{0, 0.3f, -0.8f}}, dt, 0).full;
            }
            CHECK(diagLockedV == 1 && diagLooseV == 1 && slowFull == 1 && slowV == 1 && downStabSlash == 0 &&
                  lowerFull == 0);
        }
        // judged on its last frame too: the swing reaches 0.3 m only on the frame it slows down
        SwingDetector last(c);
        int lastFull = 0;
        float y = 0, startY = 1;
        const float full = c.swingDownDistance;
        for (int i = 0; i < 40 && (startY > 0 || startY - (y - 0.03f) < full - 0.04f); ++i) {  // 2.7 m/s down, well short of it
            y -= 0.03f;
            const SwingEvent s = last.update(carried({0, y, 0}), dt, 0);
            if (s.started) startY = y;
            lastFull += s.full;
        }
        CHECK(lastFull == 0 && startY <= 0);
        y = startY - (full - 0.005f);  // still fast (above swingEndSpeed), just short of it
        lastFull += last.update(carried({0, y, 0}), dt, 0).full;
        CHECK(lastFull == 0);
        y = startY - (full + 0.003f);  // 0.7 m/s: slowing down (below swingEndSpeed) and past it on the same frame
        const SwingEvent slow = last.update(carried({0, y, 0}), dt, 0);
        CHECK(slow.ended && slow.full && slow.fullKind == SwingKind::Vertical);
    }
    // a one-frame tracking glitch (the tip 0.5 m away in 1/90 s: 45 m/s) is not a swing
    {
        SwingDetector g(c);
        g.update(arc_blade(0), dt, 0);
        Blade far = arc_blade(0);
        far.tip = far.tip + Vec3{0.5f, 0, 0};
        CHECK(!g.update(far, dt, 0).started && g.speed() == 0);
        CHECK(!g.update(arc_blade(0), dt, 0).started && g.phase() == SwingPhase::Idle);
    }
    // a hitch (long frame) or a teleported blade is not a swing
    d.reset();
    d.update(arc_blade(0), dt, 0);
    SwingEvent e = d.update(arc_blade(2.0f), 0.5f, 0);
    CHECK(!e.started && d.speed() == 0);
    // thrust and vertical classification
    int s2 = 0;
    CHECK(classify({0, 0, -3}, {0, 0, -1}, 0, &s2) == SwingKind::Thrust);
    CHECK(classify({0, -3, -0.5f}, {0, 0.2f, -1}, 0, &s2) == SwingKind::Vertical);
    CHECK(classify({0, 3, 0}, {0, 0, -1}, 0, &s2) == SwingKind::Rising);
    CHECK(classify({-3, 0, 0}, {0, 0, -1}, 0, &s2) == SwingKind::Horizontal && s2 == -1);
    // the body's yaw is taken into account: "right" of a body turned 90 degrees left is -Z
    CHECK(classify({0, 0, -3}, {0, 1, 0}, kPi / 2, &s2) == SwingKind::Horizontal && s2 == 1);
    // non-finite input is ignored
    e = d.update({{NAN, 0, 0}, {0, 0, 0}}, dt, 0);
    CHECK(!e.started);
}

static void test_blade_and_sweep() {
    Config c;
    c.gripPitchDegrees = 0;
    c.gripOffsetX = c.gripOffsetY = c.gripOffsetZ = 0;
    c.bladeLength = 1.0f;
    const Blade b = blade_from_grip(Pose{{}, {0, 1, 0}}, c);
    CHECK(near(b.root, {0, 1, 0}) && near(b.tip, {0, 1, -1}));
    c.gripPitchDegrees = 90;  // tilted up
    const Blade up = blade_from_grip(Pose{{}, {0, 1, 0}}, c);
    CHECK(near(up.tip, {0, 2, 0}, 1e-4f));
    // fast swing: 60 degrees between two frames. A target in the middle of the swept arc is between the
    // two blades and touches none of them, but the sweep's capsules cover it.
    const Blade before = arc_blade(-0.5f), now = arc_blade(0.5f);
    const Vec3 target = arc_blade(0.0f).tip * 0.7f + arc_blade(0.0f).root * 0.3f;
    const float r = 0.05f;
    CHECK(distance_to_segment(target, {before.root, before.tip}) > r);
    CHECK(distance_to_segment(target, {now.root, now.tip}) > r);
    const Sweep s = sweep(before, now);
    CHECK(sweep_touches(s, r, target, 0.12f));
    CHECK(near(s.atVec, now.tip - before.tip));
    // far away: no contact
    CHECK(!sweep_touches(s, r, {0, 5, 0}, 0.1f));
    // hit ledger: each target once per swing
    HitLedger h;
    h.begin_swing();
    CHECK(h.record(0x1234) && !h.record(0x1234) && h.record(0x5678) && h.count() == 2);
    CHECK(!h.record(0));
    h.begin_swing();
    CHECK(h.count() == 0 && h.record(0x1234) && h.swing() == 2);

    // the physical sword: armed while swinging and kPhysicalHold after, its hits reset on each new swing; never with
    // the sword not held for an attack or while the game's own attack runs
    {
        const float st = 1.0f / 30.0f;
        PhysicalSword p;
        CHECK(!p.update(true, false, false, 1, st).armed);
        PhysicalStep s = p.update(true, false, true, 1, st);
        CHECK(s.armed && s.began);
        s = p.update(true, false, true, 1, st);
        CHECK(s.armed && !s.began);
        CHECK(p.update(true, false, false, 1, st).armed);   // 0.1 s held after the swing
        CHECK(p.update(true, false, false, 1, st).armed);
        int held = 2;
        while (p.update(true, false, false, 1, st).armed) ++held;
        CHECK(held >= 2 && held <= 3);  // steps of 1/30 s: about 0.1 s
        s = p.update(true, false, true, 2, st);  // a new swing
        CHECK(s.armed && s.began);
        s = p.update(true, false, true, 3, st);  // a new swing while armed (a flurry): its hits afresh
        CHECK(s.armed && s.began);
        CHECK(!p.update(true, true, true, 3, st).armed);   // the game's attack
        CHECK(p.update(true, false, true, 3, st).began);    // after it: armed again
        CHECK(!p.update(false, false, true, 3, st).armed);  // sheathed / third person
        CHECK(!p.update(true, false, false, 3, st).armed);  // no hold carried over from before
    }
}

// Fix 4: the gameplay camera's transform is the one the first play test found correct; these values pin it
static void test_gameplay_camera_regression() {
    WorldMapping m;
    m.origin = {0.1f, 1.6f, -0.2f};
    m.heading = 0.5f;
    const Vec3 anchor{1000, 50, -2000};
    // the head at the origin, looking straight ahead: Link's eyes, facing heading 0.5
    Pose h = gameplay_head(m, anchor, Pose{{}, m.origin}, 0, 0.45f, 100);
    CHECK(near(h.position, anchor));
    CHECK(near(rotate(h.orientation, {0, 0, -1}), heading_dir(0.5f)));
    // leaning 20 cm right and 10 cm down: 20 and 10 game units, turned with the heading
    h = gameplay_head(m, anchor, Pose{{}, m.origin + Vec3{0.2f, -0.1f, 0}}, 0, 0.45f, 100);
    CHECK(near(h.position, anchor + heading_right(0.5f) * 20.0f + Vec3{0, -10, 0}, 1e-2f));
    // walking 1 m forward is held at 45 cm; a height offset adds to the anchor
    h = gameplay_head(m, anchor, Pose{{}, m.origin + Vec3{0, 0, -1}}, 0.1f, 0.45f, 100);
    CHECK(near(h.position, anchor + Vec3{0, 10, 0} + heading_dir(0.5f) * 45.0f, 1e-2f));
    // head turned 30 degrees left and pitched down 20 degrees
    const Quat q = yaw_quat(30 * kPi / 180) * axis_angle({1, 0, 0}, -20 * kPi / 180);
    h = gameplay_head(m, anchor, Pose{q, m.origin}, 0, 0.45f, 100);
    const Vec3 f = rotate(h.orientation, {0, 0, -1});
    CHECK(near(game_heading(f), 0.5f + 30 * kPi / 180, 1e-3f) && f.y < -0.3f);
}

// The first-person camera pipeline end to end (docs/VR_CAMERA_AUDIT.md): the headset pose, the tracking origin, Link's
// eye point and the turns combine once, in the order CameraOverride uses, and both eyes come from that one result
static void test_camera_audit() {
    constexpr float s = 100;  // units per metre
    const Vec3 anchor{1200, 80, -400};
    // (6DoF) translation on every axis and rotation on every axis go through unchanged and exactly once
    {
        WorldMapping m;
        m.origin = {0.3f, 1.5f, -0.1f};
        m.heading = 1.1f;
        const Vec3 lean{0.12f, -0.07f, -0.2f};  // right, down (crouch), forward
        const Quat q = yaw_quat(0.4f) * axis_angle({1, 0, 0}, -0.3f) * axis_angle({0, 0, 1}, 0.2f);  // yaw, pitch, roll
        const Pose h = gameplay_head(m, anchor, Pose{q, m.origin + lean}, 0, 1.0f, s);
        CHECK(near(h.position, anchor + to_game_dir(m, lean) * s, 1e-3f));
        CHECK(near(length(h.position - anchor), length(lean) * s, 1e-3f));  // no zoom, no scaling but the world's
        CHECK(angle_between(h.orientation, normalize(yaw_quat(1.1f + kPi) * q)) < 1e-5f);
        // the game's camera takes eye, centre and up (look_at); the orientation it builds again keeps the roll
        const LookAt la = look_at(h, 100);
        CHECK(angle_between(look_rotation(la.center - la.eye, la.up), h.orientation) < 1e-3f);
        // the lean cap (1 m) holds the horizontal lean only; crouching is never held
        const Pose far = gameplay_head(m, anchor, Pose{q, m.origin + Vec3{2.0f, -0.5f, 0}}, 0, 1.0f, s);
        const Vec3 d = far.position - anchor;
        CHECK(near(std::sqrt(d.x * d.x + d.z * d.z), 100.0f, 1e-2f) && near(d.y, -50.0f, 1e-3f));
    }
    // (turning + head movement) the stick turns the mapping's heading: the view's position turns about the tracking
    // origin's game point, Link's eyes; its distance from them (the lean) is kept, the head's own yaw adds on top
    {
        WorldMapping m;
        m.origin = {0, 1.6f, 0};
        const Vec3 lean{0.2f, 0, 0};
        const Pose before = gameplay_head(m, anchor, Pose{{}, m.origin + lean}, 0, 1.0f, s);
        m.heading += 0.5f;
        const Pose after = gameplay_head(m, anchor, Pose{{}, m.origin + lean}, 0, 1.0f, s);
        CHECK(near(length(after.position - anchor), length(before.position - anchor), 1e-3f));
        CHECK(near(game_heading(rotate(after.orientation, {0, 0, -1})),
                   wrap_radians(game_heading(rotate(before.orientation, {0, 0, -1})) + 0.5f), 1e-4f));
        // Link's drawn body turned between steps about the same point (turn_pivot): his eye point stays at the view's
        // base; about his feet it left it by eye distance times the turn
        const Vec3 feet{1180, 0, -410}, eyeOffset = Vec3{anchor.x - feet.x, 0, anchor.z - feet.z};
        const Quat r = yaw_quat(0.07f);  // one step's share of a smooth turn
        const Vec3 pivot = turn_pivot(feet, eyeOffset, true);
        const Vec3 eyes{anchor.x, 0, anchor.z};
        CHECK(near(pivot + rotate(r, eyes - pivot), eyes, 1e-3f));
        CHECK(length(feet + rotate(r, eyes - feet) - eyes) > 0.06f * length(eyeOffset));
        CHECK(near(turn_pivot(feet, eyeOffset, false), feet));  // (no eye point measured yet: his feet, as before)
    }
    // (room scale) a step's walk in the play space moves Link and the origin by the same amount: the view stays at
    // the head, the walk is applied exactly once
    {
        WorldMapping m;
        m.origin = {0, 1.6f, 0};
        m.heading = -0.8f;
        const Vec3 head = m.origin + Vec3{0.05f, 0.02f, -0.03f};
        const Pose before = gameplay_head(m, anchor, Pose{{}, head}, 0, 1.0f, s);
        Vec3 walk = head - m.origin;
        walk.y = 0;
        WorldMapping moved = m;
        moved.origin = m.origin + walk;
        const Pose after = gameplay_head(moved, anchor + to_game_dir(m, walk) * s, Pose{{}, head}, 0, 1.0f, s);
        CHECK(near(after.position, before.position, 1e-3f));
        // half a walk taken (a wall): the origin takes the same half, the rest stays a lean
        moved.origin = m.origin + walk * 0.5f;
        const Pose half = gameplay_head(moved, anchor + to_game_dir(m, walk * 0.5f) * s, Pose{{}, head}, 0, 1.0f, s);
        CHECK(near(half.position, before.position, 1e-3f));
    }
    // (left stick + head) "forward" on the stick is the head's heading whatever the game camera's
    {
        const float head = 0.9f, camera = -2.0f;
        const Stick st = remap_stick({0, 1}, head, camera);
        const Vec3 world = heading_right(camera) * st.x + heading_dir(camera) * st.y;
        CHECK(near(world, heading_dir(head), 1e-4f));
    }
    // (recentre) the head at the recentre stands at Link's eyes facing his heading, from any room position and yaw
    {
        const Pose head{yaw_quat(2.3f) * axis_angle({1, 0, 0}, 0.2f), {1.4f, 1.7f, -2.2f}};
        const float linkHeading = -1.2f;
        WorldMapping m;
        m.origin = head.position;
        m.heading = linkHeading - yaw_of(head.orientation);
        const Pose h = gameplay_head(m, anchor, head, 0, 1.0f, s);
        CHECK(near(h.position, anchor, 1e-3f));
        CHECK(near(game_heading(rotate(h.orientation, {0, 0, -1})), linkHeading, 1e-4f));
    }
    // (stereo) both eyes from one head pose: its orientation, the eyes' offsets along its right, apart by the IPD
    {
        const Pose head{yaw_quat(0.6f) * axis_angle({0, 0, 1}, 0.3f), {0.1f, 1.6f, -0.4f}};
        const Pose l = eye_pose(head, -0.032f), r = eye_pose(head, 0.032f);
        CHECK(angle_between(l.orientation, head.orientation) < 1e-6f && angle_between(r.orientation, head.orientation) < 1e-6f);
        CHECK(near(length(r.position - l.position), 0.064f, 1e-5f));
        CHECK(near((l.position + r.position) * 0.5f, head.position, 1e-5f));
        CHECK(near(r.position - l.position, rotate(head.orientation, {0.064f, 0, 0}), 1e-5f));
        // an eye's view matrix is the head camera's shifted by the eye's offset in view space (stereo project_eye):
        // a world point seen from the eye pose is the head camera's view of it less that offset
        WorldMapping m;
        m.origin = {0, 1.6f, 0};
        m.heading = 0.3f;
        m.anchor = anchor;
        m.unitsPerMetre = s;
        const Pose gh = to_game_pose(m, head), ge = to_game_pose(m, r);
        const LookAt la = look_at(gh, 100);
        const Quat cam = look_rotation(la.center - la.eye, la.up);
        const Vec3 p{1250, 120, -500};
        const Vec3 fromHead = rotate(conjugate(cam), p - la.eye) - Vec3{0.032f * s, 0, 0};
        const Vec3 fromEye = rotate(conjugate(ge.orientation), p - ge.position);
        CHECK(near(fromHead, fromEye, 2e-2f));
    }
    // (lean back) held at the limit behind his eyes along his drawn facing; forward and sideways untouched
    {
        const Vec3 eyes{0, 100, 0};
        Vec3 v = eyes + heading_dir(0.4f) * -20.0f;
        CHECK(hold_lean_back(v, eyes, 0.4f, 8.0f) && near(v, eyes + heading_dir(0.4f) * -8.0f, 1e-4f));
        Vec3 fwd = eyes + heading_dir(0.4f) * 30.0f + heading_right(0.4f) * 25.0f;
        const Vec3 kept = fwd;
        CHECK(!hold_lean_back(fwd, eyes, 0.4f, 8.0f) && near(fwd, kept));
        // the facing between two steps' (blend_heading): the short way, across the wrap
        CHECK(near(blend_heading(3.0f, -3.0f, 0.5f), wrap_radians(3.0f + (2 * kPi - 6.0f) * 0.5f), 1e-5f));
        CHECK(near(blend_heading(0.2f, 0.6f, 0.25f), 0.3f, 1e-6f) && near(blend_heading(0.2f, 0.6f, 1.0f), 0.6f, 1e-6f));
    }
    // (ladder, ledge, lock-on) the view's base turned each frame (RigTurn) reaches where the pad-read turn reached, in
    // frames' shares: 90 frames a second turn in steps a third of the 30 Hz ones
    {
        RigTurn turn{true, false, 1.5f};
        float perFrame = 0, perStep = 0, frameMost = 0, stepMost = 0;
        for (int i = 0; i < 90; ++i) {
            const float next = turn.update(perFrame, 1.0f / 90.0f);
            frameMost = std::max(frameMost, std::fabs(next - perFrame));
            perFrame = next;
        }
        for (int i = 0; i < 30; ++i) {
            const float next = turn_towards(perStep, 1.5f, 1.0f / 30.0f);
            stepMost = std::max(stepMost, std::fabs(next - perStep));
            perStep = next;
        }
        CHECK(near(perFrame, perStep, 0.03f) && near(perFrame, 1.5f, 0.05f));
        CHECK(frameMost < stepMost * 0.4f);
        CHECK(RigTurn{}.update(0.7f, 0.1f) == 0.7f);  // (not active: the heading is left alone)
    }
}

// The first-person head's one owner (vr_head.h HeadController, docs/VR_HEAD_CONTROLLER.md): the recentre, the turns, the
// room-scale origin step by step, the head drawn in first person and with the game's camera, the views by display time
static void test_head_controller() {
    // ownership: the mapping and the room-scale state are read-only from outside (their writers are its methods)
    static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<HeadController&>().mapping())>>);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<HeadController&>().room())>>);
    constexpr float s = 100;
    const Vec3 eyes{500, 120, -300};
    HeadController h;
    CHECK(!h.calibrated() && h.recenter_requested() && !h.calibrated_for(7));
    // recentre: the head at Link's eyes facing his heading, from any room position and yaw
    const Pose start{yaw_quat(1.9f) * axis_angle({1, 0, 0}, -0.2f), {0.4f, 1.6f, -0.7f}};
    h.recenter(start, 0.6f, 7);
    CHECK(h.calibrated_for(7) && !h.calibrated_for(8) && !h.recenter_requested());
    Pose v = h.first_person(start, eyes, 0.6f, 1.0f, 0, 1.0f, s, 0);
    CHECK(near(v.position, eyes, 1e-3f) && near(game_heading(rotate(v.orientation, {0, 0, -1})), 0.6f, 1e-4f));
    // the same as the transform the headset approved (gameplay_head on the controller's mapping): nothing added
    {
        const Pose lean{start.orientation * axis_angle({0, 0, 1}, 0.15f), start.position + Vec3{0.1f, -0.05f, 0.08f}};
        WorldMapping m = h.mapping();
        const Pose ref = gameplay_head(m, eyes, lean, 0.02f, 0.7f, s);
        const Pose got = h.first_person(lean, eyes, 0.6f, 1.0f, 0.02f, 0.7f, s, 0);
        CHECK(near(got.position, ref.position, 1e-4f) && angle_between(got.orientation, ref.orientation) < 1e-6f);
    }
    // a request recentres again; a new Link too
    h.request_recenter();
    CHECK(!h.calibrated_for(7));
    h.recenter(start, 0.6f, 7);
    // turns: the heading and the total the body follows, wrapped
    h.turn(0.3f);
    h.turn(-0.1f);
    CHECK(near(h.mapping().heading, wrap_radians(0.6f - yaw_of(start.orientation) + 0.2f), 1e-5f) && near(h.turn_total(), 0.2f));
    // the rig turn advances only while set, and does not count as a stick turn
    const float headingBefore = h.mapping().heading;
    CHECK(!h.advance_rig_turn(0.1f) && h.mapping().heading == headingBefore);
    h.set_rig_turn({true, false, headingBefore + 0.5f});
    CHECK(h.advance_rig_turn(1.0f / 90.0f) && h.mapping().heading > headingBefore && near(h.turn_total(), 0.2f));
    h.stop_rig_turn();
    CHECK(!h.advance_rig_turn(0.1f));
    // the room-scale walk applied exactly once: Link moved by it, the origin by it, the view where it was
    {
        HeadController r;
        r.recenter(start, 0.0f, 1);
        const Pose head{start.orientation, start.position + Vec3{0.06f, 0, -0.04f}};
        const Pose before = r.first_person(head, eyes, 0, 1.0f, 0, 1.0f, s, 0);
        Vec3 walk = head.position - r.origin_now();
        walk.y = 0;
        const Vec3 game = to_game_dir(r.mapping(), walk) * s;
        r.begin_move();
        r.walk(walk, game, eyes + game, true);
        const Vec3 push = r.step(eyes + game);  // (he took it all: his position is where the walk put him)
        CHECK(near(push, game, 1e-4f));
        const Pose after = r.first_person(head, eyes + game, 0, 1.0f, 0, 1.0f, s, 0);
        CHECK(near(after.position, before.position, 1e-3f));
        // between the steps both blend with the same fraction: half way the view is still at the head
        HeadController r2;
        r2.recenter(start, 0.0f, 1);
        r2.walk(walk, game, eyes + game, true);
        r2.step(eyes + game);
        const Pose half = r2.first_person(head, eyes + game * 0.5f, 0, 0.5f, 0, 1.0f, s, 0);
        CHECK(near(half.position, before.position, 1e-3f));
        // a wall stopped him half way: the origin takes half, the rest stays a lean towards the wall
        HeadController w;
        w.recenter(start, 0.0f, 1);
        w.walk(walk, game, eyes + game, true);
        w.step(eyes + game * 0.5f);
        CHECK(near(w.origin(1.0f), w.room().base + walk * 0.5f, 1e-4f));
        const Pose leaning = w.first_person(head, eyes + game * 0.5f, 0, 1.0f, 0, 1.0f, s, 0);
        CHECK(near(leaning.position, before.position, 1e-3f));
        const HeadStats st = w.take_stats();
        CHECK(st.walks == 1 && st.shortWalks == 1 && near(st.taken, st.wanted * 0.5f, 1e-2f));
        CHECK(w.take_stats().walks == 0);
        // the same step again (true 60's preview) adds what was pending since; a new Link takes it unblended
        w.walk(walk, game, eyes + game, false);
        CHECK(near(w.step_again(eyes + game), game, 1e-4f));
        w.walk(walk, game, eyes, false);
        w.step_new_link();
        CHECK(near(w.origin(0.0f), w.origin(1.0f)) && near(w.origin_now(), w.origin(1.0f)));
        // turned about his eyes: reported once to his step
        w.begin_move();
        w.turned_about_eyes({3, 0, 1}, 0.4f);
        float facing = 0;
        CHECK(w.take_turned_about_eyes(facing) && near(facing, 0.4f) && !w.take_turned_about_eyes(facing));
        CHECK(w.set_catching_up(true) && !w.set_catching_up(true) && w.set_catching_up(false));
    }
    // walls: the lean held short of the wall in front; begin_move forgets the last step's
    {
        HeadController wl;
        wl.recenter(start, 0.0f, 1);
        const Vec3 fwd = rotate(start.orientation, {0, 0, -1});
        const Pose far{start.orientation, start.position + Vec3{fwd.x, 0, fwd.z} * 0.5f};
        wl.set_wall_lean(0.1f);
        const Pose held = wl.first_person(far, eyes, 0, 1.0f, 0, 1.0f, s, 0);
        CHECK(near(length(Vec3{held.position.x - eyes.x, 0, held.position.z - eyes.z}), 10.0f, 1e-2f));
        wl.begin_move();
        CHECK(wl.wall_lean() > 1e8f);
    }
    // leaning back: held at the limit and counted
    {
        HeadController lb;
        lb.recenter(start, 0.0f, 1);
        const Vec3 fwd = rotate(start.orientation, {0, 0, -1});
        const Pose back{start.orientation, start.position - Vec3{fwd.x, 0, fwd.z} * 0.3f};
        const Pose held = lb.first_person(back, eyes, 0.0f, 1.0f, 0, 1.0f, s, 8.0f);
        const Vec3 d = held.position - eyes;
        CHECK(near(d.x * heading_dir(0).x + d.z * heading_dir(0).z, -8.0f, 1e-2f) && lb.take_stats().backHeld == 1);
    }
    // the step's head for the arms: the origin with this step's walk, nothing changed
    {
        HeadController sh;
        sh.recenter(start, 0.0f, 1);
        sh.walk({0.05f, 0, 0}, {5, 0, 0}, eyes, true);
        const Pose a = sh.step_head(start, eyes, 0, 1.0f, s);
        WorldMapping m = sh.mapping();
        m.origin = sh.origin_now();
        CHECK(near(a.position, gameplay_head(m, eyes, start, 0, 1.0f, s).position, 1e-4f));
        CHECK(near(sh.mapping().origin, start.position));  // (the mapping itself untouched)
    }
    // the game's camera turned by the head since it began, its position the game's
    {
        HeadController g;
        CHECK(!g.game_camera() && !g.end_game_camera());
        g.begin_game_camera(0.5f);
        const Vec3 eye{0, 100, 0}, center{100, 100, 0}, up{0, 1, 0};
        const Pose same = g.game_camera_view(eye, center, up, yaw_quat(0.5f));
        CHECK(angle_between(same.orientation, look_rotation(center - eye, up)) < 1e-4f && near(same.position, eye));
        const Pose turned = g.game_camera_view(eye, center, up, yaw_quat(0.8f));
        CHECK(near(game_heading(rotate(turned.orientation, {0, 0, -1})), game_heading(center - eye) + 0.3f, 1e-4f));
        CHECK(g.end_game_camera() && !g.game_camera());
    }
    // the views by display time (the screen blur): a second pass of a frame changes nothing
    {
        HeadController f;
        const Fov fov{-1.0f, 1.0f, 0.9f, -0.9f};
        float a[6];
        f.begin_frame(100);
        f.drawn(Quat{});
        CHECK(!f.blur_reprojection(fov, a));  // (no frame before)
        f.begin_frame(111);
        f.drawn(yaw_quat(0.01f));
        CHECK(f.blur_reprojection(fov, a));
        f.begin_frame(111);  // (the same frame again)
        CHECK(f.blur_reprojection(fov, a));
        f.begin_frame(122);  // (a frame not drawn from the head: none of its own yet)
        CHECK(!f.blur_reprojection(fov, a));
    }
}

// The screen blur in the headset (docs/VR_SCREEN_EFFECTS.md): the last frame's picture moved by the view's turn
static void test_screen_blur() {
    const Fov f{-1.0f, 1.0f, 0.9f, -0.9f};
    const float tx = std::tan(1.0f), ty = std::tan(0.9f);
    const Quat base = yaw_quat(0.7f) * axis_angle({1, 0, 0}, -0.2f);
    float a[6];
    // no turn: the identity
    CHECK(blur_reprojection(base, base, f, a));
    CHECK(near(a[0], 1) && near(a[1], 0) && near(a[2], 0) && near(a[3], 0) && near(a[4], 1) && near(a[5], 0));
    // the head turned left by th: what is now in the middle was left of the middle in the last frame
    const float th = 0.02f;
    CHECK(blur_reprojection(base, base * yaw_quat(th), f, a));
    CHECK(near(a[0] * 0.5f + a[1] * 0.5f + a[2], 0.5f - std::tan(th) / (2 * tx), 1e-4f));
    CHECK(near(a[3] * 0.5f + a[4] * 0.5f + a[5], 0.5f, 1e-4f));
    // looked up by th: it was above the middle (v smaller: v runs down)
    CHECK(blur_reprojection(base, base * axis_angle({1, 0, 0}, th), f, a));
    CHECK(near(a[3] * 0.5f + a[4] * 0.5f + a[5], 0.5f - std::tan(th) / (2 * ty), 1e-4f));
    CHECK(near(a[0] * 0.5f + a[1] * 0.5f + a[2], 0.5f, 1e-4f));
    // a roll turns the picture about its middle: the middle stays
    CHECK(blur_reprojection(base, base * axis_angle({0, 0, 1}, th), f, a));
    CHECK(near(a[0] * 0.5f + a[1] * 0.5f + a[2], 0.5f, 1e-4f) && near(a[3] * 0.5f + a[4] * 0.5f + a[5], 0.5f, 1e-4f));
    CHECK(std::fabs(a[1]) > 1e-3f && std::fabs(a[3]) > 1e-3f);
    // a cut (turned 40 degrees between two frames): not moved
    CHECK(!blur_reprojection(base, base * yaw_quat(0.7f), f, a));
    // the game's zoom after the move: m * T on (u, v, z, 1) is m applied to T(u, v)
    CHECK(blur_reprojection(base, base * yaw_quat(th) * axis_angle({1, 0, 0}, -th), f, a));
    float m[12] = {0.99f, 0, 0, 0.005f, 0, 0.99f, 0, 0.005f, 0, 0, 1, 0};  // 0.99 about the middle
    float game[12];
    for (int i = 0; i < 12; ++i) game[i] = m[i];
    blur_compose(m, a);
    for (const float u : {0.1f, 0.5f, 0.8f})
        for (const float v : {0.2f, 0.5f, 0.9f}) {
            const float tu = a[0] * u + a[1] * v + a[2], tv = a[3] * u + a[4] * v + a[5];
            CHECK(near(m[0] * u + m[1] * v + m[2] * 1 + m[3], game[0] * tu + game[1] * tv + game[2] + game[3], 1e-5f));
            CHECK(near(m[4] * u + m[5] * v + m[6] * 1 + m[7], game[4] * tu + game[5] * tv + game[6] + game[7], 1e-5f));
        }
    // an affine map is exact at the middle only (a wide view's projection is not affine): everywhere it is nearer
    // where the picture was than not moving it at all, by far near the middle (a 1.1-degree turn and a 0.6 pitch)
    {
        const Quat rel = yaw_quat(0.02f) * axis_angle({1, 0, 0}, 0.01f);
        CHECK(blur_reprojection(Quat{}, rel, f, a));
        for (const float r : {0.1f, 0.25f, 0.5f}) {
            float fitted = 0, still = 0;
            for (int i = 0; i <= 10; ++i)
                for (int j = 0; j <= 10; ++j) {
                    const float u = 0.5f - r + 0.2f * r * float(i), v = 0.5f - r + 0.2f * r * float(j);
                    const Vec3 d = rotate(rel, Vec3{(2 * u - 1) * tx, (1 - 2 * v) * ty, -1});
                    const float pu = (d.x / -d.z / tx + 1) * 0.5f, pv = (1 - d.y / -d.z / ty) * 0.5f;
                    fitted = std::max(fitted, std::hypot(a[0] * u + a[1] * v + a[2] - pu, a[3] * u + a[4] * v + a[5] - pv));
                    still = std::max(still, std::hypot(pu - u, pv - v));
                }
            CHECK(fitted < still);
            if (r < 0.2f) CHECK(fitted < still * 0.25f);
        }
    }
    // the settings: off by default in the headset (Disable screen blur, Disable bloom checked), saved and read back
    const Config d;
    CHECK(!d.screenBlur && !d.bloom);
    Config c;
    CHECK(from_kv(c, "vr.screenBlur", "1") && from_kv(c, "vr.bloom", "1") && c.screenBlur && c.bloom);
    bool blurSaved = false, bloomSaved = false;
    for (const auto& kv : to_kv(c)) {
        blurSaved |= kv.first == "vr.screenBlur" && kv.second == "1";
        bloomSaved |= kv.first == "vr.bloom" && kv.second == "1";
    }
    CHECK(blurSaved && bloomSaved);
    // Hide Link's body and arms: on by default (2026-11-04), saved and read back
    CHECK(d.hideBody && d.hideArms);  // (on by default since 2026-11-04)
    CHECK(from_kv(c, "vr.hideBody", "1") && c.hideBody);
    bool hideSaved = false;
    for (const auto& kv : to_kv(c)) hideSaved |= kv.first == "vr.hideBody" && kv.second == "1";
    CHECK(hideSaved);
}

static void test_menu_and_panel() {
    // menus: 50 % farther along the same line; the target stays
    const Vec3 eye{0, 100, 300}, center{0, 80, 0};
    const Vec3 z = menu_eye(eye, center, 1.5f);
    CHECK(near(length(z - center), 1.5f * length(eye - center), 1e-2f));
    CHECK(near(normalize(z - center), normalize(eye - center)));
    CHECK(near(menu_eye(eye, center, 1.0f), eye));
    CHECK(camera_kind(true, "sea_T") == CameraKind::Gameplay);  // gameplay wins (never zoomed)
    CHECK(camera_kind(false, "sea_T") == CameraKind::Menu && camera_kind(false, "Name") == CameraKind::Menu);
    CHECK(camera_kind(false, "sea") == CameraKind::Event && camera_kind(false, "M_NewD2") == CameraKind::Event);
    // the panel: ahead of the head's horizontal direction, level, facing the head
    const Pose head{yaw_quat(1.0f) * axis_angle({1, 0, 0}, -0.6f), {0.5f, 1.6f, 0}};
    const Pose p = panel_pose(head, 1.2f, 0.1f);
    const Vec3 toPanel = p.position - head.position;
    CHECK(near(std::sqrt(toPanel.x * toPanel.x + toPanel.z * toPanel.z), 1.2f) && near(toPanel.y, -0.1f));
    CHECK(near(yaw_of(Quat{0, 0, 0, 1} * yaw_quat(1.0f)), 1.0f));
    CHECK(near(normalize(rotate(p.orientation, {0, 0, 1})), normalize(Vec3{-toPanel.x, 0, -toPanel.z})));  // faces the head
}

static void test_stick_and_angle() {
    // dead zone: nothing inside, continuous at the edge, at most length 1
    CHECK(dead_zone({0.1f, 0.05f}, 0.15f).x == 0 && dead_zone({0.1f, 0.05f}, 0.15f).y == 0);
    Stick s = dead_zone({0.16f, 0}, 0.15f);
    CHECK(s.x > 0 && s.x < 0.02f);
    s = dead_zone({1, 1}, 0.15f);
    CHECK(near(std::sqrt(s.x * s.x + s.y * s.y), 1.0f));
    s = dead_zone({0, -1}, 0.15f);
    CHECK(near(s.y, -1.0f) && s.x == 0);
    CHECK(dead_zone({NAN, 0}, 0.1f).x == 0);
    // the game's controlled angle: heading plus the measured offset, wrapping
    CHECK(controlled_angle(0, 0) == 0);
    CHECK(controlled_angle(kPi / 2, 0) == 0x4000);
    CHECK(controlled_angle(kPi / 2, int16_t(0x8000)) == int16_t(0xC000));
    CHECK(controlled_angle(kPi, int16_t(0x8000)) == 0);
    CHECK(snap_angle_offset(int16_t(0x8000 + 0x0300)) == int16_t(0x8000));
    CHECK(snap_angle_offset(0x0123) == 0);
    CHECK(snap_angle_offset(0x2000) == 0x2000);  // not near a quarter turn: kept
}

static void test_arm_pose() {
    // an arm hanging down from (0, 100, 0): upper arm, forearm, hand, joint axes = world axes
    ArmJoints a;
    a.upper.pos = {0, 100, 0};
    a.lower.pos = {0, 82, 1};
    a.hand.pos = {0, 65, 0};
    a.upper.rot = a.lower.rot = a.hand.rot = Quat{};
    CHECK(plausible_arm(a, 5, 60) && !plausible_arm(a, 20, 60));
    const float lu = length(a.lower.pos - a.upper.pos), ll = length(a.hand.pos - a.lower.pos);
    // reaching forward (-Z) and up: wrist on target, lengths kept, the shoulder stays
    const Vec3 target{5, 95, -25};
    ArmJoints p = pose_arm(a, target, {0, 0, -1}, 1.0f);
    CHECK(near(p.hand.pos, target, 1e-3f));
    CHECK(near(p.upper.pos, a.upper.pos));
    CHECK(near(length(p.lower.pos - p.upper.pos), lu, 1e-3f) && near(length(p.hand.pos - p.lower.pos), ll, 1e-3f));
    // the upper arm's frame turned with its bone: its old bone direction now points at the new elbow
    CHECK(near(rotate(p.upper.rot, rotate(conjugate(a.upper.rot), normalize(a.lower.pos - a.upper.pos))),
               normalize(p.lower.pos - p.upper.pos), 1e-3f));
    // the hand points where the controller points
    const Vec3 local = rotate(conjugate(a.hand.rot), normalize(a.hand.pos - a.lower.pos));
    CHECK(near(rotate(p.hand.rot, local), {0, 0, -1}, 1e-3f));
    // out of reach: no stretching
    p = pose_arm(a, {0, 100, -500}, {}, 1.0f);
    CHECK(length(p.hand.pos - p.upper.pos) <= (lu + ll) + 1e-3f);
    CHECK(near(length(p.lower.pos - p.upper.pos), lu, 1e-3f) && near(length(p.hand.pos - p.lower.pos), ll, 1e-2f));
    // weight 0: the animation; 0.5: in between (no snapping when tracking starts)
    p = pose_arm(a, target, {0, 0, -1}, 0.0f);
    CHECK(near(p.hand.pos, a.hand.pos));
    p = pose_arm(a, target, {0, 0, -1}, 0.5f);
    CHECK(length(p.hand.pos - a.hand.pos) > 1 && length(p.hand.pos - target) > 1);
    // a non-finite target (tracking lost mid-frame) leaves the animation
    p = pose_arm(a, {NAN, 0, 0}, {}, 1.0f);
    CHECK(near(p.hand.pos, a.hand.pos));
    // matrices round trip, with scale
    float m[12] = {0, -1.2f, 0, 5, 1.2f, 0, 0, 6, 0, 0, 1.2f, 7};
    const Joint j = joint_from_matrix(m);
    float back[12];
    matrix_from_joint(j, back);
    for (int i = 0; i < 12; ++i) CHECK(near(back[i], m[i], 1e-4f));
    // rotation_between, including opposite directions
    CHECK(near(rotate(rotation_between({1, 0, 0}, {0, 0, 1}), {1, 0, 0}), {0, 0, 1}));
    CHECK(near(rotate(rotation_between({0, 1, 0}, {0, -1, 0}), {0, 1, 0}), {0, -1, 0}, 1e-3f));
    // the wrist target scaled to Link's arm
    CHECK(near(scaled_wrist_target({0, 100, 0}, {0, 0, -62}, 31, 62), {0, 100, -31}));
    // the camera holds still against an idle sway, and follows a change of posture
    {
        Settled<float> h;
        h.reset(110);
        for (int i = 0; i < 300; ++i) h.update(110 + 3.0f * std::sin(i * 0.2f), 6, 1 / 30.0f, 0.35f);
        CHECK(h.value == 110);
        for (int i = 0; i < 90; ++i) h.update(60, 6, 1 / 30.0f, 0.35f);
        CHECK(std::fabs(h.value - 60) < 1 && !h.moving);
        Settled<Vec3> o;
        o.reset({0, 0, 6});
        for (int i = 0; i < 300; ++i) o.update({std::sin(i * 0.3f) * 2.0f, 0, 6}, 4, 1 / 30.0f, 0.2f);
        CHECK(near(o.value, {0, 0, 6}));
    }
    // first person: the hand stays in the direction the controller is seen from the eyes (looking down at it)
    {
        const Vec3 eyes{10, 120, 5}, seen{0, -40, -30};
        const Vec3 w = eye_wrist_target(eyes, seen, 35, 62);
        CHECK(near(normalize(w - eyes), normalize(seen), 1e-4f));
        CHECK(std::fabs(length(w - eyes) - 50.0f * 35.0f / 62.0f) < 1e-3f);
        // beyond reach: pulled back along the line of sight, onto the reach sphere
        const Vec3 shoulder{10, 105, 5}, far = eyes + normalize(seen) * 80.0f;
        const Vec3 k = keep_in_view(eyes, far, shoulder, 35);
        CHECK(near(normalize(k - eyes), normalize(seen), 1e-4f));
        CHECK(std::fabs(length(k - shoulder) - 35.0f) < 1e-2f);
        CHECK(near(keep_in_view(eyes, w, shoulder, 35), w));  // reachable: unchanged
        const Vec3 level = eyes + Vec3{0, 0, -90};  // far, at eye level: the line misses his reach, the target stays
        CHECK(near(keep_in_view(eyes, level, {10, 60, 5}, 35), level));
    }
}

// fix 2: a 90 Hz headset is no longer drawn at 60 fps; the desktop rule is unchanged
static void test_frame_rate_cap() {
    // a 90 Hz headset: 90 fps, three frames a logic step, one per refresh (120 fps there showed unevenly)
    CHECK(interp::pacing::cap_fps_vr(120, 90) == 90 && interp::pacing::cap_fps_vr(240, 90) == 90);
    CHECK(interp::pacing::pass_fraction(0, 2, false) == 1.0f / 3.0f && interp::pacing::pass_fraction(1, 2, false) == 2.0f / 3.0f);
    CHECK(interp::pacing::cap_fps_vr(120, 120) == 120 && interp::pacing::cap_fps_vr(240, 144) == 120);
    CHECK(interp::pacing::cap_fps_vr(120, 72) == 60 && interp::pacing::cap_fps_vr(60, 90) == 60);
    CHECK(interp::pacing::cap_fps_vr(120, 120) == 120 && interp::pacing::cap_fps_vr(40, 90) == 40);
    CHECK(interp::pacing::cap_fps_vr(120, 0) == 120);
    CHECK(interp::pacing::cap_fps(120, 90, true) == 60);  // the desktop's rule for a 90 Hz monitor stays as it was
}

// the launcher's settings file: only vr.* keys change, everything else stays in place
static void test_settings_file() {
    vr::settings::File f;
    f.lines = {"# Wind Waker HD settings", "resScale=2", "vr.snapDegrees=45", "garbage line without equals", "vr.turn=smooth",
               "language=fr", "vr.snapDegrees=50", "vr.menuDistance=abc", "vr.bind.menu=Q"};
    std::vector<std::string> problems;
    const Config c = vr::settings::read_config(f, &problems);
    CHECK(c.snapDegrees == 50);              // the last of two, as the game reads it
    CHECK(c.turn == Turn::Smooth);
    CHECK(c.menuDistance == Config{}.menuDistance);  // unreadable: the default, and reported
    const Bindings b = vr::settings::read_bindings(f, &problems);
    CHECK(b.button[int(Action::Menu)] == pad::kPlus);
    CHECK(problems.size() == 2);
    Config d = c;
    d.menuDistance = 2.0f;
    d.leftHanded = true;
    vr::settings::write(f, d, b);
    std::string v;
    CHECK(f.get("resScale", v) && v == "2" && f.get("language", v) && v == "fr");  // not ours: untouched
    CHECK(f.lines[0] == "# Wind Waker HD settings" && f.lines[2] == "garbage line without equals");
    int snaps = 0;
    for (const auto& l : f.lines) snaps += l.rfind("vr.snapDegrees=", 0) == 0;
    CHECK(snaps == 1);                       // one line per key after writing
    CHECK(f.get("vr.format", v) && v == std::to_string(kSettingsFormat));
    const Config e = vr::settings::read_config(f);
    CHECK(e.menuDistance == 2.0f && e.leftHanded && e.snapDegrees == 50);
    // a later format's file: the keys this version knows still read
    vr::settings::File g;
    g.lines = {"vr.format=7", "vr.menuHeight=0.2", "vr.somethingNew=1"};
    CHECK(near(vr::settings::read_config(g).menuHeight, 0.2f));
    // out-of-range values are clamped to the documented range
    g.lines = {"vr.menuScale=99"};
    float lo, hi;
    CHECK(float_range("vr.menuScale", lo, hi) && vr::settings::read_config(g).menuScale == hi);
    // saving through a temporary file, and loading it back; a missing file is an empty one
    const std::string dir = (std::filesystem::temp_directory_path() / "wwhd_vr_test").string();
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/settings.ini";
    std::filesystem::remove(path);
    std::string err;
    vr::settings::File h;
    CHECK(h.load(path, err) && h.lines.empty());
    auto rename = [](const std::string& a, const std::string& b) {
        std::error_code ec;
        std::filesystem::rename(a, b, ec);
        return !ec;
    };
    CHECK(f.save(path, err, rename));
    CHECK(f.save(path, err, rename));  // replacing an existing file
    CHECK(h.load(path, err) && h.lines == f.lines);
    CHECK(!std::filesystem::exists(path + ".vr-tmp"));
    std::filesystem::remove_all(dir);
}

// natural arm motion: shoulder shrug, elbow side, wrist limit, twist
static void test_arm_natural() {
    ArmJoints a;
    a.upper.pos = {10, 100, 0};
    a.lower.pos = {12, 83, 2};
    a.hand.pos = {12, 65, 0};
    Joint clav;
    clav.pos = {2, 103, 0};
    ArmStyle st;
    st.pole = natural_pole(0, 1);
    // the natural pole of the right arm: down, to Link's right (heading 0: -X), back (-Z)
    CHECK(st.pole.y < -0.7f && st.pole.x < 0 && st.pole.z < 0);
    CHECK(natural_pole(0, -1).x > 0);
    // the elbow from the player's body: relaxed it matches the heading version; it follows the body when the player
    // turns, swings out when the hand is raised, and stays a unit direction
    CHECK(near(natural_pole_body({0, 0, 1}, 1, {}, 35), natural_pole(0, 1), 1e-4f));
    CHECK(near(natural_pole_body({1, 0, 0}, 1, {}, 35), natural_pole(kPi / 2, 1), 1e-4f));
    {
        const Vec3 relaxed = natural_pole_body({0, 0, 1}, 1, {0, -30, 10}, 35);
        const Vec3 raised = natural_pole_body({0, 0, 1}, 1, {0, 30, 10}, 35);
        CHECK(-raised.x > -relaxed.x && raised.y > relaxed.y && raised.y < 0);  // further out (-X is right), less down
        CHECK(std::fabs(length(raised) - 1) < 1e-4f);
        const Vec3 ahead = natural_pole_body({0, 0, 1}, -1, {0, 0, 35}, 35);
        CHECK(std::fabs(ahead.z) < 1e-4f && ahead.x > 0);  // reaching forward: under and out, not back
    }
    // the elbow stays a hinge: the animation bends it in the XY plane (hinge Z); a target that needs a bend in
    // another plane turns the upper arm about its axis, so the forearm turns relative to it about the hinge only
    {
        ArmJoints an;
        an.upper.pos = {0, 0, 0};
        an.lower.pos = {17, 0, 0};
        an.hand.pos = {17, -18, 0};  // bent 90 degrees downwards: hinge -Z (cross(+X, -Y))
        an.upper.rot = an.lower.rot = an.hand.rot = Quat{};
        const Vec3 animHinge = normalize(cross(an.lower.pos - an.upper.pos, an.hand.pos - an.lower.pos));
        ArmStyle hs;
        hs.pole = {0, 0, -1};  // the elbow out to -Z instead of the animation's +Y side
        hs.poleBlend = 1;
        hs.shrug = 0;
        Vec3 hingeLocal;
        const ArmJoints o = pose_arm_natural(an, nullptr, {20, 5, 5}, {}, hs, 1, nullptr, &hingeLocal);
        const Vec3 n = normalize(cross(o.lower.pos - o.upper.pos, o.hand.pos - o.lower.pos));
        CHECK(near(rotate(o.upper.rot, hingeLocal), n, 1e-3f));          // the upper arm's hinge on the new bend normal
        CHECK(near(rotate(o.lower.rot, animHinge), n, 1e-3f));           // the forearm's too: a pure hinge turn
        CHECK(near(rotate(o.upper.rot, {1, 0, 0}), normalize(o.lower.pos - o.upper.pos), 1e-3f));  // bone axis kept
        // a straight animated arm afterwards keeps the hinge it learnt
        ArmJoints straight = an;
        straight.hand.pos = {35, 0, 0};
        const ArmJoints o2 = pose_arm_natural(straight, nullptr, {20, 5, 5}, {}, hs, 1, nullptr, &hingeLocal);
        const Vec3 n2 = normalize(cross(o2.lower.pos - o2.upper.pos, o2.hand.pos - o2.lower.pos));
        CHECK(near(rotate(o2.upper.rot, hingeLocal), n2, 1e-3f));
    }
    // the world matched to Link's arm: 35 units against a 0.62 m arm, clamped to the settings' range
    CHECK(std::fabs(world_scale_for_arms(35, 0.62f) - 56.45f) < 0.01f && world_scale_for_arms(10, 1) == 40.0f);
    const float lu = length(a.lower.pos - a.upper.pos), ll = length(a.hand.pos - a.lower.pos);
    // a high far reach: the clavicle turns, but no more than its limit; the arm keeps its lengths
    Joint clavOut;
    const Vec3 target{20, 140, -25};
    ArmJoints p = pose_arm_natural(a, &clav, target, {0, 1, -1}, st, 1.0f, &clavOut);
    CHECK(angle_between(clavOut.rot, clav.rot) > 0.01f && angle_between(clavOut.rot, clav.rot) <= st.shrugMax + 1e-3f);
    CHECK(near(length(p.upper.pos - clav.pos), length(a.upper.pos - clav.pos), 1e-3f));  // the shoulder stays on the clavicle
    CHECK(near(length(p.lower.pos - p.upper.pos), lu, 1e-3f) && near(length(p.hand.pos - p.lower.pos), ll, 1e-2f));
    CHECK(length(p.hand.pos - target) < length(a.hand.pos - target));
    // a close reach does not shrug
    pose_arm_natural(a, &clav, a.upper.pos + Vec3{0, -10, -5}, {}, st, 1.0f, &clavOut);
    CHECK(angle_between(clavOut.rot, clav.rot) < 0.05f);
    // the wrist bends no more than its limit from the forearm, however the controller points
    p = pose_arm_natural(a, nullptr, {14, 85, -20}, {0, 0, 1}, st, 1.0f);
    const Vec3 local = rotate(conjugate(a.hand.rot), normalize(a.hand.pos - a.lower.pos));
    const Vec3 pointing = rotate(p.hand.rot, local), forearm = normalize(p.hand.pos - p.lower.pos);
    CHECK(std::acos(std::clamp(dot(pointing, forearm), -1.0f, 1.0f)) <= st.wristLimit + 1e-3f);
    // twist turns the hand about its pointing axis only (the pointing stays)
    ArmStyle tw = st;
    tw.twist = 0.8f;
    const ArmJoints q = pose_arm_natural(a, nullptr, {14, 85, -20}, {0, 0, -1}, tw, 1.0f);
    const ArmJoints r = pose_arm_natural(a, nullptr, {14, 85, -20}, {0, 0, -1}, st, 1.0f);
    CHECK(near(rotate(q.hand.rot, local), rotate(r.hand.rot, local), 1e-3f));
    CHECK(near(angle_between(q.hand.rot, r.hand.rot), 0.8f, 1e-3f));
    // detached arms (vr.detachedArms): a target beyond the reach carries the whole arm out from the shoulder by what
    // the reach lacks (at most the given shift), so the hand gets there with every bone its own length; nothing for
    // a target within reach; the wrist's and twist's limits open to a half turn
    {
        const float reach = lu + ll;
        const Vec3 far = a.upper.pos + normalize(Vec3{1, -0.3f, -2}) * (reach * 1.5f);
        const Vec3 shift = detached_shift(a.upper.pos, far, reach, reach);
        CHECK(near(length(shift), reach * 0.5f, 1e-3f));
        CHECK(near(normalize(shift), normalize(far - a.upper.pos), 1e-4f));
        ArmJoints moved = a;
        moved.upper.pos = moved.upper.pos + shift;
        moved.lower.pos = moved.lower.pos + shift;
        moved.hand.pos = moved.hand.pos + shift;
        const ArmJoints d = pose_arm_natural(moved, nullptr, far, {}, st, 1.0f);
        CHECK(length(d.hand.pos - far) < 0.05f * reach);  // the hand on the controller
        CHECK(near(length(d.lower.pos - d.upper.pos), lu, 1e-3f) && near(length(d.hand.pos - d.lower.pos), ll, 1e-2f));
        const ArmJoints held = pose_arm_natural(a, nullptr, far, {}, st, 1.0f);
        CHECK(length(held.hand.pos - far) > 0.4f * reach);  // without: held to the shoulder's reach
        CHECK(length(detached_shift(a.upper.pos, a.upper.pos + Vec3{0, -10, -5}, reach, reach)) == 0);  // within reach
        CHECK(near(length(detached_shift(a.upper.pos, a.upper.pos + Vec3{0, 0, -reach * 4}, reach, reach)), reach, 1e-3f));
        CHECK(length(detached_shift(a.upper.pos, Vec3{NAN, 0, 0}, reach, reach)) == 0);
        CHECK(detached_limit(st.wristLimit, kPi, 0) == st.wristLimit && detached_limit(st.wristLimit, kPi, 1) == kPi);
        CHECK(near(detached_limit(1.0f, 3.0f, 0.5f), 2.0f, 1e-6f) && detached_limit(1.0f, 3.0f, 7) == 3.0f);
        // the wrist limit opened: the hand points where the controller does, beyond the normal 80 degrees
        ArmStyle open = st;
        open.wristLimit = kPi;
        const ArmJoints w = pose_arm_natural(a, nullptr, {14, 85, -20}, {0, 0, 1}, open, 1.0f);
        CHECK(near(rotate(w.hand.rot, local), Vec3{0, 0, 1}, 1e-3f));
    }
    // an empty hand's roll from level (ArmStyle::levelUp): the same hand however the animation rolls it about the
    // forearm (Link turned, his run cycle); level with no controller roll; the controller's roll on top
    {
        const Vec3 fwdDir{0, 0, -1};
        ArmStyle lv = st;
        lv.levelUp = level_hand_up(a.hand.rot, local, fwdDir);
        CHECK(near(dot(lv.levelUp, local), 0.0f, 1e-3f));
        const ArmJoints h0 = pose_arm_natural(a, nullptr, {14, 85, -20}, fwdDir, lv, 1.0f);
        const Vec3 axisNow = normalize(a.hand.pos - a.lower.pos);
        for (float roll : {0.7f, -1.9f, 2.8f}) {
            ArmJoints rolled = a;
            rolled.hand.rot = normalize(axis_angle(axisNow, roll) * a.hand.rot);
            // without: the animation's roll comes through
            CHECK(angle_between(pose_arm_natural(rolled, nullptr, {14, 85, -20}, fwdDir, st, 1.0f).hand.rot, r.hand.rot) > 0.3f);
            const ArmJoints h = pose_arm_natural(rolled, nullptr, {14, 85, -20}, fwdDir, lv, 1.0f);
            CHECK(angle_between(h.hand.rot, h0.hand.rot) < 1e-3f);
        }
        const Vec3 f0 = rotate(h0.hand.rot, local);
        CHECK(near(roll_from_level(f0, rotate(h0.hand.rot, lv.levelUp)), 0.0f, 1e-3f));
        lv.twist = 0.6f;
        const ArmJoints h1 = pose_arm_natural(a, nullptr, {14, 85, -20}, fwdDir, lv, 1.0f);
        CHECK(near(rotate(h1.hand.rot, local), f0, 1e-3f));
        CHECK(near(roll_from_level(f0, rotate(h1.hand.rot, lv.levelUp)), 0.6f, 1e-3f));
    }
    // an empty hand held rigidly to its controller (ArmStyle::rigid): it turns exactly as the controller does, whatever
    // the animation's roll; pointing straight down, a turn of the body turns it by that much and no more (the roll from
    // level had spun it there)
    {
        const Vec3 fwdDir{0, 0, -1};
        ArmStyle rg = st;
        rg.wristLimit = kPi;  // (the wrist's limit off: the controller's turn alone)
        rg.rigid = true;
        rg.ctrlToHand = controller_to_hand(level_hand(a.hand.rot, local, fwdDir), fwdDir);
        const Quat level = controller_level(fwdDir);
        rg.ctrl = level;
        const ArmJoints h0 = pose_arm_natural(a, nullptr, {14, 85, -20}, fwdDir, rg, 1.0f);
        CHECK(near(rotate(h0.hand.rot, local), fwdDir, 1e-3f));  // level and forward: the hand points forward
        ArmJoints rolled = a;
        rolled.hand.rot = normalize(axis_angle(normalize(a.hand.pos - a.lower.pos), 1.9f) * a.hand.rot);
        CHECK(angle_between(pose_arm_natural(rolled, nullptr, {14, 85, -20}, fwdDir, rg, 1.0f).hand.rot, h0.hand.rot) < 1e-3f);
        // pointing down, then the body (and the controller with it) turned 30 degrees about the vertical
        const Quat down = normalize(axis_angle({1, 0, 0}, -kPi / 2) * level);
        rg.ctrl = down;
        const ArmJoints d0 = pose_arm_natural(a, nullptr, {14, 85, -20}, rotate(down, {0, 0, -1}), rg, 1.0f);
        rg.ctrl = normalize(yaw_quat(0.52f) * down);
        const ArmJoints d1 = pose_arm_natural(a, nullptr, {14, 85, -20}, rotate(rg.ctrl, {0, 0, -1}), rg, 1.0f);
        CHECK(near(angle_between(d1.hand.rot, d0.hand.rot), 0.52f, 2e-2f));
    }
    tw.twist = 5.0f;  // clamped
    CHECK(near(angle_between(pose_arm_natural(a, nullptr, {14, 85, -20}, {0, 0, -1}, tw, 1.0f).hand.rot, r.hand.rot), st.twistLimit, 1e-3f));
    // invalid target, zero weight: the animation
    CHECK(near(pose_arm_natural(a, &clav, {NAN, 0, 0}, {}, st, 1.0f).hand.pos, a.hand.pos));
    CHECK(near(pose_arm_natural(a, &clav, target, {}, st, 0.0f).hand.pos, a.hand.pos));
    // controller roll: level 0, rolled right +, pointing straight up has no roll
    CHECK(near(controller_roll(Quat{}), 0));
    CHECK(near(controller_roll(axis_angle({0, 0, -1}, 0.5f)), 0.5f, 1e-3f));
    CHECK(near(controller_roll(yaw_quat(1.2f) * axis_angle({0, 0, -1}, -0.4f)), -0.4f, 1e-3f));
    CHECK(controller_roll(axis_angle({1, 0, 0}, kPi / 2)) == 0);
    CHECK(near(limit_angle({0, 0, 1}, {1, 0, 0}, 0.3f), rotate(axis_angle({0, 1, 0}, -0.3f), Vec3{1, 0, 0}), 1e-3f));
}

// a controller's pose through a short tracking gap (vr_tracking.h)
static void test_tracking_hold() {
    PoseHold h;
    Pose out;
    bool held = false;
    Pose a;
    a.position = {0.1f, 1.2f, -0.3f};
    CHECK(!hold_pose(h, false, false, a, 0.0, 2.0, out));            // nothing seen yet
    CHECK(hold_pose(h, true, true, a, 1.0, 2.0, out, &held) && !held && near(out.position, a.position));
    Pose b;
    b.position = {9, 9, 9};
    b.orientation = axis_angle({0, 1, 0}, 0.5f);
    // position lost, orientation still valid: the last position with the fresh orientation
    CHECK(hold_pose(h, false, true, b, 1.5, 2.0, out, &held) && held && near(out.position, a.position));
    CHECK(angle_between(out.orientation, b.orientation) < 1e-4f);
    // both lost within the hold: the last valid pose
    CHECK(hold_pose(h, false, false, b, 2.9, 2.0, out, &held) && held && near(out.position, a.position));
    // a longer loss: untracked
    CHECK(!hold_pose(h, false, false, b, 3.5, 2.0, out, &held) && !held);
    // tracked again: fresh at once
    CHECK(hold_pose(h, true, true, b, 3.6, 2.0, out, &held) && !held && near(out.position, b.position));

    // the tracked bit (track_pose): a valid but untracked position (the runtime's estimate out of view) is a gap once the
    // hand has been tracked; normal tracking passes through unfiltered; back from a gap it blends, without a jump
    {
        PoseHold t;
        Pose p0, est, back;
        p0.position = {0.2f, 1.1f, -0.4f};
        est.position = {0.9f, 0.5f, 0.3f};  // drifted far off
        est.orientation = axis_angle({0, 1, 0}, 0.4f);
        back.position = {0.25f, 1.1f, -0.35f};
        CHECK(track_pose(t, true, true, true, p0, 0.0, 2.0, out, &held) && !held && near(out.position, p0.position));
        CHECK(track_pose(t, true, true, true, p0, 0.01, 2.0, out, &held) && near(out.position, p0.position));  // unfiltered
        // estimated: the last tracked position, the fresh orientation
        CHECK(track_pose(t, true, false, true, est, 0.5, 2.0, out, &held) && held && near(out.position, p0.position));
        CHECK(angle_between(out.orientation, est.orientation) < 1e-4f);
        // estimated for longer than the hold: the estimate (the best there is), not dropped; blended to it from the held
        // pose (no jump there either)
        CHECK(track_pose(t, true, false, true, est, 2.5, 2.0, out, &held) && !held && near(out.position, p0.position, 1e-4f));
        CHECK(track_pose(t, true, false, true, est, 2.65, 2.0, out, &held) && !held && near(out.position, est.position));
        // tracked again after a held stretch: blends from what was shown, done within kRecoverSeconds
        PoseHold u;
        track_pose(u, true, true, true, p0, 0.0, 2.0, out);
        track_pose(u, true, false, true, est, 0.2, 2.0, out, &held);
        CHECK(held && near(out.position, p0.position));
        CHECK(track_pose(u, true, true, true, back, 0.3, 2.0, out, &held) && !held);
        CHECK(near(out.position, p0.position, 1e-4f));  // the first tracked read starts where the held one was
        track_pose(u, true, true, true, back, 0.35, 2.0, out);
        CHECK(length(out.position - p0.position) > 0 && length(out.position - back.position) > 0);  // on the way
        track_pose(u, true, true, true, back, 0.41, 2.0, out);
        CHECK(near(out.position, back.position));  // there
        // a runtime that never reports the tracked bit: valid is enough, as before
        PoseHold v;
        CHECK(track_pose(v, true, false, true, est, 0.0, 2.0, out, &held) && !held && near(out.position, est.position));
        CHECK(track_pose(v, true, false, true, p0, 0.1, 2.0, out, &held) && !held && near(out.position, p0.position));
        // invalid: held, then dropped after the hold
        CHECK(track_pose(v, false, false, false, est, 1.0, 2.0, out, &held) && held && near(out.position, p0.position));
        CHECK(!track_pose(v, false, false, false, est, 3.0, 2.0, out, &held));
    }

    // the head's jumps (head_out_of_reach): the PSVR2 run of 2026-10-31, 1.44 m in 6 ms, 1.25 m of it down and 0.72 m
    // across, still there 0.47 s later, is still held then (it was taken, and the view sank through the floor)
    {
        const Vec3 last{0, 1.6f, 0}, sunk{0.72f, 1.6f - 1.25f, 0};
        CHECK(!head_out_of_reach({0.01f, 1.6f, 0}, last, 0.011, -1));  // a normal read
        CHECK(head_out_of_reach(sunk, last, 0.006, -1));                 // the jump
        CHECK(head_out_of_reach(sunk, last, 0.476, 0.47));               // 0.47 s later: still held
        CHECK(head_out_of_reach(sunk, last, 5.0, 4.99));
        CHECK(!head_out_of_reach({0.05f, 1.55f, 0.02f}, last, 0.48, 0.47));  // back near the last good one
        // across, a drifted estimate snapping back 1.2 m is taken once the reach has grown to it (as before)
        const Vec3 snapped{1.2f, 1.6f, 0};
        CHECK(head_out_of_reach(snapped, last, 0.011, -1));
        CHECK(head_out_of_reach(snapped, last, 0.2, 0.19));
        CHECK(!head_out_of_reach(snapped, last, 0.4, 0.39));
        // down within the reach at the jump (a crouch while held): taken
        CHECK(!head_out_of_reach({0.3f, 1.25f, 0}, last, 0.6, 0.5));
        // up, an estimate that drifted down snapping back 1.2 m: taken once the reach has grown to it
        const Vec3 drifted{0, 0.4f, 0}, found{0, 1.6f, 0};
        CHECK(head_out_of_reach(found, drifted, 0.2, 0.19));
        CHECK(!head_out_of_reach(found, drifted, 0.4, 0.39));

        // back from it the view eases (head_ease): all of the offset at first, half halfway, none after
        const Vec3 off{0, 1.0f, 0};
        CHECK(near(head_ease(off, 10.0, 10.0), off));
        CHECK(near(head_ease(off, 10.0, 10.0 + kHeadEaseSeconds / 2), off * 0.5f, 1e-4f));
        CHECK(near(head_ease(off, 10.0, 10.0 + kHeadEaseSeconds), Vec3{}));
        CHECK(near(head_ease(off, -1, 10.0), Vec3{}));
    }

    // the floor limit (hold_above_floor): a view lower than the floor is held at it, a higher one left as it is
    {
        Vec3 low{1, -30, 2}, high{1, 80, 2};
        CHECK(hold_above_floor(low, 15.0f) && near(low, Vec3{1, 15, 2}));
        CHECK(!hold_above_floor(high, 15.0f) && near(high, Vec3{1, 80, 2}));
        CHECK(!hold_above_floor(low, -INFINITY));
        // in the water (above_water): swimming, the floor rises to kViewAboveWater over the surface; a surface under his
        // feet, too far over them, or unread leaves the floor as it was
        CHECK(near(above_water(15.0f, 0.0f, 100.0f), 100.0f + kViewAboveWater));
        CHECK(near(above_water(-INFINITY, 0.0f, 100.0f), 100.0f + kViewAboveWater));
        CHECK(near(above_water(15.0f, 0.0f, -5.0f), 15.0f));
        CHECK(near(above_water(15.0f, 0.0f, kWaterReach + 1.0f), 15.0f));
        CHECK(near(above_water(15.0f, 0.0f, -INFINITY), 15.0f));
        CHECK(near(above_water(200.0f, 0.0f, 100.0f), 200.0f));  // (a higher floor stays)
        // through the head controller: the sunk view held at the floor, nothing else changed
        HeadController h;
        Pose head;
        head.position = {0, 1.6f, 0};
        const Vec3 eyes{0, 120, 0};
        h.recenter(head, 0, 1);
        Pose sunk = head;
        sunk.position.y -= 1.25f;
        const Pose free = h.first_person(sunk, eyes, 0, 1.0f, 0, 1.0f, 100.0f, 0);
        CHECK(near(free.position.y, -5.0f, 0.01f));  // 125 units below his eyes at 120: under his feet at 0
        const Pose held = h.first_person(sunk, eyes, 0, 1.0f, 0, 1.0f, 100.0f, 0, 15.0f);
        CHECK(near(held.position.y, 15.0f, 0.01f) && near(held.position.x, free.position.x, 0.01f));
        CHECK(h.take_stats().floorHeld == 1);
    }
}

// the right stick down (crouch) and the grips (closing the hands): thresholds with hysteresis; the old ZR grip
static void test_stick_down_and_grip() {
    StickDown d;
    CHECK(!d.update(0, -0.1f) && !d.update(0.05f, -0.3f));   // drift and a light push: nothing
    CHECK(!d.update(0, 0.9f) && !d.update(0.9f, 0) && !d.update(-0.9f, 0));  // up, right, left: nothing
    CHECK(!d.update(0.8f, -0.7f));                              // more sideways than down (a turn): nothing
    CHECK(d.update(0.1f, -0.7f));                               // down: pressed
    CHECK(d.update(0.1f, -0.5f));                               // eased back a little: still held
    CHECK(!d.update(0, -0.3f));                                 // back towards the centre: released
    CHECK(!d.update(0, -0.5f));                                 // between the thresholds: stays released
    GripLatch g;
    CHECK(!g.update(0.3f) && g.update(0.7f) && g.update(0.5f) && !g.update(0.4f) && !g.update(0.55f));
    Bindings b;
    b.button[int(Action::OffGrip)] = pad::kZR;                  // as saved before format 2
    migrate_bindings(b, 1);
    CHECK(b.button[int(Action::OffGrip)] == 0);
    b.button[int(Action::OffGrip)] = pad::kZR;                  // chosen again under format 2: kept
    migrate_bindings(b, 2);
    CHECK(b.button[int(Action::OffGrip)] == pad::kZR);
    CHECK(Bindings{}.button[int(Action::RightStickDown)] == pad::kZR && Bindings{}.button[int(Action::OffGrip)] == 0);
    b.button[int(Action::MainTrigger)] = pad::kA;               // as saved before format 10: cleared (the item wheel)
    migrate_bindings(b, 9);
    CHECK(b.button[int(Action::MainTrigger)] == 0 && Bindings{}.button[int(Action::MainTrigger)] == 0);
    b.button[int(Action::MainTrigger)] = pad::kA;               // chosen again under format 10: kept
    migrate_bindings(b, 10);
    CHECK(b.button[int(Action::MainTrigger)] == pad::kA);
    // the sword tilt: before format 6 it was from the fist (another angle): the new default; from format 6 a chosen
    // value stays. The swing speeds: the old tip defaults become the hand's, chosen values stay
    Config old;
    old.gripPitchDegrees = 60;
    old.swingStartSpeed = 2.6f;
    old.swingEndSpeed = 1.2f;
    migrate_config(old, 5);
    CHECK(old.gripPitchDegrees == 20.0f && Config{}.gripPitchDegrees == 20.0f);
    CHECK(old.swingStartSpeed == 1.6f && old.swingEndSpeed == 0.8f);
    old.gripPitchDegrees = -40;
    migrate_config(old, 2);
    CHECK(old.gripPitchDegrees == 20.0f);
    old.gripPitchDegrees = 35;
    old.swingStartSpeed = 3.0f;
    migrate_config(old, 5);
    CHECK(old.gripPitchDegrees == 20.0f && old.swingStartSpeed == 3.0f);
    old.gripPitchDegrees = 35;
    migrate_config(old, 6);
    CHECK(old.gripPitchDegrees == 35.0f);
}

// the view toggle: one toggle per short right click, released; never while held, nor with the recentre gesture
static void test_view_toggle() {
    ClickToggle t;
    const float dt = 1.0f / 90.0f;
    CHECK(!t.update(true, false, dt) && !t.update(true, false, dt));  // pressed: nothing yet
    CHECK(t.update(false, false, dt));                                 // released: one toggle
    CHECK(!t.update(false, false, dt));                                // and no more
    bool fired = false;
    for (int i = 0; i < 90; ++i) fired |= t.update(true, false, dt);   // held a second
    CHECK(!fired && !t.update(false, false, dt));                      // a held click never toggles
    CHECK(!t.update(true, true, dt) && !t.update(false, false, dt));   // with the left click: the recentre gesture
    CHECK(!t.update(true, false, dt) && !t.update(true, true, dt) && !t.update(false, false, dt));  // left joins later
    t.reset();
    CHECK(!t.update(false, false, dt));
}

// the sword's state: swings only while held, the draw's slash without hits, a failed draw gives up, the shoulder
// reach with hysteresis
// the first-person eyes from the game's eye point (only while his head faces forward); the hearts on the arm: shown
// while looked at (hysteresis, fade), and placed so the heart row has its width and both eyes see it at its depth
static void test_sword_state() {
    const float dt = 1.0f / 30.0f;
    SwordMachine m;
    CHECK(m.sheathed() && !m.can_swing());
    m.update(false, false, false, dt);
    CHECK(m.state == SwordState::Sheathed);               // nothing asked: stays sheathed
    m.update(false, false, true, dt);
    CHECK(m.state == SwordState::Drawing && !m.can_swing());
    m.update(true, true, false, dt);                      // in the hand, the draw's slash set
    CHECK(m.state == SwordState::Held && m.can_swing() && !m.hits_allowed());
    for (int i = 0; i < 30; ++i) m.update(true, false, false, dt);
    CHECK(m.hits_allowed());                              // after the draw's slash: real attacks hit
    m.update(true, true, false, dt);
    CHECK(m.state == SwordState::Attacking && m.hits_allowed());
    m.update(true, false, false, dt);
    CHECK(m.state == SwordState::Held);
    m.update(false, false, false, dt);
    CHECK(m.state == SwordState::Sheathing && !m.can_swing());
    for (int i = 0; i < 30; ++i) m.update(false, false, false, dt);
    CHECK(m.state == SwordState::Sheathed);
    m.update(false, false, true, dt);
    for (int i = 0; i < 60; ++i) m.update(false, false, false, dt);
    CHECK(m.state == SwordState::Sheathed);               // a draw that never reached the hand gives up
    ShoulderReach r;
    CHECK(!r.update({0.4f, -0.3f, -0.3f}, 1));            // in front, low: no
    CHECK(r.update({0.15f, -0.05f, 0.1f}, 1));            // over the right shoulder: yes
    CHECK(r.update({0.15f, -0.05f, 0.1f + 0.3f}, 1));     // a little beyond the entry: still in (hysteresis)
    CHECK(!r.update({0.15f, 0.4f, 0.4f}, 1));             // well away: out
    CHECK(!r.update({-0.15f, -0.05f, 0.1f}, 1));          // over the other shoulder: not this hand's
}

static void test_filters() {
    CHECK(near(follow(0, 10, 1.0f, 0), 10));
    const float a = follow(0, 10, 0.1f, 0.25f);
    const float b = follow(follow(0, 10, 0.05f, 0.25f), 10, 0.05f, 0.25f);
    CHECK(a > 0 && a < 10 && near(a, b, 1e-4f));  // two half steps = one step
}

// the wrist displays: one pane per element is the wrist's (docs/VR_WRIST_HUD.md, "One display of each element")
// the wrist HUD (vr_wrist_hud.h, docs/VR_WRIST_HUD.md): what it shows and where it goes
static void test_wrist_hud() {
    // hearts by quarters, as the save keeps life: 3 hearts and a quarter of 5
    CHECK(heart_count(20) == 5 && heart_count(12) == 3 && heart_count(15) == 3 && heart_count(80) == 20 && heart_count(0) == 0);
    CHECK(heart_quarters(13, 0) == 4 && heart_quarters(13, 2) == 4 && heart_quarters(13, 3) == 1 && heart_quarters(13, 4) == 0);
    CHECK(heart_quarters(0, 0) == 0 && heart_quarters(80, 19) == 4);
    // the panels' parts of the picture do not overlap and fit in it
    for (int a = 0; a < kWristPanels; ++a) {
        const WristRect& r = kWristRects[a];
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= int(kWristHudWidth) && r.y + r.h <= int(kWristHudHeight));
        for (int b = a + 1; b < kWristPanels; ++b) {
            const WristRect& o = kWristRects[b];
            CHECK(r.y + r.h <= o.y || o.y + o.h <= r.y || r.x + r.w <= o.x || o.x + o.w <= r.x);
        }
    }
    // placement in the grip's frame (identity grip at the origin): the hearts face out of the back of the left hand
    // (-X), the items out of its palm (+X), the rupees out of the back of the right hand (+X); up along the forearm
    // towards the fingers (-Z); right-handed (not mirrored); placed towards the elbow and off the wrist
    const Pose grip{};
    const WristPlacement at{0.07f, 0.035f};
    struct Want { WristPanel panel; Vec3 out; };
    // (the rupees under the right wrist, its palm -X; the magic on top of it, its back +X)
    for (const Want& w : {Want{kWristHearts, {-1, 0, 0}}, Want{kWristItems, {1, 0, 0}}, Want{kWristRupees, {-1, 0, 0}},
                          Want{kWristMagic, {1, 0, 0}}}) {
        const Pose p = wrist_panel_pose(grip, w.panel, at);
        const Vec3 x = rotate(p.orientation, {1, 0, 0}), y = rotate(p.orientation, {0, 1, 0}), z = rotate(p.orientation, {0, 0, 1});
        CHECK(near(z, w.out, 1e-4f) && near(y, Vec3{0, 0, -1}, 1e-4f));
        CHECK(near(cross(x, y), z, 1e-4f));  // right-handed: the text reads left to right
        CHECK(near(p.position, Vec3{0, 0, 0.07f} + w.out * 0.035f, 1e-5f));
    }
    // the magic on the right wrist and the hearts on the left read the same way when both backs face up: palms down
    // (left: +X down to the floor... its back -X up; right: its back +X up), each panel's x points to the wearer's right
    {
        const Pose left{yaw_quat(0) * axis_angle({0, 0, 1}, -kPi / 2), {}};   // the left hand rolled: its -X up
        const Pose right{axis_angle({0, 0, 1}, kPi / 2), {}};                 // the right hand rolled: its +X up
        const Pose h = wrist_panel_pose(left, kWristHearts, at), r = wrist_panel_pose(right, kWristMagic, at);
        CHECK(near(rotate(h.orientation, {0, 0, 1}), Vec3{0, 1, 0}, 1e-4f) && near(rotate(r.orientation, {0, 0, 1}), Vec3{0, 1, 0}, 1e-4f));
        CHECK(near(rotate(h.orientation, {1, 0, 0}), rotate(r.orientation, {1, 0, 0}), 1e-4f));  // the same reading direction
    }
    // a grip somewhere: the panel moves and turns with it
    {
        const Pose g{axis_angle({0, 1, 0}, 0.7f), {0.3f, 1.1f, -0.4f}};
        const Pose p = wrist_panel_pose(g, kWristMagic, at);
        CHECK(near(p.position, g.position + rotate(g.orientation, Vec3{0.035f, 0, 0.07f}), 1e-5f));
        // the magic bar: the whole panel once Link has magic (the first meter or doubled); filled by what is left
        CHECK(magic_bar_length(0) == 0 && magic_bar_length(16) == 1.0f && magic_bar_length(32) == 1.0f);
        CHECK(magic_bar_fill(8, 16) == 0.5f && magic_bar_fill(0, 32) == 0 && magic_bar_fill(40, 32) == 1.0f && magic_bar_fill(5, 0) == 0);
        CHECK(near(rotate(p.orientation, {0, 0, 1}), rotate(g.orientation, {1, 0, 0}), 1e-4f));
    }
    // level and turned to the eyes (the hearts and rupees): faces the head, its x horizontal, right-handed; from above
    // its x is the head's right
    for (const Vec3& headAt : {Vec3{0.1f, 1.6f, 0.2f}, Vec3{-0.3f, 1.2f, -0.1f}}) {
        const Vec3 panelAt{0.05f, 1.05f, -0.3f};
        const Quat q = level_facing(panelAt, headAt, {1, 0, 0});
        const Vec3 x = rotate(q, {1, 0, 0}), y = rotate(q, {0, 1, 0}), z = rotate(q, {0, 0, 1});
        CHECK(near(z, normalize(headAt - panelAt), 1e-4f) && std::fabs(x.y) < 1e-4f && y.y > 0);
        CHECK(near(cross(x, y), z, 1e-4f));
    }
    {
        const Quat q = level_facing({0, 1, -0.2f}, {0, 1.5f, -0.2f}, {0, 0, -1});  // straight below the eyes
        CHECK(near(rotate(q, {1, 0, 0}), Vec3{0, 0, -1}, 1e-4f) && near(rotate(q, {0, 0, 1}), Vec3{0, 1, 0}, 1e-4f));
    }
    // shown while it faces the eyes: within 45 degrees to show, 60 to stay; not from behind or far away
    const Pose face{{}, {0, 0, -0.4f}};  // facing +Z, the eyes at the origin
    CHECK(wrist_panel_facing(face, {0, 0, 0}, false));
    CHECK(!wrist_panel_facing(face, {0, 0, -0.8f}, true));                     // its back
    const Vec3 at50{std::sin(0.87f) * 0.4f, 0, -0.4f + std::cos(0.87f) * 0.4f};  // 50 degrees off its face
    CHECK(!wrist_panel_facing(face, at50, false) && wrist_panel_facing(face, at50, true));
    CHECK(!wrist_panel_facing(face, {0, 0, 1.0f}, true));                      // too far
}

static void test_item_wheel() {
    // ---- the inventory: owned items only, no bags (their contents are pages), no swords, bottles as their contents
    WheelSave s;
    s.items[0] = 0x20;   // telescope
    s.items[4] = 0x24;   // the Spoils Bag
    s.items[12] = 0x35;  // bow with fire and ice arrows
    s.items[13] = 0x31;  // bombs
    s.items[14] = 0x57;  // a bottled fairy
    s.items[15] = 0x50;  // an empty bottle
    s.items[19] = 0x3E;  // (a sword where none belongs: never offered)
    s.beast[0] = 0x45, s.beastNum[0] = 3;  // three skull necklaces
    s.bait[0] = 0x82, s.baitNum[0] = 5;    // all-purpose bait, but no Bait Bag: not offered
    s.arrows = 30, s.bombs = 0;
    s.buttons[1] = 13;  // the bombs on Y
    std::vector<WheelPageItems> pages = wheel_pages(s);
    CHECK(pages.size() == 2);
    CHECK(pages[0].page == WheelPage::Items && pages[1].page == WheelPage::Spoils);
    const auto& items = pages[0].entries;
    CHECK(items.size() == 5);  // telescope, bow, bombs, fairy, empty bottle
    CHECK(items[0].item == 0x20 && items[0].key == 0 && items[0].count == -1 && items[0].button == -1);
    CHECK(items[1].item == 0x35 && items[1].count == 30 && !items[1].empty && items[1].detail == "Fire and Ice Arrows");
    CHECK(items[2].item == 0x31 && items[2].count == 0 && items[2].empty && items[2].button == 1);
    CHECK(items[3].item == 0x57 && items[3].key == 14 && std::string(item_name(items[3].item)) == "Fairy");
    for (const auto& p : pages)
        for (const auto& e : p.entries) CHECK(!sword_or_shield(e.item) && !bag_item(e.item));
    CHECK(pages[1].entries.size() == 1 && pages[1].entries[0].key == 0x18 && pages[1].entries[0].count == 3);
    // the Bait Bag owned: its page; an empty save: nothing at all
    s.items[11] = 0x2C;
    pages = wheel_pages(s);
    CHECK(pages.size() == 3 && pages[2].page == WheelPage::Bait && pages[2].entries[0].key == 0x24 && pages[2].entries[0].count == 5);
    CHECK(wheel_pages(WheelSave{}).empty());
    // the sails are not on the wheel (asked for in the headset)
    {
        WheelSave sail;
        sail.items[1] = 0x78;
        CHECK(wheel_pages(sail).empty());
        sail.items[1] = 0x77;
        CHECK(wheel_pages(sail).empty());
    }
    // the layout: the title's band above the ring; the icons inside it, apart, as large as fit
    for (int n : {1, 4, 9, 16, 17}) {
        const WheelLayout l = wheel_layout(768.0f, n);
        CHECK(l.ring <= 384.0f - kWheelTitleBand && l.icon >= 12.0f);
        CHECK(l.centres + l.icon * kWheelChosenScale <= l.ring);  // the chosen icon inside the ring
        if (n > 1) CHECK(2.0f * l.centres * std::sin(kPi / float(n)) >= 2.0f * l.icon);  // neighbours apart
    }

    // ---- the game's assignment: the key on the button; a button that held it takes the button's old key
    const uint8_t before[3] = {0x00, 0x0D, 0xFF};  // telescope on X, bombs on Y, nothing on R
    ButtonKeys k = assign_button(before, 0, 0x0D);  // the bombs on X: the telescope moves to Y
    CHECK(k.keys[0] == 0x0D && k.keys[1] == 0x00 && k.keys[2] == 0xFF);
    k = assign_button(before, 2, 0x0E);  // a bottle on R: the others stay
    CHECK(k.keys[0] == 0x00 && k.keys[1] == 0x0D && k.keys[2] == 0x0E);
    k = assign_button(before, 0, 0x00);  // already there: nothing changes
    CHECK(k.keys[0] == 0x00 && k.keys[1] == 0x0D);

    // ---- the gesture (body frame from the head: x right, y up, z back; the off hand on the left)
    const float dt = 1.0f / 60.0f;
    const Vec3 shoulder = ItemWheel::shoulder(-1), side{-0.25f, -0.45f, 0.0f}, front{-0.1f, -0.3f, -0.35f};
    auto input = [&](Vec3 hand) {
        WheelInput in;
        in.allowed = true, in.tracked = true, in.hand = hand, in.side = -1, in.dt = dt, in.grip = true;
        return in;
    };
    auto run = [&](ItemWheel& w, Vec3 hand, int frames, WheelEvent* last = nullptr) {
        WheelEvent any;
        for (int i = 0; i < frames; ++i) {
            const WheelEvent e = w.update(input(hand));
            any.opened |= e.opened, any.committed |= e.committed, any.cancelled |= e.cancelled;
        }
        if (last) *last = any;
    };
    ItemWheel w;
    w.set_pages({5, 1});
    WheelEvent e;
    // passing through the shoulder (a few frames) opens nothing
    run(w, shoulder, 3, &e);
    run(w, side, 5);
    CHECK(!e.opened && w.phase() == WheelPhase::Idle);
    // held there: it opens once
    run(w, shoulder, 20, &e);
    CHECK(e.opened && w.phase() == WheelPhase::Selecting);
    // the stick: one step per push, repeats while held, the selection stays when it is let go
    WheelInput in = input(shoulder);
    in.stickX = 0.9f;
    w.update(in);
    CHECK(w.index() == 1);
    for (int i = 0; i < int(ItemWheel::kRepeatFirst / dt) - 2; ++i) w.update(in);
    CHECK(w.index() == 1);  // not yet repeating
    for (int i = 0; i < 6; ++i) w.update(in);
    CHECK(w.index() == 2);  // the first repeat
    in.stickX = 0;
    for (int i = 0; i < 30; ++i) w.update(in);
    CHECK(w.index() == 2);
    in.stickX = -0.9f;
    w.update(in);
    CHECK(w.index() == 1);
    in.stickX = 0, in.stickY = -0.9f;  // down: the next page
    w.update(in);
    CHECK(w.page() == 1 && w.index() == 0);
    in.stickY = 0;
    w.update(in);
    in.stickY = 0.9f;  // up: back (wraps)
    w.update(in);
    CHECK(w.page() == 0);
    in.stickY = 0;
    w.update(in);
    // a small push inside the dead zone does nothing
    in.stickX = 0.4f;
    for (int i = 0; i < 60; ++i) w.update(in);
    CHECK(w.index() == 0);
    // the hand forward: committed once, closed; then the cooldown
    run(w, Vec3{-0.3f, -0.4f, -0.1f}, 3);  // on the way (out of the shoulder, not yet in front)
    CHECK(w.phase() == WheelPhase::Committing);
    run(w, front, 1, &e);
    CHECK(e.committed && w.phase() == WheelPhase::Cooldown);
    run(w, front, 60, &e);
    CHECK(!e.opened && !e.committed);
    // back to the shoulder: it opens again (the hand has left it since)
    run(w, shoulder, 20, &e);
    CHECK(e.opened);
    // the hand dropped to the side (not to the front): cancelled after the commit window, nothing chosen
    run(w, side, int(ItemWheel::kCommitWindow / dt) + 5, &e);
    CHECK(e.cancelled && !e.committed);
    // tracking lost while choosing: cancelled, never committed
    run(w, side, 30);
    run(w, shoulder, 20, &e);
    CHECK(e.opened);
    WheelInput lost = input(front);
    lost.tracked = false;
    e = w.update(lost);
    CHECK(e.cancelled && !e.committed);
    // play stops allowing it (an event, a menu, Link falls): cancelled
    run(w, side, 30);
    run(w, shoulder, 20, &e);
    CHECK(e.opened);
    WheelInput blocked = input(shoulder);
    blocked.allowed = false;
    e = w.update(blocked);
    CHECK(e.cancelled);
    // the right stick clicked: cancelled
    run(w, side, 30);
    run(w, shoulder, 20, &e);
    CHECK(e.opened);
    WheelInput click = input(shoulder);
    click.cancel = true;
    e = w.update(click);
    CHECK(e.cancelled && !e.committed);
    // the sword hand at its own shoulder (its draw): the wheel does not open
    run(w, side, 30);
    for (int i = 0; i < 30; ++i) {
        WheelInput both = input(shoulder);
        both.swordReaching = true;
        CHECK(!w.update(both).opened);
    }
    // not allowed (third person, a menu): it never opens
    ItemWheel n;
    n.set_pages({3});
    for (int i = 0; i < 30; ++i) {
        WheelInput no = input(shoulder);
        no.allowed = false;
        CHECK(!n.update(no).opened);
    }
    // the sword hand's trigger while Link holds nothing: a press opens it (wherever the off hand is), the stick chooses,
    // letting go commits; with something in hand, or held from before, it opens nothing; the stick's click cancels
    {
        auto trig = [&](bool held, bool empty = true) {
            WheelInput t = input(side);
            t.grip = false, t.trigger = held, t.triggerHand = true, t.emptyHanded = empty;
            return t;
        };
        ItemWheel tw;
        tw.set_pages({5});
        CHECK(!tw.update(trig(true, false)).opened);  // the sword out: nothing
        tw.update(trig(false));
        CHECK(!tw.update(trig(true, false)).opened);
        CHECK(!tw.update(trig(true)).opened);         // put away while held: not a new press
        tw.update(trig(false));
        CHECK(tw.update(trig(true)).opened && tw.by_trigger());
        WheelInput t = trig(true);
        t.stickX = 0.9f;
        CHECK(!tw.update(t).committed && tw.index() == 1);
        for (int i = 0; i < 30; ++i) CHECK(!tw.update(trig(true)).committed);  // held: still choosing
        e = tw.update(trig(false));
        CHECK(e.committed && tw.index() == 1 && tw.phase() == WheelPhase::Cooldown);
        for (int i = 0; i < 30; ++i) tw.update(trig(false));
        CHECK(tw.update(trig(true)).opened);
        t = trig(true);
        t.cancel = true;
        e = tw.update(t);
        CHECK(e.cancelled && !e.committed);
        for (int i = 0; i < 30; ++i) tw.update(trig(false));
        CHECK(tw.update(trig(true)).opened);
        t = trig(true);
        t.triggerHand = false;  // the sword hand lost: cancelled, not committed
        e = tw.update(t);
        CHECK(e.cancelled && !e.committed);
    }
    // the right shoulder for a left-handed player's off hand
    ItemWheel r;
    r.set_pages({3});
    run(r, ItemWheel::shoulder(1), 20, &e);
    CHECK(!e.opened);  // measured against the left shoulder: not there
    bool opened = false;
    for (int i = 0; i < 20; ++i) {
        WheelInput right = input(ItemWheel::shoulder(1));
        right.side = 1;
        opened |= r.update(right).opened;
    }
    CHECK(opened);
    // the grip: the hand over the shoulder alone opens nothing; let go at the shoulder it closes without a choice; let
    // go during the pull forward it still chooses
    {
        ItemWheel gw;
        gw.set_pages({4});
        bool openedNoGrip = false;
        for (int i = 0; i < 30; ++i) {
            WheelInput ng = input(shoulder);
            ng.grip = false;
            openedNoGrip |= gw.update(ng).opened;
        }
        CHECK(!openedNoGrip && gw.phase() == WheelPhase::Idle);
        run(gw, side, 5);
        run(gw, shoulder, 20, &e);
        CHECK(e.opened);
        WheelInput let = input(shoulder);
        let.grip = false;
        e = gw.update(let);
        CHECK(e.cancelled && !e.committed);
        run(gw, side, 40);
        run(gw, shoulder, 20, &e);
        CHECK(e.opened);
        WheelEvent pulled;
        for (Vec3 h : {Vec3{-0.3f, -0.4f, -0.1f}, front}) {
            WheelInput p = input(h);
            p.grip = false;  // let go as the arm pulls forward
            const WheelEvent pe = gw.update(p);
            pulled.committed |= pe.committed, pulled.cancelled |= pe.cancelled;
        }
        CHECK(pulled.committed && !pulled.cancelled);
    }
    // the forward pull takes the item out (presses its button), except what a press would use up or start
    CHECK(wheel_presses_button(0x20) && wheel_presses_button(0x27) && wheel_presses_button(0x31) &&
          wheel_presses_button(0x50) && wheel_presses_button(0x29) && wheel_presses_button(0x2F));
    CHECK(!wheel_presses_button(0x51) && !wheel_presses_button(0x57) && !wheel_presses_button(0x82) &&
          !wheel_presses_button(0x45) && !wheel_presses_button(0x98) && !wheel_presses_button(0x21));
    // the ring: the first item at the top, the next clockwise
    float x, y;
    ring_position(0, 4, 100, x, y);
    CHECK(std::fabs(x) < 1e-3f && std::fabs(y + 100) < 1e-3f);
    ring_position(1, 4, 100, x, y);
    CHECK(std::fabs(x - 100) < 1e-3f && std::fabs(y) < 1e-3f);
}

static void test_zoom_magnification() {
    // the telescope and the Picto Boxes: the game narrows the subject camera's field of view to 0.444 of the normal
    // on entry (dCamera_c::subjectCamera: m39C = fovy * 0.44444445), then the zoom (x1 to x9) narrows it further
    const float normal = 60.0f;
    const float entry = 2.0f * std::atan(std::tan(normal * 0.5f * kPi / 180.0f) * 0.44444445f) * 180.0f / kPi;
    CHECK(near(zoom_magnification(normal, normal), 1.0f));
    CHECK(near(zoom_magnification(normal, entry), 2.25f, 1e-3f));  // 1 / 0.444: the Picto Box's x1, true scale in its window
    CHECK(zoom_magnification(normal, 10.0f) > zoom_magnification(normal, 20.0f));  // narrower: more
    CHECK(near(zoom_magnification(normal, 0.5f), 1.0f));   // out of range: none
    CHECK(near(zoom_magnification(normal, 90.0f), 1.0f));  // wider than normal: never below 1
    CHECK(near(zoom_magnification(normal, 1.1f), 16.0f));  // capped
    CHECK(near(zoom_magnification(0.0f, 30.0f), 1.0f));    // no normal yet: none
}

static void test_pointing_aim() {
    // dCamera_c::CalcSubjectAngle's conventions, measured on the hookshot: the yaw is the view's heading (0 along +Z),
    // the pitch the negative of its elevation (its -12743 looked up 70 degrees)
    int16_t pitch = 1, yaw = 1;
    CHECK(pointing_aim({0, 0, 1}, 12743, pitch, yaw));
    CHECK(yaw == 0 && pitch == 0);
    CHECK(pointing_aim({1, 0, 0}, 12743, pitch, yaw));
    CHECK(yaw == 0x4000 && pitch == 0);
    CHECK(pointing_aim({0, 0, -1}, 12743, pitch, yaw));
    CHECK(yaw == int16_t(0x8000) || yaw == 0x7FFF);
    CHECK(pointing_aim({0, 1, 1}, 12743, pitch, yaw));  // up 45 degrees
    CHECK(pitch == -0x2000 && yaw == 0);
    CHECK(pointing_aim({0, -1, 1}, 12743, pitch, yaw));  // down 45 degrees
    CHECK(pitch == 0x2000);
    CHECK(pointing_aim({0, 1, 0.01f}, 12743, pitch, yaw));  // straight up: held at the game's limit
    CHECK(pitch == -12743);
    CHECK(!pointing_aim({0, 0, 0}, 12743, pitch, yaw));
    CHECK(!pointing_aim({NAN, 0, 1}, 12743, pitch, yaw));
}

static void test_laser() {
    Pose p;
    float len = 0, size = 0;
    // a beam 2 m ahead (-Z) of a hand, the eye above and behind it
    const Vec3 from{0, 1.2f, -0.3f}, to{0, 1.2f, -2.3f}, eye{0, 1.6f, 0};
    CHECK(laser_beam_pose(from, to, eye, p, len));
    CHECK(near(len, 2.0f, 1e-4f));
    CHECK(near(p.position, {0, 1.2f, -1.3f}));
    CHECK(near(rotate(p.orientation, {0, 1, 0}), {0, 0, -1}));          // +Y along the beam
    const Vec3 face = rotate(p.orientation, {0, 0, 1});                 // +Z towards the eye, square to the beam
    CHECK(near(dot(face, {0, 0, -1}), 0.0f, 1e-4f) && face.y > 0.99f);
    CHECK(!laser_beam_pose(from, from, eye, p, len));                   // no length
    CHECK(!laser_beam_pose(from, {NAN, 0, 0}, eye, p, len));
    CHECK(laser_beam_pose({0, 0, 0}, {0, 0, -1}, {0, 0, 1}, p, len));   // looking along it: still a quad
    CHECK(near(rotate(p.orientation, {0, 1, 0}), {0, 0, -1}));
    // the dot faces the eye and grows with the distance
    CHECK(laser_dot_pose({0, 1.6f, -4}, eye, p, size));
    CHECK(near(rotate(p.orientation, {0, 0, 1}), {0, 0, 1}));
    CHECK(near(size, 4 * kLaserDotPerMetre, 1e-5f));
    float nearSize = 0;
    CHECK(laser_dot_pose({0, 1.6f, -1}, eye, p, nearSize) && nearSize < size);
    CHECK(laser_dot_pose({0, 5.6f, 0}, eye, p, size));                  // straight above: still a pose
    CHECK(!laser_dot_pose(eye, eye, p, size));
}

// the sailing views (vr_sail_view.h, docs/VR_SAILING_VIEWS.md): the mode cycle, when the boat's views may be chosen, the
// anchors on the boat, and the head-tracked view from them (HeadController::boat_view_pose)
static void test_sailing_views() {
    // 0. a Baba Bud (vr_bud_view.h): from first person, third person in the bud and its launch; first person again on
    // the Deku Leaf's glide or a landing; nothing from third person or outside normal play; a click hands it back
    {
        BudView b;
        CHECK(b.step(0x4, true, true) == BudSwitch::None);                 // (standing: nothing)
        CHECK(b.step(kProcVomitReady, true, false) == BudSwitch::None);    // (not normal play: no switch)
        CHECK(b.step(kProcVomitReady, true, true) == BudSwitch::ToThird);  // stepping in: third person
        CHECK(b.step(kProcVomitWait, false, true) == BudSwitch::None);     // inside
        CHECK(b.step(kProcVomitJump, false, true) == BudSwitch::None);     // shot out
        CHECK(b.step(kProcFall, false, true) == BudSwitch::None);          // falling from it
        CHECK(b.step(kProcVomitJump, false, false) == BudSwitch::None);    // (an event meanwhile: it waits)
        CHECK(b.step(0x93, false, true) == BudSwitch::ToFirst);            // the leaf's glide: first person
        CHECK(!b.active && b.step(0x93, true, true) == BudSwitch::None);
        BudView land;
        land.step(kProcVomitWait, true, true);
        CHECK(land.step(kProcVomitLand, false, true) == BudSwitch::ToFirst);  // landing from it
        BudView third;
        CHECK(third.step(kProcVomitReady, false, true) == BudSwitch::None);   // (third person already: nothing)
        BudView clicked;
        clicked.step(kProcVomitWait, true, true);
        clicked.cancel();                                                      // (the player clicked the stick)
        CHECK(clicked.step(0x25, false, true) == BudSwitch::None);
    }
    // 1. normal play: first and third person only
    {
        ViewCycle v;
        CHECK(v.mode == ViewMode::FirstPerson && !v.third_person());
        CHECK(v.click(false) == ViewMode::ThirdPerson && v.third_person());
        CHECK(v.click(false) == ViewMode::FirstPerson);
        for (int i = 0; i < 9; ++i) CHECK(!boat_view_mode(v.click(false)));
    }
    // 2. sailing: third person, the neck view (when it is on), the Free Roam Camera, wrapping round; 10. one click,
    // one mode (no skipping, nothing stale)
    {
        ViewCycle v;
        const ViewMode all[] = {ViewMode::ThirdPerson, ViewMode::Neck, ViewMode::FreeRoam, ViewMode::FirstPerson};
        const ViewMode noNeck[] = {ViewMode::ThirdPerson, ViewMode::FreeRoam, ViewMode::FirstPerson};
        static_assert(kFreeRoamView, "the Free Roam Camera is offered while sailing");
        // (a click in the neck view after sailing ended: third person first, then first person)
        ViewCycle n{ViewMode::Neck};
        CHECK(n.click(false) == ViewMode::FirstPerson);
        for (int lap = 0; lap < 3; ++lap) {
            if (kNeckView)
                for (ViewMode m : all) CHECK(v.click(true) == m);
            else
                for (ViewMode m : noNeck) CHECK(v.click(true) == m);
        }
        CHECK(!v.update(true) && v.mode == ViewMode::FirstPerson);
        for (ViewMode m : {ViewMode::FreeRoam, ViewMode::Neck}) CHECK(boat_view_mode(m));
        for (ViewMode m : {ViewMode::FirstPerson, ViewMode::ThirdPerson}) CHECK(!boat_view_mode(m));
    }
    // 3. leaving the boat (or sailing ending any other way) in one of its views: third person at once, then the
    // normal two again; first and third person are never changed by it
    for (ViewMode start : {ViewMode::FreeRoam, ViewMode::Neck}) {
        ViewCycle v{start};
        CHECK(v.update(false) && v.mode == ViewMode::ThirdPerson && v.third_person());
        CHECK(!v.update(false));
        CHECK(v.click(false) == ViewMode::FirstPerson);
        ViewCycle w{start};
        CHECK(w.click(false) == ViewMode::FirstPerson);  // a click after sailing ended starts from third person
    }
    for (ViewMode start : {ViewMode::FirstPerson, ViewMode::ThirdPerson}) {
        ViewCycle v{start};
        CHECK(!v.update(false) && !v.update(true) && v.mode == start);
    }
    // 4.-6. the conditions: all of them, or not sailing
    SailState ok;
    ok.ship = ok.riding = ok.greatSea = ok.playCamera = true;
    ok.shipMode = 1;
    CHECK(sailing_eligible(ok) && !std::strcmp(sail_blocker(ok), "sailing"));
    // sailing, and the boat's tools at sea: the cannon (9), the salvage arm (10) and its lift (11) keep the boat's views
    // (they had ended them, 2026-11-06)
    for (uint8_t m : {0, 1, 2, 9, 10, 11}) {
        SailState s = ok;
        s.shipMode = m;
        CHECK(sailing_eligible(s));
        ViewCycle v{ViewMode::FreeRoam};
        CHECK(!v.update(sailing_eligible(s)) && v.mode == ViewMode::FreeRoam);
        ViewCycle w{ViewMode::ThirdPerson};
        CHECK(w.click(sailing_eligible(s)) == ViewMode::Neck);
    }
    // getting on and off, events, talking, a tornado, warps, a whirlpool, being thrown
    for (uint8_t m : {3, 4, 5, 6, 7, 8, 12, 13, 14, 15, 16, 0xFF}) {
        SailState s = ok;
        s.shipMode = m;
        CHECK(!sailing_eligible(s));
    }
    {
        SailState s = ok;
        s.playCamera = false;  // 4. a cutscene, an event, a talk
        CHECK(!sailing_eligible(s));
        ViewCycle v{ViewMode::FreeRoam};
        CHECK(v.update(sailing_eligible(s)) && v.mode == ViewMode::ThirdPerson);
        s = ok;
        s.menu = true;  // 5. a menu
        CHECK(!sailing_eligible(s) && !std::strcmp(sail_blocker(s), "a menu"));
        ViewCycle w{ViewMode::Neck};
        CHECK(w.update(sailing_eligible(s)) && w.mode == ViewMode::ThirdPerson);
        s = ok;
        s.zoomView = true;
        CHECK(!sailing_eligible(s));
        s = ok;
        s.riding = false;  // 6. not on the boat yet (or any more)
        CHECK(!sailing_eligible(s));
        s = ok;
        s.greatSea = false;  // not the open world
        CHECK(!sailing_eligible(s));
        s = ok;
        s.ship = false;
        CHECK(!sailing_eligible(s));
        // getting on: third person clicked while boarding offers no boat view
        s = ok;
        s.shipMode = 3;
        ViewCycle b{ViewMode::ThirdPerson};
        CHECK(b.click(sailing_eligible(s)) == ViewMode::FirstPerson);
    }
    // the boat's controls (A also the hop, the left grip makes the left stick the D-pad): on the boat in normal play, in
    // every view and with its tools out; never off the boat, in a menu, an event or the telescope
    {
        CHECK(boat_controls(ok));
        for (uint8_t m : {0, 1, 2, 3, 6, 9, 10, 11}) {  // (getting on or off too: Link rides it)
            SailState s = ok;
            s.shipMode = m;
            CHECK(boat_controls(s));
        }
        SailState s = ok;
        s.greatSea = false;  // a boat elsewhere is still the boat
        CHECK(boat_controls(s));
        s = ok;
        s.riding = false;
        CHECK(!boat_controls(s));
        s = ok;
        s.ship = false;
        CHECK(!boat_controls(s));
        s = ok;
        s.menu = true;
        CHECK(!boat_controls(s));
        s = ok;
        s.playCamera = false;
        CHECK(!boat_controls(s));
        s = ok;
        s.zoomView = true;
        CHECK(!boat_controls(s));
        CHECK(!boat_controls(SailState{}));
        // A's hop only while sailing: at the tiller, steering, paddling; not the cannon, the salvage arm, getting on or off
        for (uint8_t m : {0, 1, 2}) {
            SailState h = ok;
            h.shipMode = m;
            CHECK(boat_hop(h));
        }
        for (uint8_t m : {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 0xFF}) {
            SailState h = ok;
            h.shipMode = m;
            CHECK(!boat_hop(h));
        }
        {
            SailState h = ok;
            h.riding = false;
            CHECK(!boat_hop(h));
            h = ok;
            h.menu = true;
            CHECK(!boat_hop(h));
        }
        // a weapon out in a boat view: first person (the boomerang 0x8B, the hookshot 0x8C, the bow 0x8D); the telescope,
        // the cannon, the salvage arm and sailing itself keep the view; first and third person never change
        for (uint32_t p : {0x8Bu, 0x8Cu, 0x8Du}) {
            CHECK(boat_weapon_proc(p));
            CHECK(boat_view_to_first_person(ViewMode::Neck, p) && boat_view_to_first_person(ViewMode::FreeRoam, p));
            CHECK(!boat_view_to_first_person(ViewMode::FirstPerson, p) && !boat_view_to_first_person(ViewMode::ThirdPerson, p));
        }
        for (uint32_t p : {0x86u, 0x88u, 0x89u, 0x8Au, 0x8Eu, 0x8Fu, 0x90u, 0x7Du})
            CHECK(!boat_weapon_proc(p) && !boat_view_to_first_person(ViewMode::FreeRoam, p) &&
                  !boat_view_to_first_person(ViewMode::Neck, p));
        // the item wheel: first person and the Red Lion's view, not third person or Free Roam
        CHECK(wheel_view(ViewMode::FirstPerson) && wheel_view(ViewMode::Neck));
        CHECK(!wheel_view(ViewMode::ThirdPerson) && !wheel_view(ViewMode::FreeRoam));
        // the triggers fire the bow in third person only, in play, with the bow out (with its arrows: 0x27, 0x35, 0x36)
        for (uint16_t bow : {uint16_t(0x27), uint16_t(0x35), uint16_t(0x36)}) {
            CHECK(triggers_fire_bow(true, true, bow));
            CHECK(!triggers_fire_bow(false, true, bow) && !triggers_fire_bow(true, false, bow));
        }
        CHECK(!triggers_fire_bow(true, true, kItemHookshot) && !triggers_fire_bow(true, true, kItemBoomerang) &&
              !triggers_fire_bow(true, true, 0x100));
        // the bow's string by its shape: the two joints at one place, not the limb nearest the hand
        {
            Vec3 p[12];
            for (int j = 0; j < 12; ++j) p[j] = {float(j) * 10.0f, float(j % 3) * 7.0f, 0};
            p[6] = p[11] = {50, 0, -30};  // (the string's middle, both halves)
            const Vec3 hand{20, 10, 0};   // nearer the limbs (joints 1 to 3) than the string
            int a = -1, b = -1;
            CHECK(bow_string_pair(p, 12, hand, a, b) && ((a == 11 && b == 6) || (a == 6 && b == 11)));
            p[11] = {200, 0, 0};          // no pair: none found (the nearest joint's search is the fallback)
            CHECK(!bow_string_pair(p, 12, hand, a, b) && a < 0);
            p[0] = p[1] = {0, 0, 0};      // (the root never counts)
            p[0] = p[1];
            CHECK(!bow_string_pair(p, 12, hand, a, b));
        }
        // the item an aiming procedure aims (its hand and laser): the boomerang's and hookshot's whatever is equipped
        CHECK(aimed_item(0x8B, 0x100) == kItemBoomerang && aimed_item(0x8B, kItemBoomerang) == kItemBoomerang);
        CHECK(aimed_item(0x80, 0x100) == kItemBoomerang && aimed_item(0x81, 0) == kItemBoomerang);
        CHECK(aimed_item(0x8C, 0x100) == kItemHookshot && aimed_item(0x83, 0x100) == kItemHookshot);
        CHECK(aimed_item(0x8D, 0x36) == 0x36 && aimed_item(0x88, 0x100) == 0x100);
        CHECK(fixed_item_side(aimed_item(0x8B, 0x100)) == 0);  // (the left hand, as the boomerang's own)
        // the boat's boomerang: flying (no laser), and taken out again once caught, not when put away unthrown
        CHECK(boat_boomerang_flying(0x8B, 0x100) && !boat_boomerang_flying(0x8B, kItemBoomerang) &&
              !boat_boomerang_flying(0x89, 0x100));
        {
            BoatBoomerangCatch k;
            CHECK(!k.step(0x8B, kItemBoomerang));  // held, aimed
            CHECK(!k.step(0x8B, 0x100));           // thrown: flying
            CHECK(!k.step(0x8B, 0x100));
            CHECK(k.step(0x89, 0x100));            // caught: put away by the game, taken out again
            CHECK(!k.step(0x89, 0x100));           // once
            CHECK(!k.step(0x8B, kItemBoomerang));  // taken out again
            CHECK(!k.step(0x89, 0x100));           // put away unthrown (A or B): not again
            CHECK(!k.step(0x8B, 0x100));
            CHECK(k.step(0x88, 0x100));            // (steering after the catch counts too)
            BoatBoomerangCatch m;
            CHECK(!m.step(0x8B, 0x100) && !m.step(0x90, 0x100));  // getting off meanwhile: nothing
        }
        // the bow's first-person aim on land (0x94) and on the boat (0x8D) alike; Link's boat procedures (0x86 to 0x91)
        CHECK(bow_aim_proc(0x94) && bow_aim_proc(0x8D));
        for (uint32_t p : {0x8Bu, 0x8Cu, 0x95u, 0x83u, 0x80u, 0x88u, 0u})
            CHECK(!bow_aim_proc(p));
        for (uint32_t p = 0x86; p <= 0x91; ++p) CHECK(ship_ride_proc(p));
        for (uint32_t p : {0x85u, 0x92u, 0x94u, 4u, 0x80u, 0x83u}) CHECK(!ship_ride_proc(p));
        // the left grip's D-pad: on the boat in any view, or third person in normal play; never first person off it
        CHECK(left_grip_dpad(true, false, false) && left_grip_dpad(true, true, true));
        CHECK(left_grip_dpad(false, true, true));
        CHECK(!left_grip_dpad(false, true, false));   // (third person in an event, a menu, conducting)
        CHECK(!left_grip_dpad(false, false, true) && !left_grip_dpad(false, false, false));  // (first person)
        // which grip: the right, on the boat and in third person on foot; the left grip guards on foot only
        CHECK(dpad_grip_side(true) == 1 && dpad_grip_side(false) == 1);
        CHECK(left_grip_guards(false, true, true) && !left_grip_guards(true, true, true));
        CHECK(!left_grip_guards(false, true, false) && !left_grip_guards(false, false, true));
        // the stick as the D-pad: its strongest direction past half a push
        CHECK(boat_dpad(0, 1) == BoatDpad::Up && boat_dpad(0, -1) == BoatDpad::Down);
        CHECK(boat_dpad(-1, 0) == BoatDpad::Left && boat_dpad(1, 0) == BoatDpad::Right);
        CHECK(boat_dpad(0.6f, 0.3f) == BoatDpad::Right && boat_dpad(-0.3f, -0.6f) == BoatDpad::Down);
        CHECK(boat_dpad(0.5f, 0.5f) == BoatDpad::Up);  // (a tie: up or down)
        CHECK(boat_dpad(0.3f, 0.3f) == BoatDpad::None && boat_dpad(0, 0) == BoatDpad::None);
        CHECK(boat_dpad(0, 0.49f) == BoatDpad::None && boat_dpad(0, 0.5f) == BoatDpad::Up);
        CHECK(boat_dpad(std::nanf(""), 1) == BoatDpad::None && boat_dpad(1, INFINITY) == BoatDpad::None);
        // the Free Roam Camera: Link not drawn there in play; the label not shown
        CHECK(free_roam_hides_link(ViewMode::FreeRoam, true));
        CHECK(!free_roam_hides_link(ViewMode::FreeRoam, false));  // (an event, a talk: the game's camera, Link seen)
        for (ViewMode m : {ViewMode::FirstPerson, ViewMode::ThirdPerson, ViewMode::Neck})
            CHECK(!free_roam_hides_link(m, true));
        static_assert(!kFreeRoamLabel, "the Free Roam label is not shown (2026-11-06)");
    }
    // the boat's frame: the same rotation as its model's base matrix (Y, then X, then Z), headings as Link's
    {
        const Quat q = boat_rotation(0, kPi / 2, 0);
        CHECK(near(rotate(q, {0, 0, 1}), heading_dir(kPi / 2), 1e-5f));
        const Quat pitch = boat_rotation(0.2f, 0, 0);  // +x: the bow down (the right-handed rotation about +x)
        CHECK(rotate(pitch, {0, 0, 1}).y < 0);
        // yaw outermost: a pitched, rolled, turned boat's bow still points along its heading on the floor
        const Vec3 bow = rotate(boat_rotation(0.2f, 1.0f, 0.1f), {0, 0, 1});
        CHECK(near(game_heading(bow), 1.0f, 1e-4f));
    }
    // 7. the anchors follow the boat: translation, turning, pitching; blended between steps as its model
    {
        BoatFrames f;
        CHECK(!f.valid);
        f.sample(0x100, 10, {{1000, 0, 2000}, 0, 0, 0});
        const Vec3 local{30, 100, 50};
        CHECK(near(f.to_world(local, 0.5f), Vec3{1030, 100, 2050}, 1e-3f));
        f.sample(0x100, 11, {{1100, 0, 2000}, 0, int16_t(0x4000), 0});  // moved 100 along x, turned a quarter
        CHECK(near(f.position(0.5f), Vec3{1050, 0, 2000}, 1e-3f));
        CHECK(near(f.heading(0.5f), kPi / 4, 1e-4f));
        CHECK(near(f.heading(1.0f), kPi / 2, 1e-4f));
        // turned a quarter: the boat's +z (the bow) is the world's +x, its +x (left) the world's -z
        CHECK(near(f.to_world({0, 0, 50}, 1.0f), Vec3{1150, 0, 2000}, 1e-2f));
        CHECK(near(f.to_world({30, 0, 0}, 1.0f), Vec3{1100, 0, 1970}, 1e-2f));
        f.sample(0x100, 11, {{1120, 0, 2000}, 0, int16_t(0x4000), 0});  // the same step again: only the latest
        CHECK(near(f.prev.pos, Vec3{1000, 0, 2000}) && near(f.cur.pos, Vec3{1120, 0, 2000}));
        // pitched: a point up the mast leans with the boat
        f.sample(0x100, 12, {{0, 0, 0}, int16_t(0x0800), 0, 0});
        f.sample(0x100, 13, {{0, 0, 0}, int16_t(0x0800), 0, 0});
        CHECK(f.to_world({0, 100, 0}, 1.0f).z > 1.0f);  // the bow down: the top leans towards the bow
        // headings across the wrap blend the short way
        f.sample(0x100, 14, {{0, 0, 0}, 0, int16_t(0x7F00), 0});
        f.sample(0x100, 15, {{0, 0, 0}, 0, int16_t(-0x7F00), 0});
        CHECK(std::fabs(std::fabs(f.heading(0.5f)) - kPi) < 1e-3f);
        // 11. another boat (a new scene, the old one gone): no blend from the old one
        f.sample(0x200, 16, {{5000, 0, 0}, 0, 0, 0});
        CHECK(f.ship == 0x200 && near(f.position(0.5f), Vec3{5000, 0, 0}));
        f.reset();
        CHECK(!f.valid && f.ship == 0);
    }
    // the geometry: believed only when it is a boat; kept once it holds still; anchors placed from it
    {
        BoatGeometry g;
        CHECK(plausible_boat(g));
        BoatGeometry bad = g;
        bad.head.z = g.mast.z;  // the head not ahead of the mast
        CHECK(!plausible_boat(bad));
        bad = g;
        bad.mast.x = NAN;
        CHECK(!plausible_boat(bad));
        BoatMeasure m;
        CHECK(!m.geometry().measured);
        const BoatGeometry real{{0, 25, 40}, {0, 230, 210}, {0, 15, -110}, false};
        for (int i = 0; i < kBoatSettleSteps - 1; ++i) CHECK(!m.add(0x100, real));
        CHECK(m.add(0x100, real) && m.geometry().measured && near(m.geometry().head, real.head));
        CHECK(!m.add(0x100, real));  // kept once
        // moving geometry (the mast being raised) is not kept
        BoatMeasure moving;
        for (int i = 0; i < 3 * kBoatSettleSteps; ++i) {
            BoatGeometry r = real;
            r.mast.y += float(i);
            CHECK(!moving.add(0x100, r));
        }
        // 11. another boat: measured again
        CHECK(!m.add(0x200, real) && !m.geometry().measured);
        // the neck view's anchor: ahead of the head (in front of the horns) and above it, facing the bow
        const SailAnchor neck = sail_anchor(ViewMode::Neck, real);
        CHECK(neck.local.z >= real.head.z && neck.local.y > real.head.y && near(neck.yaw, 0));
        CHECK(length(neck.local - real.head) > 40);  // clear of the head
        for (ViewMode mo : {ViewMode::FirstPerson, ViewMode::ThirdPerson, ViewMode::FreeRoam})
            CHECK(near(sail_anchor(mo, real).local, Vec3{}));
        // 12. the Free Roam Camera: it begins above and behind the boat's middle, facing the bow
        const FreeRoamOffsets& o = kFreeRoam;
        const Vec3 focus = free_roam_focus(real);
        FreeRoamCamera f;
        const float start = f.distance;
        CHECK(near(start, std::sqrt(o.back * o.back + o.up * o.up)) && f.slideX == 0 && f.slideZ == 0);
        CHECK(near(f.local(real), focus + Vec3{0, o.up, -o.back}, 1e-2f));
        CHECK(f.local(real).z < real.seat.z && f.local(real).y > real.head.y);  // behind Link, above the figurehead
        // the left trigger zooms in (towards the boat's middle, along the same line), the right one out; the harder,
        // the faster; a resting trigger and both at once do nothing
        f.update(1.0f, 0.0f, 0, 0, 0.1f);
        CHECK(near(f.distance, start - o.zoomPerSecond * 0.1f, 1e-2f));
        const Vec3 along = normalize(f.local(real) - focus);
        CHECK(near(along, normalize(Vec3{0, o.up, -o.back}), 1e-4f));
        f.reset();
        f.update(0.0f, 1.0f, 0, 0, 0.1f);
        CHECK(near(f.distance, start + o.zoomPerSecond * 0.1f, 1e-2f));
        f.reset();
        f.update(0.55f, 0.0f, 0, 0, 0.1f);
        CHECK(f.distance < start && f.distance > start - o.zoomPerSecond * 0.1f);
        f.reset();
        f.update(o.triggerDeadZone * 0.5f, 0.0f, 0, 0, 0.1f);
        CHECK(near(f.distance, start));
        f.update(1.0f, 1.0f, 0, 0, 0.1f);
        CHECK(near(f.distance, start));
        // the zoom's ends: never into the boat, never out of sight
        for (int i = 0; i < 200; ++i) f.update(1.0f, 0.0f, 0, 0, 0.1f);
        CHECK(near(f.distance, o.minDistance) && length(f.local(real) - focus) >= o.minDistance - 1e-2f);
        for (int i = 0; i < 200; ++i) f.update(0.0f, 1.0f, 0, 0, 0.1f);
        CHECK(near(f.distance, o.maxDistance));
        // the right stick slides it to the view's right (the boat's -X), and up on it forward along the boat (+Z, past
        // Link), down back; its height and the zoom unchanged; limited
        f.reset();
        f.update(0, 0, 1.0f, 0.0f, 0.1f);
        CHECK(near(f.local(real) - (focus + Vec3{0, o.up, -o.back}), Vec3{-o.slidePerSecond * 0.1f, 0, 0}, 1e-2f));
        f.update(0, 0, 0.0f, 1.0f, 0.1f);
        CHECK(near(f.slideZ, o.slidePerSecond * 0.1f) && near(f.distance, start));
        CHECK(near(f.local(real), focus + Vec3{-o.slidePerSecond * 0.1f, o.up, -o.back + o.slidePerSecond * 0.1f}, 1e-2f));
        {
            FreeRoamCamera past;  // pushed up long enough: ahead of Link's seat, at the same height
            for (int i = 0; i < 20; ++i) past.update(0, 0, 0.0f, 1.0f, 0.1f);
            CHECK(past.local(real).z > real.seat.z && near(past.local(real).y, FreeRoamCamera{}.local(real).y));
            FreeRoamCamera back;  // down: further behind
            back.update(0, 0, 0.0f, -1.0f, 0.1f);
            CHECK(back.local(real).z < FreeRoamCamera{}.local(real).z);
        }
        f.update(0, 0, -1.0f, -1.0f, 0.2f);
        CHECK(near(f.slideX, -o.slidePerSecond * 0.1f) && near(f.slideZ, -o.slidePerSecond * 0.1f));
        for (int i = 0; i < 200; ++i) f.update(0, 0, 1.0f, 1.0f, 0.1f);
        CHECK(near(f.slideX, o.slideLimit) && near(f.slideZ, o.slideLimit));
        // nothing for no time, a bad time or bad input
        const FreeRoamCamera was = f;
        f.update(1.0f, 0.0f, 1.0f, 1.0f, 0.0f);
        f.update(1.0f, 0.0f, 1.0f, 1.0f, std::nanf(""));
        f.update(std::nanf(""), std::nanf(""), std::nanf(""), std::nanf(""), 0.1f);
        CHECK(near(f.distance, was.distance) && near(f.slideX, was.slideX) && near(f.slideZ, was.slideZ));
        // leaving it and coming back: it starts again from its first place
        f.reset();
        CHECK(near(f.distance, start) && f.slideX == 0 && f.slideZ == 0);
        // it follows the boat's position and turn but not its pitch or roll (the horizon level, no bobbing)
        BoatFrames bf;
        bf.sample(0x100, 1, {{1000, 50, -300}, int16_t(0x0800), int16_t(0x4000), int16_t(-0x0600)});
        const Vec3 local = f.local(real);
        const Vec3 w = boat_level_to_world(bf, local, 1.0f);
        const float yaw = s16_to_radians(int16_t(0x4000));
        CHECK(near(w, Vec3{1000, 50, -300} + rotate(yaw_quat(yaw), local), 1e-2f));
        CHECK(near(w.y, 50 + local.y, 1e-2f));  // pitched and rolled, the camera's height is the level one
        BoatFrame flat = bf.cur;
        flat.ax = flat.az = 0;
        BoatFrames level;
        level.sample(0x100, 1, flat);
        CHECK(near(boat_level_to_world(level, local, 1.0f), w, 1e-2f));
    }
    // 8. the head on the boat: 6DoF, untouched; the head controller's own mapping (first person) not changed
    {
        HeadController h;
        const Pose start{yaw_quat(0.4f), {0.1f, 1.5f, -0.2f}};
        h.recenter(start, 0.3f, 0x1234);
        const WorldMapping before = h.mapping();
        h.begin_boat_view(start, int(ViewMode::Neck));
        CHECK(h.boat_view() == int(ViewMode::Neck) && !h.game_camera());
        const Vec3 anchor{5000, 300, -2000};
        const float heading = 1.2f;
        const float scale = 100.0f;
        // at the start: at the anchor, looking along the heading
        Pose p = h.boat_view_pose(start, anchor, heading, 1.0f, scale);
        CHECK(near(p.position, anchor, 1e-2f));
        CHECK(near(game_heading(rotate(p.orientation, {0, 0, -1})), heading, 1e-4f));
        // leaning forward, sideways, up and down: by the world scale
        const Vec3 leanBy{0.2f, -0.1f, 0.05f};
        const Pose lean{start.orientation, start.position + leanBy};
        p = h.boat_view_pose(lean, anchor, heading, 1.0f, scale);
        CHECK(near(length(p.position - anchor), length(leanBy) * scale, 1e-2f));
        CHECK(near(p.position.y - anchor.y, -10.0f, 1e-2f));
        // looking round and up: the head's own rotation relative to its start, exactly
        const Quat turned = normalize(yaw_quat(0.4f + 0.7f) * axis_angle({1, 0, 0}, 0.3f));
        const Pose looked = h.boat_view_pose({turned, start.position}, anchor, heading, 1.0f, scale);
        const Quat rel = normalize(conjugate(h.boat_view_pose(start, anchor, heading, 1.0f, scale).orientation) *
                                   looked.orientation);
        const Quat want = normalize(conjugate(start.orientation) * turned);
        CHECK(std::fabs(rel.x * want.x + rel.y * want.y + rel.z * want.z + rel.w * want.w) > 0.99999f);
        // the boat turning turns the view by as much, about the anchor; its position follows the anchor
        const Pose q1 = h.boat_view_pose(lean, anchor, heading + 0.5f, 1.0f, scale);
        CHECK(near(game_heading(rotate(q1.orientation, {0, 0, -1})), heading + 0.5f, 1e-4f));
        CHECK(near(length(q1.position - anchor), length(leanBy) * scale, 1e-2f));
        const Pose q2 = h.boat_view_pose(lean, anchor + Vec3{100, 0, 0}, heading, 1.0f, scale);
        CHECK(near(q2.position - p.position, Vec3{100, 0, 0}, 1e-2f));
        // the right stick's turn (the neck view, as in first person): the view turns by as much about the anchor, the
        // head still at it; turning back undoes it; the first-person mapping is not turned
        {
            HeadController n = h;
            n.boat_turn(0.3f);
            const Pose tp = n.boat_view_pose(start, anchor, heading, 1.0f, scale);
            CHECK(near(tp.position, anchor, 1e-2f));
            CHECK(near(game_heading(rotate(tp.orientation, {0, 0, -1})), wrap_radians(heading + 0.3f), 1e-4f));
            n.boat_turn(-0.3f);
            CHECK(near(game_heading(rotate(n.boat_view_pose(start, anchor, heading, 1.0f, scale).orientation, {0, 0, -1})),
                       heading, 1e-4f));
            CHECK(near(n.mapping().heading, before.heading));
            // a new boat view begins unturned
            n.boat_turn(1.0f);
            n.begin_boat_view(start, int(ViewMode::Neck));
            CHECK(near(game_heading(rotate(n.boat_view_pose(start, anchor, heading, 1.0f, scale).orientation, {0, 0, -1})),
                       heading, 1e-4f));
        }
        // the first-person mapping and calibration are not touched
        CHECK(h.calibrated() && h.calibrated_for(0x1234));
        CHECK(near(h.mapping().heading, before.heading) && near(h.mapping().origin, before.origin));
        // a click ends the boat view; the game's camera (third person) ends it too
        CHECK(h.end_game_camera() && h.boat_view() == 0 && !h.end_game_camera());
        h.begin_boat_view(start, int(ViewMode::FreeRoam));
        h.begin_game_camera(0.0f);
        CHECK(h.boat_view() == 0 && h.game_camera());
        // a new boat view begins from where the head is then
        h.begin_boat_view(lean, int(ViewMode::FreeRoam));
        CHECK(near(h.boat_view_pose(lean, anchor, heading, 1.0f, scale).position, anchor, 1e-2f));
    }
    // 9. stereo: in every boat view the two eyes keep their separation and lie along the view's own right axis
    {
        HeadController h;
        const Pose head{normalize(yaw_quat(-0.6f) * axis_angle({1, 0, 0}, -0.2f)), {0, 1.6f, 0}};
        const float ipd = 0.064f, scale = 100.0f;
        const Vec3 eyeL = head.position + rotate(head.orientation, {-ipd / 2, 0, 0});
        const Vec3 eyeR = head.position + rotate(head.orientation, {ipd / 2, 0, 0});
        const BoatGeometry g;
        for (ViewMode m : {ViewMode::FreeRoam, ViewMode::Neck}) {
            h.begin_boat_view(head, int(m));
            const SailAnchor a = m == ViewMode::FreeRoam ? SailAnchor{FreeRoamCamera{}.local(g), 0} : sail_anchor(m, g);
            const Vec3 at = a.local + Vec3{400, 0, 900};
            const float hd = 0.8f + a.yaw;
            const Pose c = h.boat_view_pose(head, at, hd, 1.0f, scale);
            const Pose l = h.boat_view_pose({head.orientation, eyeL}, at, hd, 1.0f, scale);
            const Pose r = h.boat_view_pose({head.orientation, eyeR}, at, hd, 1.0f, scale);
            CHECK(near(length(r.position - l.position), ipd * scale, 1e-2f));
            CHECK(near((l.position + r.position) * 0.5f, c.position, 1e-2f));
            CHECK(near(normalize(r.position - l.position), rotate(c.orientation, {1, 0, 0}), 1e-3f));
        }
    }
}

// VR upscaling (docs/VR_UPSCALING.md): the choice per device, and the settings saved and read back
static void test_upscaling() {
    using namespace vr::upscale;
    Caps quest; quest.superResolution = true; quest.shaderPass = true; quest.standalone = true;
    Caps frame; frame.shaderPass = true; frame.standalone = true;  // Steam Frame: SteamVR, no Meta layer settings
    Caps pc; pc.shaderPass = true;                    // a PC on AMD or Intel (no DLSS)
    Caps rtx = pc; rtx.dlss = true;                   // a PC whose NVIDIA RTX GPU runs DLSS
    // auto, the best the device offers, one upscaler only: Super Resolution on a Quest, DLSS on an RTX PC, SGSR1 on
    // the Frame or a PC without DLSS when upscaled, else the blit
    auto a = choose(Mode::Auto, quest, 1.75f);
    CHECK(a.superResolution && !a.sgsr1 && !a.dlss);
    a = choose(Mode::Auto, frame, 1.75f);
    CHECK(a.sgsr1 && !a.superResolution && !a.dlss);
    a = choose(Mode::Auto, frame, 1.0f);  // not upscaled: the blit
    CHECK(!a.sgsr1 && !a.superResolution && !a.dlss);
    a = choose(Mode::Auto, rtx, 1.5f);
    CHECK(a.dlss && !a.sgsr1 && !a.superResolution);
    a = choose(Mode::Auto, pc, 1.5f);
    CHECK(a.sgsr1 && !a.dlss);
    a = choose(Mode::Auto, pc, 1.0f);
    CHECK(!a.sgsr1 && !a.dlss && !a.superResolution);
    // SGSR2 by choice only (never auto): with motion vectors SGSR2 (SGSR1 the fallback for an eye without motion);
    // without them SGSR1
    Caps temporalQuest = frame; temporalQuest.temporal = true;
    a = choose(Mode::Sgsr2, temporalQuest, 1.75f);
    CHECK(a.sgsr2 && a.sgsr1 && !a.superResolution && !a.dlss);
    a = choose(Mode::Sgsr2, frame, 1.75f);  // no motion pass: SGSR1
    CHECK(!a.sgsr2 && a.sgsr1);
    CHECK(!choose(Mode::Auto, temporalQuest, 1.75f).sgsr2);
    Caps noShader = temporalQuest; noShader.shaderPass = false;
    CHECK(!choose(Mode::Sgsr2, noShader, 1.75f).sgsr2 && !choose(Mode::Sgsr2, noShader, 1.75f).sgsr1);
    // FSR 1 by choice only: where the renderer draws its passes, alone (no other upscaler with it)
    a = choose(Mode::Fsr1, frame, 1.75f);
    CHECK(a.fsr1 && !a.sgsr1 && !a.sgsr2 && !a.superResolution && !a.dlss);
    CHECK(choose(Mode::Fsr1, pc, 1.5f).fsr1 && choose(Mode::Fsr1, quest, 1.5f).fsr1);
    CHECK(!choose(Mode::Fsr1, noShader, 1.5f).fsr1);
    for (const Caps& k : {quest, frame, pc, rtx, temporalQuest}) CHECK(!choose(Mode::Auto, k, 1.75f).fsr1);
    // a saved "dlss" (no longer offered) runs as Auto
    CHECK(choose(Mode::Dlss, rtx, 1.5f).dlss);
    CHECK(!choose(Mode::Dlss, pc, 1.5f).dlss && choose(Mode::Dlss, pc, 1.5f).sgsr1);
    CHECK(!choose(Mode::Dlss, quest, 1.5f).dlss && choose(Mode::Dlss, quest, 1.5f).superResolution);
    // manual choices, and their fallbacks where the device lacks them
    CHECK(choose(Mode::Sgsr1, quest, 1.5f).sgsr1 && !choose(Mode::Sgsr1, quest, 1.5f).superResolution);
    CHECK(choose(Mode::Sgsr1, pc, 1.0f).sgsr1);
    Caps noPass = frame; noPass.shaderPass = false;
    CHECK(!choose(Mode::Sgsr1, noPass, 1.5f).sgsr1);
    CHECK(choose(Mode::SuperResolution, quest, 1.5f).superResolution);
    CHECK(!choose(Mode::SuperResolution, frame, 1.5f).superResolution && !choose(Mode::SuperResolution, frame, 1.5f).sgsr1);
    for (Mode m : {Mode::Off}) for (const Caps& k : {quest, frame, pc}) {
        const auto c = choose(m, k, 2.0f);
        CHECK(!c.sgsr1 && !c.superResolution);
    }
    // every choice gives a reason for the log and the status line
    for (Mode m : {Mode::Auto, Mode::Off, Mode::Sgsr1, Mode::SuperResolution, Mode::Dlss, Mode::Sgsr2, Mode::Fsr1})
        for (const Caps& k : {quest, frame, pc, rtx, temporalQuest}) CHECK(choose(m, k, 1.5f).why && *choose(m, k, 1.5f).why);
    // settings: default auto and sharpness 2; saved and read back; the sharpness clamped to 1..2
    vr::Config c;
    CHECK(c.upscaler == (vr::kPcVrBuild ? Mode::Off : Mode::Auto) &&
          c.upscaleSharpness == (vr::kSteamFrameBuild ? 1.5f : 2.0f));  // (PC VR: off; the Steam Frame: 1.5)
    for (Mode m : {Mode::Off, Mode::Sgsr1, Mode::SuperResolution, Mode::Dlss, Mode::Sgsr2, Mode::Fsr1, Mode::Auto}) {
        vr::Config w; w.upscaler = m; w.upscaleSharpness = 1.25f;
        vr::Config r;
        for (const auto& kv : vr::to_kv(w)) vr::from_kv(r, kv.first, kv.second);
        CHECK(r.upscaler == m && std::fabs(r.upscaleSharpness - 1.25f) < 1e-4f);
    }
    vr::Config r;
    CHECK(vr::from_kv(r, "vr.upscaleSharpness", "9") && r.upscaleSharpness == 2.0f);
    CHECK(c.fsrSharpness == 0.2f);
    CHECK(vr::from_kv(r, "vr.fsrSharpness", "5") && r.fsrSharpness == 2.0f);
    CHECK(vr::from_kv(r, "vr.fsrSharpness", "-1") && r.fsrSharpness == 0.0f);
    CHECK(!vr::from_kv(r, "vr.upscaler", "dlss5") && r.upscaler == vr::Config{}.upscaler);
}

// VR frame synthesis (docs/VR_FRAME_SYNTHESIS.md): the backend per runtime, the depth planes recovered from the game's
// projection, and the motion pass's values turned into the runtimes' convention (current - previous, Vulkan NDC)
static void test_frame_synthesis() {
    using namespace vr::framesynth;
    Caps quest; quest.ext = quest.fb = quest.depth = true; quest.standalone = true;   // Meta's runtime
    Caps frame; frame.ext = frame.fb = frame.depth = true; frame.standalone = true;   // SteamVR on the Steam Frame
    Caps pcSteam; pcSteam.depth = true;                                               // SteamVR on a PC: depth only
    Caps none;                                                                        // a runtime with neither
    CHECK(choose(Mode::Auto, quest).backend == Backend::ExtFrameSynthesis);           // the EXT before FB
    Caps fbOnly = quest; fbOnly.ext = false;
    CHECK(choose(Mode::Auto, fbOnly).backend == Backend::FbSpaceWarp);
    CHECK(choose(Mode::Auto, frame).backend == Backend::ExtFrameSynthesis);
    auto c = choose(Mode::Auto, pcSteam);
    CHECK(c.backend == Backend::None && c.depth);
    c = choose(Mode::Auto, none);
    CHECK(c.backend == Backend::None && !c.depth);
    Caps pcBoth = pcSteam; pcBoth.ext = true;                                         // a PC runtime with the EXT: auto keeps depth
    CHECK(choose(Mode::Auto, pcBoth).backend == Backend::None && choose(Mode::Auto, pcBoth).depth);
    CHECK(choose(Mode::On, pcBoth).backend == Backend::ExtFrameSynthesis);
    Caps noMotion = quest; noMotion.motion = false;                                   // no motion vectors: depth only
    CHECK(choose(Mode::On, noMotion).backend == Backend::None && choose(Mode::On, noMotion).depth);
    for (const Caps& k : {quest, frame, pcSteam, none}) {
        const auto off = choose(Mode::Off, k);
        CHECK(off.backend == Backend::None && !off.depth && off.why && *off.why);
        CHECK(choose(Mode::Auto, k).why && *choose(Mode::Auto, k).why);
    }
    // planes: projections built from known planes give them back; clip z -1..1 and 0..1; an infinite far plane
    auto projection = [](float n, float f, bool halfZ, float* p) {
        for (int i = 0; i < 16; ++i) p[i] = 0;
        p[0] = 1.2f, p[5] = 1.5f, p[14] = -1.0f;
        if (std::isinf(f)) { p[10] = -1.0f; p[11] = halfZ ? -n : -2.0f * n; }
        else if (halfZ) { p[10] = -f / (f - n); p[11] = -f * n / (f - n); }
        else { p[10] = -(f + n) / (f - n); p[11] = -2.0f * f * n / (f - n); }
    };
    for (bool halfZ : {false, true})
        for (float f : {1000.0f, 50000.0f, std::numeric_limits<float>::infinity()}) {
            float p[16], n = 0, fz = 0;
            projection(10.0f, f, halfZ, p);
            CHECK(planes(p, halfZ, n, fz));
            CHECK(std::fabs(n - 10.0f) < 1e-2f);
            CHECK(std::isinf(f) ? std::isinf(fz) : std::fabs(fz - f) / f < 1e-3f);
            // the depth these planes give at a point matches the projection's (0 at near, 1 at far)
            const float z = -300.0f;
            const float clipZ = p[10] * z + p[11], ndc = clipZ / -z;
            const float depth = halfZ ? ndc : ndc * 0.5f + 0.5f;
            const float expected = std::isinf(f) ? 1.0f - 10.0f / 300.0f : (fz / (fz - n)) * (1.0f - n / 300.0f);
            CHECK(std::fabs(depth - expected) < 1e-4f);
        }
    float bad[16] = {};
    float n = 0, fz = 0;
    CHECK(!planes(bad, false, n, fz));
    // motion: a point seen in two frames of a full 6DoF head move (turned and moved); the motion pass stores
    // (prev - cur) * 0.5 of the game's clip xy; the game's clip y is up and its viewport flips it to the picture
    const float prevClip[2] = {0.30f, -0.20f}, curClip[2] = {0.10f, 0.25f};
    const float stored[2] = {(prevClip[0] - curClip[0]) * 0.5f, (prevClip[1] - curClip[1]) * 0.5f};
    float sx = 0, sy = 0;
    motion_scale(true, sx, sy);
    // Vulkan NDC (y down) of the same points through the flipping viewport: x as clip, y negated
    const float prevVk[2] = {prevClip[0], -prevClip[1]}, curVk[2] = {curClip[0], -curClip[1]};
    CHECK(std::fabs(stored[0] * sx - (curVk[0] - prevVk[0])) < 1e-6f);
    CHECK(std::fabs(stored[1] * sy - (curVk[1] - prevVk[1])) < 1e-6f);
    motion_scale(false, sx, sy);  // a viewport that does not flip: Vulkan NDC y is clip y
    CHECK(std::fabs(stored[1] * sy - (curClip[1] - prevClip[1])) < 1e-6f);
    // a still point: no motion
    CHECK(0.0f * sx == 0.0f);
    // settings: off by default; saved and read back
    vr::Config d;
    CHECK(d.frameSynthesis == Mode::Off);  // (off by default on every build: PC VR, Steam Frame, Quest)
    for (Mode m : {Mode::Off, Mode::On, Mode::Auto}) {
        vr::Config w; w.frameSynthesis = m;
        vr::Config r;
        for (const auto& kv : vr::to_kv(w)) vr::from_kv(r, kv.first, kv.second);
        CHECK(r.frameSynthesis == m);
    }
    vr::Config r;
    CHECK(!vr::from_kv(r, "vr.frameSynthesis", "maybe") && r.frameSynthesis == d.frameSynthesis);
    {
        // VR dynamic resolution (vr_dynres.h, docs/VR_DYNAMIC_RESOLUTION.md): off by default; profiles; standalone
        // headsets only with multiview off; saved and read back; Custom clamped
        namespace dv = vr::dynres_vr;
        CHECK(vr::Config{}.dynamicResolution == dv::Mode::Off);
        const dv::Device pc{false, true}, quest{true, false}, frameMultiview{true, true}, turnipMultiview{true, true, true};
        CHECK(!dv::choose(dv::Mode::Off, {}, pc).on);
        const auto q = dv::choose(dv::Mode::Quality, {}, pc);
        CHECK(q.on && q.profile.minScale == 0.875f && q.profile.maxScale == 1.0f && q.profile.gpuTarget == 0.90f);
        const auto b = dv::choose(dv::Mode::Balanced, {}, quest);
        CHECK(b.on && b.profile.minScale == 0.75f && b.profile.gpuTarget == 0.85f);
        CHECK(dv::choose(dv::Mode::Performance, {}, pc).profile.minScale == 0.625f);
        CHECK(!dv::choose(dv::Mode::Balanced, {}, frameMultiview).on);  // multiview, Qualcomm's driver: off
        CHECK(dv::choose(dv::Mode::Balanced, {}, turnipMultiview).on);   // multiview, Mesa Turnip (Frame Native): on
        CHECK(dv::choose(dv::Mode::Balanced, {}, pc).on);                // (a PC with multiview: on)
        // standalone: the profiles top out at the fixed factor (0.75x), Custom keeps its range; a PC is not capped
        {
            dv::Device frame075{true, true, true, 0.75f}, pc075{false, true, false, 0.75f};
            const auto pf = dv::choose(dv::Mode::Performance, {}, frame075).profile;
            CHECK(pf.maxScale == 0.75f && pf.minScale == 0.625f);
            const auto qf = dv::choose(dv::Mode::Quality, {}, frame075).profile;
            CHECK(qf.maxScale == 0.75f && qf.minScale == 0.75f);  // (the minimum never above the maximum)
            dv::Custom full;
            full.minScale = 0.625f; full.maxScale = 1.0f;
            CHECK(dv::choose(dv::Mode::Custom, full, frame075).profile.maxScale == 1.0f);
            CHECK(dv::choose(dv::Mode::Performance, {}, pc075).profile.maxScale == 1.0f);
            dv::Device frame1{true, false, false, 1.0f};  // (a fixed factor of 1: as before)
            CHECK(dv::choose(dv::Mode::Balanced, {}, frame1).profile.maxScale == 1.0f);
        }
        dv::Custom wild;
        wild.minScale = 0.2f; wild.maxScale = 3.0f; wild.gpuTargetPercent = 120.0f; wild.settleSeconds = 0.0f;
        const auto cw = dv::choose(dv::Mode::Custom, wild, pc).profile;
        CHECK(cw.minScale == 0.5f && cw.maxScale == 1.0f && cw.gpuTarget == 0.98f && cw.settleSeconds == 0.5f);
        dv::Custom crossed;
        crossed.minScale = 0.9f; crossed.maxScale = 0.6f;
        const auto cc = dv::choose(dv::Mode::Custom, crossed, pc).profile;
        CHECK(cc.maxScale == 0.6f && cc.minScale == 0.6f);  // the minimum never above the maximum
        for (dv::Mode m : {dv::Mode::Off, dv::Mode::Quality, dv::Mode::Balanced, dv::Mode::Performance, dv::Mode::Custom}) {
            vr::Config w; w.dynamicResolution = m; w.dynresMin = 0.625f; w.dynresGpuTarget = 80.0f; w.dynresSettle = 6.0f;
            vr::Config rr;
            for (const auto& kv : vr::to_kv(w)) vr::from_kv(rr, kv.first, kv.second);
            CHECK(rr.dynamicResolution == m && rr.dynresMin == 0.625f && rr.dynresGpuTarget == 80.0f && rr.dynresSettle == 6.0f);
        }
        vr::Config rr;
        CHECK(!vr::from_kv(rr, "vr.dynamicResolution", "turbo") && rr.dynamicResolution == dv::Mode::Off);
        CHECK(vr::from_kv(rr, "vr.dynresGpuTarget", "200") && rr.dynresGpuTarget == 98.0f);  // clamped
    }
    // vr.eyeCrop: off by default (each eye's own projection has nothing to crop); Auto crops except SteamVR on Windows;
    // saved and read back
    CHECK(d.eyeCrop == vr::EyeCrop::Off);
    CHECK(!vr::eye_crop_on(vr::EyeCrop::Auto, false, true));   // SteamVR on Windows: as drawn
    CHECK(vr::eye_crop_on(vr::EyeCrop::Auto, false, false));   // VDXR, Meta's runtime: cropped
    CHECK(vr::eye_crop_on(vr::EyeCrop::Auto, true, true));     // Linux: cropped
    CHECK(vr::eye_crop_on(vr::EyeCrop::On, false, true) && !vr::eye_crop_on(vr::EyeCrop::Off, true, false));
    for (vr::EyeCrop m : {vr::EyeCrop::On, vr::EyeCrop::Off, vr::EyeCrop::Auto}) {
        vr::Config w; w.eyeCrop = m;
        vr::Config back;
        for (const auto& kv : vr::to_kv(w)) vr::from_kv(back, kv.first, kv.second);
        CHECK(back.eyeCrop == m);
    }
}

int main() {
    test_upscaling();
    test_frame_synthesis();
    test_conversion();
    test_angles();
    test_projection();
    test_config();
    test_vr_ui();
    test_melee_items();
    test_shield_flip();
    test_shield_orientation();
    test_baton();
    test_bindings();
    test_locomotion();
    test_ik();
    test_swing();
    test_flap();
    test_blade_and_sweep();
    test_filters();
    test_gameplay_camera_regression();
    test_camera_audit();
    test_head_controller();
    test_screen_blur();
    test_menu_and_panel();
    test_stick_and_angle();
    test_arm_pose();
    test_frame_rate_cap();
    test_settings_file();
    test_arm_natural();
    test_tracking_hold();
    test_stick_down_and_grip();
    test_view_toggle();
    test_sailing_views();
    test_sword_state();
    test_wrist_hud();
    test_item_wheel();
    test_zoom_magnification();
    test_pointing_aim();
    test_laser();
    test_skull_hammer();
    test_test_env();
    test_bow_and_hands();
    if (failures) {
        std::fprintf(stderr, "vr_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("vr_test: ok\n");
    return 0;
}

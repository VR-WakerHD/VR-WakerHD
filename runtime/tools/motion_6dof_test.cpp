// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The motion vectors' 6DoF geometry (docs/VR_DLSS.md, "6DoF"): the motion pass runs the game's own vertex shader on
// the previous frame's data and this frame's, and the target holds half their clip-space difference
// (motion_glsl.cpp motion_only_pixel). These checks build cameras from head poses as the game does (view from the
// pose, the perspective of stereo::camera_projection), move them on all six axes, and check what that formula gives:
// stationary points land where they were, translation shows as parallax that falls with depth and vanishes at
// infinity, rotation agrees with a rotation-only reprojection written independently (nr-bridge's, MIT), the eye shift
// (stereo::project_eye) equals a camera at the eye, and the background, cut and scale decisions of motion_history.h.
#include "gfx/stereo_projection.h"
#include "gfx/vulkan/motion_history.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

struct V3 { double x, y, z; };
struct V4 { double x, y, z, w; };
struct Q { double x, y, z, w; };
static V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
static Q mul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Q conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
static V3 rot(Q q, V3 v) {
    const Q r = mul(mul(q, {v.x, v.y, v.z, 0}), conj(q));
    return {r.x, r.y, r.z};
}
static Q axis_angle(V3 a, double rad) {
    const double s = std::sin(rad / 2);
    return {a.x * s, a.y * s, a.z * s, std::cos(rad / 2)};
}
static const Q kIdentity{0, 0, 0, 1};

// a camera as the game draws it: -Z forward, +Y up (OpenXR's view space too), the projection's rows as
// stereo::camera_projection expects them (row-major, w = -z)
struct Camera {
    V3 pos{0, 0, 0};
    Q ori = kIdentity;
    double proj[16]{};
};
static Camera camera(V3 pos, Q ori, double tanX = 1.0, double tanY = 0.8, double n = 1.0, double f = 10000.0) {
    Camera c{pos, ori, {}};
    c.proj[0] = 1.0 / tanX;
    c.proj[5] = 1.0 / tanY;
    c.proj[10] = -(f + n) / (f - n);
    c.proj[11] = -2.0 * f * n / (f - n);
    c.proj[14] = -1.0;
    return c;
}
// a world point (w 1) or a direction (w 0: at infinity) to clip space
static V4 clip(const Camera& c, V4 p) {
    const V3 world{p.x, p.y, p.z};
    V3 v = rot(conj(c.ori), p.w != 0 ? world - c.pos * p.w : world);  // view space (a direction: rotation only)
    const double w = p.w;
    const double* m = c.proj;
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3] * w, m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7] * w,
            m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11] * w, m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15] * w};
}
// motion_only_pixel's formula, word for word: (prev.xy / prev.w - cur.xy / cur.w) * 0.5
struct M2 { double x, y; };
static M2 mv(V4 prev, V4 cur) {
    return {(prev.x / prev.w - cur.x / cur.w) * 0.5, (prev.y / prev.w - cur.y / cur.w) * 0.5};
}
static M2 uv(V4 c) { return {c.x / c.w * 0.5 + 0.5, c.y / c.w * 0.5 + 0.5}; }
static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
static double len(M2 m) { return std::sqrt(m.x * m.x + m.y * m.y); }

// nr-bridge's CS_MotionVectors (src/nr_direct.h), rotation only, translation taken as zero: the pixel's direction in
// the current view, turned into the previous view, projected with the previous field of view. Written independently
// of the vertex path above; at infinity the two must agree. uv here: x right, y up (clip space's orientation)
static M2 rotation_only(Q qPrev, Q qCur, double tanX, double tanY, M2 uvCur) {
    const V3 dView{(uvCur.x * 2 - 1) * tanX, (uvCur.y * 2 - 1) * tanY, -1.0};
    const V3 dWorld = rot(qCur, dView);
    const V3 dPrev = rot(conj(qPrev), dWorld);
    const double px = -dPrev.x / dPrev.z, py = -dPrev.y / dPrev.z;
    const M2 uvPrev{(px / tanX) * 0.5 + 0.5, (py / tanY) * 0.5 + 0.5};
    return {uvPrev.x - uvCur.x, uvPrev.y - uvCur.y};
}

int main() {
    const V3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    const V4 nearPt{0.4, 0.3, -2.0, 1}, farPt{40, 30, -2000, 1}, sky{0.2, 0.1, -1.0, 0};
    const Camera still = camera({0, 0, 0}, kIdentity);

    // (mv() is the shader's expression, which motion_glsl_test pins in motion_only_pixel's text)
    // zero head movement: zero motion everywhere, infinity included
    {
        const M2 a = mv(clip(still, nearPt), clip(still, nearPt)), b = mv(clip(still, sky), clip(still, sky));
        CHECK(a.x == 0 && a.y == 0 && b.x == 0 && b.y == 0);
    }
    // each axis of translation and of rotation, alone and together: a stationary point's motion takes the current
    // pixel to where the point was (prev = cur + motion: DLSS's convention, current to previous)
    const struct { V3 move; Q turn; const char* what; } cases[] = {
        {X * 0.05, kIdentity, "x"},
        {Y * 0.05, kIdentity, "y"},
        {Z * 0.05, kIdentity, "z"},
        {{0, 0, 0}, axis_angle(Y, 0.03), "yaw"},
        {{0, 0, 0}, axis_angle(X, 0.03), "pitch"},
        {{0, 0, 0}, axis_angle(Z, 0.05), "roll"},
        {Z * -0.04, axis_angle(Y, 0.02), "turning while walking forward"},
        {Z * 0.04, axis_angle(X, -0.03), "looking down while stepping back"},
        {X * 0.04, axis_angle(Z, 0.04), "rolling while moving sideways"},
        {X * -0.05 + Z * -0.02, axis_angle(Y, -0.03), "walking sideways turning towards it"},
        {X * 0.3 + Y * 0.2 + Z * -0.4, mul(axis_angle(Y, 0.35), axis_angle(X, 0.2)), "a fast swing (a frame's worth)"},
    };
    for (const auto& k : cases) {
        const Camera cur = camera(k.move, k.turn);
        for (V4 pt : {nearPt, farPt, sky}) {
            const V4 pc = clip(still, pt), cc = clip(cur, pt);
            if (pc.w <= 0 || cc.w <= 0) continue;
            const M2 m = mv(pc, cc), u0 = uv(pc), u1 = uv(cc);
            if (!near(u1.x + m.x, u0.x) || !near(u1.y + m.y, u0.y)) {
                std::fprintf(stderr, "case %s: the point does not land where it was\n", k.what);
                CHECK(false);
            }
        }
    }
    // translation is parallax: a near point moves more than a far one, a point at infinity not at all (x, y, z)
    for (V3 move : {X * 0.05, Y * 0.05, Z * 0.05}) {
        const Camera cur = camera(move, kIdentity);
        const double n = len(mv(clip(still, nearPt), clip(cur, nearPt)));
        const double f = len(mv(clip(still, farPt), clip(cur, farPt)));
        const double s = len(mv(clip(still, sky), clip(cur, sky)));
        CHECK(n > 0 && n > 100 * f && s == 0);
    }
    // rotation: at infinity the vertex path equals the rotation-only reprojection (yaw, pitch, roll, combined)
    for (Q turn : {axis_angle(Y, 0.03), axis_angle(X, -0.02), axis_angle(Z, 0.05), mul(axis_angle(Y, 0.04), axis_angle(X, 0.03))}) {
        const Camera cur = camera({0, 0, 0}, turn);
        for (V4 dir : {sky, V4{-0.3, 0.2, -1.0, 0}, V4{0.5, -0.4, -1.0, 0}}) {
            const V4 pc = clip(still, dir), cc = clip(cur, dir);
            const M2 m = mv(pc, cc), r = rotation_only(kIdentity, turn, 1.0, 0.8, uv(cc));
            CHECK(near(m.x, r.x, 1e-9) && near(m.y, r.y, 1e-9));
        }
        // and a moving head adds no motion at infinity: the same with a translation on top
        const Camera moved = camera({0.1, -0.05, 0.2}, turn);
        const M2 m = mv(clip(still, sky), clip(moved, sky)), r = rotation_only(kIdentity, turn, 1.0, 0.8, uv(clip(moved, sky)));
        CHECK(near(m.x, r.x, 1e-9) && near(m.y, r.y, 1e-9));
    }
    // ... but at a finite depth the rotation-only reprojection misses the translation (what nr-bridge leaves as ghosting)
    {
        const Camera cur = camera(X * 0.05, axis_angle(Y, 0.02));
        const M2 m = mv(clip(still, nearPt), clip(cur, nearPt));
        const M2 r = rotation_only(kIdentity, axis_angle(Y, 0.02), 1.0, 0.8, uv(clip(cur, nearPt)));
        CHECK(std::fabs(m.x - r.x) > 1e-3);
    }
    // the eye shift: project_eye on the head camera's projection, applied to both frames, equals cameras at the eyes
    // (the head's pose plus the eye's offset along its own right), for the right eye and the left, head moving and turning
    {
        const double ipd = 0.032;
        const Camera headPrev = camera({0, 0, 0}, kIdentity), headCur = camera({0.03, 0.01, -0.02}, axis_angle(Y, 0.04));
        for (double eye : {ipd, -ipd}) {
            auto shifted = [&](const Camera& h) {
                Camera c = h;
                float p[16];
                for (int i = 0; i < 16; ++i) p[i] = float(h.proj[i]);
                CHECK(stereo::camera_projection(p));
                CHECK(stereo::project_eye(p, float(eye), INFINITY));
                for (int i = 0; i < 16; ++i) c.proj[i] = p[i];
                return c;
            };
            auto atEye = [&](const Camera& h) { return camera(h.pos + rot(h.ori, X * eye), h.ori); };
            for (V4 pt : {nearPt, farPt, sky}) {
                const M2 a = mv(clip(shifted(headPrev), pt), clip(shifted(headCur), pt));
                const M2 b = mv(clip(atEye(headPrev), pt), clip(atEye(headCur), pt));
                CHECK(near(a.x, b.x, 1e-6) && near(a.y, b.y, 1e-6));
            }
            // the eyes see different motion near (parallax between them), the same at infinity
            const M2 r = mv(clip(shifted(headPrev), nearPt), clip(shifted(headCur), nearPt));
            const M2 l = mv(clip(camera(headPrev.pos + X * -eye, kIdentity), nearPt),
                                clip(camera(headCur.pos + rot(headCur.ori, X * -eye), headCur.ori), nearPt));
            CHECK(std::fabs(r.x - l.x) > 1e-6);
        }
    }
    // the motion's pixels: the viewport's size with its sign (a negative height, y down in the picture, flips y)
    {
        CHECK(!motion::viewport_flips_y(0, 0));
        CHECK(motion::viewport_flips_y(100, 90) && !motion::viewport_flips_y(100, 10));
        CHECK(motion::motion_scale_y(1080, true) == -1080.0f && motion::motion_scale_y(1080, false) == 1080.0f);
        // a point moving up the picture (clip y up): with a flipped viewport its pixel y falls, so the scale is negative
        const Camera cur = camera(Y * -0.05, kIdentity);  // the head moves down: the point rises in view
        const M2 m = mv(clip(still, nearPt), clip(cur, nearPt));
        CHECK(m.y < 0);  // previous below current in clip space
        const double ys = -540.0;  // the game's y scale, negative: pixel y = yo + ys * ndc y
        const double pixelPrev = ys * (clip(still, nearPt).y / clip(still, nearPt).w);
        const double pixelCur = ys * (clip(cur, nearPt).y / clip(cur, nearPt).w);
        CHECK(near(pixelCur + m.y * motion::motion_scale_y(1080, true), pixelPrev, 1e-6));
    }
    // the background: draws with a 3D camera only; depths Vulkan's viewport takes
    {
        motion::DrawData d;
        float persp[16] = {};
        persp[0] = 1.2f; persp[5] = 1.6f; persp[10] = -1.0002f; persp[11] = -2.0f; persp[14] = -1.0f;
        float ortho[16] = {};
        ortho[0] = 0.002f; ortho[5] = 0.003f; ortho[10] = -0.001f; ortho[15] = 1.0f;
        unsigned char block[256] = {};
        std::memcpy(block + 32, ortho, 64);
        d.add(motion::Part::VertexSupport, 0, block, sizeof block);
        CHECK(!motion::camera_draw(d));  // a layout's orthographic projection
        std::memcpy(block + 144, persp, 64);
        motion::DrawData e;
        e.add(motion::Part::VertexBlock, 1, block, sizeof block);
        CHECK(motion::camera_draw(e));
        motion::DrawData refOnly;
        refOnly.add_ref(motion::Part::VertexBuffer, 0, 0x1000, 64);
        CHECK(!motion::camera_draw(refOnly));
        CHECK(motion::background_depth_ok(1.0f) && motion::background_depth_ok(0.0f));
        CHECK(!motion::background_depth_ok(std::nanf("")) && !motion::background_depth_ok(1.5f) &&
              !motion::background_depth_ok(-0.1f));
    }
    // history resets: a recentre or a snap turn (a jump no head makes in a frame), a new scene; not a head turn or a walk
    {
        const float upm = 100.0f;
        CHECK(!motion::view_jump(0.0f, 0.0f, upm));
        CHECK(!motion::view_jump(5.0f, 0.08f, upm));    // a fast head turn (4.6 degrees in a frame), a step
        CHECK(!motion::view_jump(250.0f, 0.0f, upm));   // 2.5 m in a frame (the boat at its fastest, a hookshot pull)
        CHECK(motion::view_jump(0.0f, 0.79f, upm));     // a 45 degree snap turn
        CHECK(motion::view_jump(400.0f, 0.0f, upm));    // a teleport, an event camera elsewhere
        CHECK(motion::view_jump(std::nanf(""), 0.0f, upm));
        CHECK(!motion::scene_changed(20, 20));          // a few new draws (an effect appearing)
        CHECK(!motion::scene_changed(600, 40));
        CHECK(motion::scene_changed(600, 450));         // a stage loaded: most draws are new
    }
    if (failures) {
        std::fprintf(stderr, "motion_6dof_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("motion_6dof_test: ok\n");
    return 0;
}

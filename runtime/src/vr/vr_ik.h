// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// Two-bone inverse kinematics for the first-person arms (docs/VR_ARCHITECTURE.md, "Arms").
//
// shoulder -> elbow -> wrist with fixed bone lengths. The wrist target comes from the tracked controller;
// it is clamped to an anatomically reachable distance (never longer than the straight arm times
// kMaxReach, never closer than kMinReach of it), so the arm neither stretches nor snaps through itself.
// The elbow lies in the plane of shoulder, wrist and the pole direction (down and out from the body for a
// natural elbow), at the angle the law of cosines gives.
#pragma once
#include <algorithm>
#include <cmath>
#include "vr_math.h"

namespace vr {

struct ArmChain {
    float upper = 0.28f;   // shoulder to elbow (metres, or any unit used consistently)
    float lower = 0.26f;   // elbow to wrist
};
struct ArmSolution {
    Vec3 shoulder, elbow, wrist;
    bool clamped = false;  // the target was out of reach and the wrist was pulled back onto the reach limit
};

constexpr float kMaxReach = 0.995f;  // of upper + lower: a straight arm never exactly (keeps the elbow defined)
constexpr float kMinReach = 0.12f;   // of upper + lower: the wrist never inside the shoulder

inline ArmSolution solve_arm(Vec3 shoulder, Vec3 target, Vec3 pole, const ArmChain& arm) {
    ArmSolution s;
    s.shoulder = shoulder;
    const float total = arm.upper + arm.lower;
    Vec3 toTarget = target - shoulder;
    float dist = length(toTarget);
    const Vec3 dir = normalize(toTarget, normalize(pole, {0, -1, 0}));
    const float maxD = total * kMaxReach;
    const float minD = std::max(std::fabs(arm.upper - arm.lower) + 1e-4f, total * kMinReach);
    if (dist > maxD || dist < minD) {
        dist = std::clamp(dist, minD, maxD);
        s.clamped = true;
    }
    s.wrist = shoulder + dir * dist;
    // law of cosines: angle at the shoulder between the shoulder->wrist line and the upper arm
    const float cosA = std::clamp((arm.upper * arm.upper + dist * dist - arm.lower * arm.lower) /
                                  (2.0f * arm.upper * dist), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    // the bend direction: the pole made perpendicular to the arm line
    auto perpendicular = [&](Vec3 v) { return v - dir * dot(v, dir); };
    Vec3 bend = perpendicular(pole);
    if (length(bend) < 1e-5f) {
        // pole along the arm: any perpendicular (prefer "down", else "right")
        bend = perpendicular({0, -1, 0});
        if (length(bend) < 1e-5f) bend = perpendicular({1, 0, 0});
    }
    bend = normalize(bend);
    s.elbow = shoulder + (dir * cosA + bend * sinA) * arm.upper;
    return s;
}

// where the shoulders sit for a head pose: below and to the side of the head, turned with the body's yaw
// only (the head's pitch and roll do not move the shoulders). side = -1 left, +1 right; OpenXR frame.
inline Vec3 shoulder_position(const Pose& head, float bodyYaw, int side, float neckDrop = 0.22f,
                              float halfWidth = 0.18f, float back = 0.06f) {
    const Quat yaw = yaw_quat(bodyYaw);
    return head.position + rotate(yaw, {float(side) * halfWidth, -neckDrop, back});
}
// a pole for the elbow: down, out to the side and slightly back
inline Vec3 elbow_pole(float bodyYaw, int side) {
    return rotate(yaw_quat(bodyYaw), normalize(Vec3{float(side) * 0.6f, -1.0f, 0.25f}));
}

// The body's facing (docs/VR_BODY_FOLLOW.md): its yaw follows the head's (tracking space, radians) only once the
// head has turned more than `enter` from it, then turns all the way to it, stopping within kExit (hysteresis: no
// jitter at the threshold, no head-torso twist left behind). Yaw only: looking up, down or tilting never turns it.
// The turn is quick at first and eases in (most of it within about a third of a second; kResponse is a rate per
// second, so frame rates agree), never slower than kMinRate, always the short way round.
struct BodyFollow {
    static constexpr float kExit = 5.0f * kPi / 180.0f;
    static constexpr float kResponse = 6.0f;  // 1/s: the share of the angle left that is turned each second
    static constexpr float kMinRate = 1.0f;   // rad/s
    float yaw = 0;
    bool following = false;

    float update(float headYaw, float dt, float enter) {
        const float diff = wrap_radians(headYaw - yaw);
        if (!following && std::fabs(diff) > std::max(enter, kExit)) following = true;
        if (!following || !(dt > 0)) return yaw;
        const float eased = std::fabs(diff) * (1.0f - std::exp(-kResponse * dt));
        const float step = std::min(std::fabs(diff), std::max(eased, kMinRate * dt));
        yaw = wrap_radians(yaw + std::copysign(step, diff));
        if (std::fabs(wrap_radians(headYaw - yaw)) <= kExit) following = false;
        return yaw;
    }
    // the game turned the body (Link moving, locked on, attacking...): it faces that way, following nothing yet
    void set(float y) {
        yaw = wrap_radians(y);
        following = false;
    }
};

// Link faces where the body faces (game heading, radians): the tracking space's turn into the game (WorldMapping
// heading, which recentring, smooth and snap turns change) plus the body's yaw in it. A turn of the view moves only
// the first, so Link turns with it exactly once.
inline float link_facing(float mapHeading, float bodyYaw) { return wrap_radians(mapHeading + bodyYaw); }
// the body's yaw for a heading the game gave Link (the inverse)
inline float body_yaw_of(float linkHeading, float mapHeading) { return wrap_radians(linkHeading - mapHeading); }
// Link's actions in which the body follows the head: standing (daPyProc_WAIT_e, daPyProc_FREE_WAIT_e). In every
// other one the game turns him (moving, lock-on 7-9, attacks, items, swimming, climbing, events)
inline bool body_follow_proc(uint32_t proc) { return proc == 4 || proc == 5; }
// the yaw the body follows while Link stands, with Detached Arms (docs/VR_BODY_FOLLOW.md, "The body follows the
// arms"): the head's, turned towards where the hands reach (their midpoint from the head, flat, in metres; tracking
// space), the more the farther out they are: none within `near`, fully from `far`. His shoulders then face the arms,
// so a reach to the side turns him instead of leaving the arm far from his body
inline float body_follow_yaw(float headYaw, bool haveHands, Vec3 handsFromHead, float near = 0.15f, float far = 0.40f) {
    const float flat = std::sqrt(handsFromHead.x * handsFromHead.x + handsFromHead.z * handsFromHead.z);
    if (!haveHands || !std::isfinite(flat) || flat <= near) return headYaw;
    const float handsYaw = std::atan2(-handsFromHead.x, -handsFromHead.z);  // as yaw_of: forward -z
    const float w = std::clamp((flat - near) / (far - near), 0.0f, 1.0f);
    return wrap_radians(headYaw + w * wrap_radians(handsYaw - headYaw));
}

// ---- what a hand holds, moved to the other hand (the sword and the shield, docs/VR_HELD_ITEMS.md)
// An item's frame in a hand's frame (rot, pos), moved to Link's other hand: his hands' frames are mirror images, so
// hand-local vectors map x -> diag(sign) x (g_mirrorSign, measured from his rig). That alone is a reflection: the
// item's axis `flipAxis` that points across his body (the item is symmetric about the plane of its other two axes: a
// sword's blade plane, a shield's up and face) is turned back so the result is a rotation again, with the item's other
// two axes (a shield's top and face) mirrored as the hand is.
inline void mirror_held(Quat rot, Vec3 pos, Vec3 sign, int flipAxis, Quat& outRot, Vec3& outPos) {
    auto m = [&](Vec3 v) { return Vec3{v.x * sign.x, v.y * sign.y, v.z * sign.z}; };
    Vec3 cols[3] = {m(rotate(rot, {1, 0, 0})), m(rotate(rot, {0, 1, 0})), m(rotate(rot, {0, 0, 1}))};
    if (dot(cross(cols[0], cols[1]), cols[2]) < 0) cols[flipAxis] = cols[flipAxis] * -1.0f;
    outRot = normalize(from_basis(cols[0], cols[1], cols[2]));
    outPos = m(pos);
}
// the item's own axis (0 x, 1 y, 2 z) that points most across Link's body (`right`, world) with the item at `rot`
// (world), leaving out an axis along `skip` (local; a sword's blade) when it is given
inline int across_axis(Quat rot, Vec3 right, Vec3 skip = {}) {
    int best = 2;
    float most = -1;
    for (int a = 0; a < 3; ++a) {
        const Vec3 e{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
        if (length(skip) > 0 && std::fabs(dot(e, skip)) > 0.7f) continue;
        const float across = std::fabs(dot(rotate(rot, e), right));
        if (across > most) { most = across; best = a; }
    }
    return best;
}

// the axis a mirrored item's frame is flipped on so that it stays a rotation: of its three axes, not the one across
// Link's body (its face: a flip there turned a shield's crest towards the body, seen in the headset) and not its up
// (a flip there put it upside down): the one left, across its own face (a symmetric shield shows no difference)
inline int mirror_flip_axis(int across, int up) {
    if (across == up) up = (across + 1) % 3;
    return 3 - across - up;
}
// the item's own axis (0 x, 1 y, 2 z) that points most up (world +Y) with the item at `rot`, other than `notAxis`
inline int up_axis(Quat rot, int notAxis) {
    int best = notAxis == 0 ? 1 : 0;
    float most = -1;
    for (int a = 0; a < 3; ++a) {
        if (a == notAxis) continue;
        const Vec3 e{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
        const float up = std::fabs(rotate(rot, e).y);
        if (up > most) { most = up; best = a; }
    }
    return best;
}

// ---- the shield in the off hand, turned by its controller (docs/VR_HELD_ITEMS.md, "The shield's orientation")
// The shield model's own axes (0 x, 1 y, 2 z): its face (the crest's side) and its top, each with its sign. The third
// is across its face, the axis it is symmetric about.
struct ShieldAxes {
    int face = -1, top = -1;
    float faceSign = 1, topSign = 1;
    bool valid() const { return face >= 0 && top >= 0 && face != top; }
    int side() const { return 3 - face - top; }
};
// the axes from sums over the game's own poses of the shield: each of its axes along the direction away from Link's
// body (`outwardSum`) and along world up (`upSum`). The face is the axis most outward, the top the one most up of the
// other two
inline ShieldAxes shield_axes(Vec3 outwardSum, Vec3 upSum) {
    auto c = [](Vec3 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
    ShieldAxes s;
    for (int a = 0; a < 3; ++a)
        if (s.face < 0 || std::fabs(c(outwardSum, a)) > std::fabs(c(outwardSum, s.face))) s.face = a;
    for (int a = 0; a < 3; ++a)
        if (a != s.face && (s.top < 0 || std::fabs(c(upSum, a)) > std::fabs(c(upSum, s.top)))) s.top = a;
    s.faceSign = c(outwardSum, s.face) < 0 ? -1.0f : 1.0f;
    s.topSign = c(upSum, s.top) < 0 ? -1.0f : 1.0f;
    return s;
}
// the shield's rotation (world) with its face towards `face` and its top towards `top` (made perpendicular to the
// face): a rotation, never a mirror image, for any directions
inline Quat shield_rotation(const ShieldAxes& s, Vec3 face, Vec3 top) {
    face = normalize(face);
    top = normalize(top - face * dot(top, face));
    Vec3 cols[3];
    cols[s.face] = face * s.faceSign;
    cols[s.top] = top * s.topSign;
    const int side = s.side();
    cols[side] = cross(cols[(side + 1) % 3], cols[(side + 2) % 3]);
    return normalize(from_basis(cols[0], cols[1], cols[2]));
}
// a point in the shield's frame (the hand holding it) moved to the other hand: mirrored across the shield's own plane
// of symmetry, so the hand stays behind its face and below its top on either side
inline Vec3 mirror_across_face(Vec3 local, const ShieldAxes& s) {
    const int side = s.side();
    return Vec3{side == 0 ? -local.x : local.x, side == 1 ? -local.y : local.y, side == 2 ? -local.z : local.z};
}
// where its face and top point in a controller's grip space (OpenXR's grip: +X out of the left palm and into the
// right one, +Y up through the fist): the face out of the back of the hand, away from the body; the top up
inline Vec3 shield_face_in_grip(int side) { return side == 0 ? Vec3{-1, 0, 0} : Vec3{1, 0, 0}; }
inline Vec3 shield_top_in_grip() { return {0, 1, 0}; }
// The shield held flush with the body (asked for in the headset, as Twilight Princess VR holds it): its face towards
// where the controller points (its aim pose's -Z), levelled, and its top straight up (tracking space +Y), so its round
// end points down however high the hand is raised (asked for). Pointing up or down, the aim's top (+Y) takes over the
// level direction, so the face turns without a jump: pointing up, the controller's top faces you, and the face is
// away from you
inline Vec3 shield_face_level(Quat aim) {
    const Vec3 fwd = rotate(aim, Vec3{0, 0, -1}), up = rotate(aim, Vec3{0, 1, 0});
    Vec3 f = fwd - up * fwd.y;
    f.y = 0;
    const float l = length(f);
    return l > 1e-4f ? f * (1.0f / l) : Vec3{0, 0, -1};
}
// and tilted with the wrist (asked for in the headset): the face pitched as the controller points, down when the wrist
// tips forward (the shield's top forward), up when it tips back, at most `maxPitch` radians, so its round end stays
// below. Its top stays square to the face in the vertical plane: no roll
inline Vec3 shield_face_tilted(Quat aim, float maxPitch = 1.047f) {
    const Vec3 level = shield_face_level(aim);
    const float y = std::clamp(rotate(aim, Vec3{0, 0, -1}).y, -1.0f, 1.0f);
    const float pitch = std::clamp(std::asin(y), -maxPitch, maxPitch);
    return level * std::cos(pitch) + Vec3{0, 1, 0} * std::sin(pitch);
}

// Link's actions in which the view's base turns to Link instead (docs/VR_BODY_FOLLOW.md, "Ladders" and "Ledges"):
//   - on a ladder (daPyProc_LADDER_UP_START_e 0x38, _UP_END_e 0x39, _DOWN_START_e 0x3A, _DOWN_END_e 0x3B, _MOVE_e
//     0x3C). Climbing down, the game turns him round to face the ladder (procLadderDownStart_init: shape_angle.y -
//     0x8000) while the view kept facing the drop;
//   - hanging from a ledge (daPyProc_HANG_START_e 0x2B, _FALL_START_e 0x2C, _UP_e 0x2D, _WAIT_e 0x2E, _MOVE_e 0x2F,
//     _CLIMB_e 0x30, _WALL_CATCH_e 0x31). Stepping off an edge, the game turns him round to face the wall
inline bool hang_proc(uint32_t proc) { return proc >= 0x2B && proc <= 0x31; }
// sidling along a wall, his back to it (daPyProc_WHIDE_READY_e 0x13, _WAIT_e 0x14, _MOVE_e 0x15, _PEEP_e 0x16): his
// head turns to look along the wall (and round its corner) while his model faces away from it
inline bool sidle_proc(uint32_t proc) { return proc >= 0x13 && proc <= 0x16; }
inline bool rig_follows_link_proc(uint32_t proc) {
    return (proc >= 0x38 && proc <= 0x3C) || hang_proc(proc) || sidle_proc(proc);
}
// the yaw the view's base turns to on a ladder, ledge or wall: his model's facing, but while sidling his animated
// head's (the horizontal direction from his head joint to the game's eye point, which the game places from his eye
// joint): only its yaw, so the head's animated pitch and roll never tilt the view
inline float rig_facing(uint32_t proc, float modelYaw, Vec3 headToEye) {
    const float flat = std::sqrt(headToEye.x * headToEye.x + headToEye.z * headToEye.z);
    if (!sidle_proc(proc) || !std::isfinite(flat) || flat < 2.0f) return modelYaw;
    return std::atan2(headToEye.x, headToEye.z);  // heading_dir(yaw) = {sin, 0, cos}
}
// the yaw of Link's own frame (his eyes' and skeleton's offsets are kept in it): current.angle.y, the way he moves,
// except where that is not the way his model faces. Sliding along a ledge the game sets it to the way he slides
// (procHangMove_init: shape_angle.y +- 0x4000) while his model keeps facing the wall (shape_angle.y): his frame turned
// a quarter round at every slide's start and end, and the view and his body swung sideways with it
// Z held with nothing targeted (daPyProc_ATN_MOVE_e 7, VR's walking backwards) too: current.angle.y is the way he steps
// (backwards) while his model keeps facing forward, and his frame turned round with it: his arms were stopped as facing
// away from the head and his shoulders held in the reversed frame (seen in the headset log)
// Locked on too (daPyProc_ATN_ACTOR_WAIT_e 8, _MOVE_e 9, the side steps 0x0A / 0x0B, the crouching guard 0x0C, the back
// jump 0x22 / 0x23): he faces the target while he moves any way, and backing away his frame turned round (175 degrees at
// once in a play test, 2026-11-04: the eye point held, then glided, a hitch); and standing (daPyProc_WAIT_e 4), when the
// way he last moved may not be the way he faces (as a lock let go while he backed away: 173 degrees at once)
inline bool frame_faces_model(uint32_t proc) {
    return proc == 4 || proc == 8 || proc == 9 || proc == 0x0A || proc == 0x0B || proc == 0x0C || proc == 0x22 ||
           proc == 0x23;
}
inline int16_t link_frame_angle(uint32_t proc, int16_t currentAngle, int16_t shapeAngle) {
    return hang_proc(proc) || sidle_proc(proc) || proc == 7 || frame_faces_model(proc) ? shapeAngle
                                                                                         : currentAngle;  // sidling: the same quarter turn
}
// the view's base (WorldMapping heading) that puts the body's facing on Link's: the inverse of link_facing
inline float rig_heading_for(float linkFacing, float bodyYaw) { return wrap_radians(linkFacing - bodyYaw); }
// Lock-on (docs/VR_BODY_FOLLOW.md, "Lock-on"): Link locked onto a person, an item or an enemy, the game turning him to
// face it (daPyProc_ATN_ACTOR_WAIT_e 8, daPyProc_ATN_ACTOR_MOVE_e 9: procAtnActorWait_init 023F0D2C and
// procAtnActorMove_init 023F0E18 pass them to commonProcInit). Z held with nothing to lock onto
// (daPyProc_ATN_MOVE_e 7) faces nothing and is not one. Kept through the moves he makes while locked on, which face the
// target too: side steps (0x0A, 0x0B: procSideStep_init, procSideStepLand_init), the back jump (0x22, 0x23) and the
// crouching guard (0x0C); not entered by them
inline bool lock_on_proc(uint32_t proc) { return proc == 8 || proc == 9; }
// (and through a parry's moves, 0x5D..0x64: the jump over the enemy, the roll round it, the leap and their cuts; the view
// was let go at the parry's start and faced away once he landed behind it: reported in the headset, 2026-11-04)
inline bool parry_move(uint32_t proc) { return proc >= 0x5D && proc <= 0x64; }
// The spin attack and the Hurricane Spin (daPyProc_CUT_TURN_e 0x55, daPyProc_CUT_ROLL_e 0x56, its end
// daPyProc_CUT_ROLL_END_e 0x57): the game turns Link round once (or again and again) on the spot. In first person the
// view's base turns with his model (asked for, 2026-11-04, after the spin attack in Majora's Mask VR: the camera turns
// round with the body); the charge (0x58) and walking charged (0x59) do not turn him
inline bool spin_attack_proc(uint32_t proc) { return proc == 0x55 || proc == 0x56 || proc == 0x57; }
// the view's turn in a spin (turn_towards' response, 1/s): one turn takes the game about half a second, and the eased
// turn trails a steady turn by its rate / response (about 10 degrees at 60/s; 20 at the first 30/s), so the view, the
// hands on the controllers and his body turn round together
constexpr float kSpinTurnResponse = 60.0f;
inline bool lock_on_keeps(uint32_t proc) {
    return lock_on_proc(proc) || proc == 0x0A || proc == 0x0B || proc == 0x0C || proc == 0x22 || proc == 0x23 ||
           parry_move(proc);
}
// The view's base during a lock-on: the tracking-space yaw the target is brought to (`anchor`: where the head looked
// when the lock began) and whether a lock-on holds. The base turns until Link's facing (towards the target, the
// game's) maps onto the anchor (rig_heading_for), so the target comes in front of where you looked and stays there
// as it moves or another is chosen; the head's later turns never move the anchor, so they look round freely and
// nothing feeds back. The headset's own pose is never changed
struct LockOnView {
    bool active = false;
    float anchor = 0;
    // one update: Link's action, the head's yaw (tracking space) and whether the game still has its lock-on target.
    // Returns whether the lock-on holds. Once on, it holds while the game keeps the target, whatever his action (a cut,
    // a parry's finishing cut: the view was let go after each and turned to the target again from where the head then
    // looked, the lock seeming lost after every parry and slash of a play test, 2026-11-05)
    bool update(uint32_t proc, float headYaw, bool gameLocked = false) {
        const bool on = active ? lock_on_keeps(proc) || gameLocked : lock_on_proc(proc);
        if (on && !active) anchor = headYaw;
        active = on;
        return on;
    }
    void reset() { active = false; }
};

// a yaw turned towards a target: the short way, frame-rate independent (the share 1 - exp(-response dt) of what is left
// each update, at least minRate rad/s), exactly on it once within the last step
inline float turn_towards(float yaw, float target, float dt, float response = 4.0f, float minRate = 1.0f) {
    const float diff = wrap_radians(target - yaw);
    if (!(dt > 0)) return yaw;
    const float eased = std::fabs(diff) * (1.0f - std::exp(-response * dt));
    const float step = std::min(std::fabs(diff), std::max(eased, minRate * dt));
    return wrap_radians(yaw + std::copysign(step, diff));
}

// The view's base turned to Link on a ladder or a ledge and during a lock-on (rig_heading_for): the pad read (30 a
// second) sets where it turns to, and each frame's camera turns towards it over the time between the frames' display
// times, as smooth turning does (docs/VR_CAMERA_AUDIT.md). Turned at the pad read, the view's yaw moved in 30 Hz steps
// (about 10 degrees a step at the start of a lock-on) while the frames came at 90
struct RigTurn {
    bool active = false;
    bool lockOn = false;  // (the body's yaw is kept as Link's heading against the base: vr_game.cpp)
    float target = 0;     // radians, the WorldMapping heading to reach
    float response = 4.0f;  // (turn_towards': faster in a parry's moves, which turn him round in half a second)
    float update(float heading, float dt) const {
        return active ? turn_towards(heading, target, dt, response, response * 0.25f) : heading;
    }
};

}  // namespace vr

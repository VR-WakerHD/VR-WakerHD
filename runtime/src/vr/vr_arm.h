// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Link's arm posed towards a tracked controller (docs/VR_FIXES.md, fix 7): the animation's upper arm, forearm
// and hand joints turned with the two-bone solver (vr_ik.h). Joint frames are J3D joint matrices (3x4, the
// columns of the 3x3 are the joint's axes, possibly scaled); everything here is in game units.
//
// The upper arm keeps its origin (the shoulder stays where the animation has it), each bone keeps its length,
// the elbow stays on the side the animation bends it to (no flips), and the hand turns with the forearm, then
// so that its pointing axis (the forearm's direction in the animation pose, i.e. along the knuckles) points
// where the controller points. A weight blends between the animation (0) and the solved pose (1), so tracking
// that starts or stops never snaps.
#pragma once
#include <cmath>
#include "vr_ik.h"
#include "vr_math.h"

namespace vr {

struct Joint {
    Quat rot;            // the joint's orientation (unscaled axes)
    Vec3 scale{1, 1, 1}; // lengths of its axes in the matrix
    Vec3 pos;
};
struct ArmJoints {
    Joint upper, lower, hand;
};

// a J3D 3x4 row-major matrix (m[r*4 + c]) <-> Joint
inline Joint joint_from_matrix(const float* m) {
    Joint j;
    const Vec3 cx{m[0], m[4], m[8]}, cy{m[1], m[5], m[9]}, cz{m[2], m[6], m[10]};
    j.scale = {length(cx), length(cy), length(cz)};
    j.rot = from_basis(normalize(cx, {1, 0, 0}), normalize(cy, {0, 1, 0}), normalize(cz, {0, 0, 1}));
    j.pos = {m[3], m[7], m[11]};
    return j;
}
inline void matrix_from_joint(const Joint& j, float* m) {
    const Vec3 c[3] = {rotate(j.rot, {j.scale.x, 0, 0}), rotate(j.rot, {0, j.scale.y, 0}), rotate(j.rot, {0, 0, j.scale.z})};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) m[r * 4 + k] = r == 0 ? c[k].x : r == 1 ? c[k].y : c[k].z;
    }
    m[3] = j.pos.x;
    m[7] = j.pos.y;
    m[11] = j.pos.z;
}

// the smallest rotation taking direction a onto direction b
inline Quat rotation_between(Vec3 a, Vec3 b) {
    a = normalize(a);
    b = normalize(b);
    const float d = dot(a, b);
    if (d > 0.999999f) return {};
    if (d < -0.999999f) {
        Vec3 axis = cross({1, 0, 0}, a);
        if (length(axis) < 1e-4f) axis = cross({0, 1, 0}, a);
        return axis_angle(axis, kPi);
    }
    const Vec3 c = cross(a, b);
    return normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

// the arm's bone lengths are plausible for an arm of the given unit scale (catches wrong joint numbers)
inline bool plausible_arm(const ArmJoints& a, float minBone, float maxBone) {
    const float u = length(a.lower.pos - a.upper.pos), l = length(a.hand.pos - a.lower.pos);
    return finite(a.upper.pos) && finite(a.lower.pos) && finite(a.hand.pos) && u >= minBone && u <= maxBone &&
           l >= minBone && l <= maxBone;
}

// The arm posed so the wrist goes to `wristTarget` (clamped to the arm's reach) and the hand's pointing axis
// along `pointTarget` (a direction; zero length: the hand only follows the forearm). weight 0..1.
inline ArmJoints pose_arm(const ArmJoints& anim, Vec3 wristTarget, Vec3 pointTarget, float weight) {
    if (!(weight > 0)) return anim;
    const Vec3 S = anim.upper.pos, E = anim.lower.pos, W = anim.hand.pos;
    const ArmChain chain{length(E - S), length(W - E)};
    if (!(chain.upper > 1e-3f) || !(chain.lower > 1e-3f) || !finite(wristTarget)) return anim;
    // the elbow on the side the animation bends it to
    Vec3 pole = E - (S + W) * 0.5f;
    if (length(pole) < 1e-3f) pole = Vec3{0, -1, 0};
    const ArmSolution s = solve_arm(S, wristTarget, pole, chain);
    ArmJoints out = anim;
    const Quat r1 = rotation_between(E - S, s.elbow - S);
    out.upper.rot = normalize(r1 * anim.upper.rot);
    const Quat r2 = rotation_between(rotate(r1, W - E), s.wrist - s.elbow);
    out.lower.rot = normalize(r2 * r1 * anim.lower.rot);
    out.lower.pos = s.elbow;
    Quat hand = normalize(r2 * r1 * anim.hand.rot);
    if (length(pointTarget) > 1e-4f) {
        // the hand's pointing axis in its own frame: the forearm's direction in the animation pose
        const Vec3 local = rotate(conjugate(anim.hand.rot), normalize(W - E));
        hand = normalize(rotation_between(rotate(hand, local), pointTarget) * hand);
    }
    out.hand.rot = hand;
    out.hand.pos = s.wrist;
    if (weight < 1.0f) {
        auto blend = [weight](const Joint& a, const Joint& b) {
            Joint j = b;
            j.rot = slerp(a.rot, b.rot, weight);
            j.pos = lerp(a.pos, b.pos, weight);
            return j;
        };
        out.upper = blend(anim.upper, out.upper);
        out.lower = blend(anim.lower, out.lower);
        out.hand = blend(anim.hand, out.hand);
    }
    return out;
}

// ---- natural arm motion (docs/VR_ARM_ANIMATION.md)
struct ArmStyle {
    Vec3 pole;                  // the natural elbow direction (body frame turned into the world: down, out, back)
    float poleBlend = 0.65f;    // 0 the animation's elbow side .. 1 the natural direction
    float shrug = 0.25f;        // share of the shoulder-to-target turn the clavicle follows on a full reach
    float shrugMax = 0.35f;     // radians the clavicle may turn at most (about 20 degrees)
    float wristLimit = 1.4f;    // radians the hand may bend away from the forearm (about 80 degrees)
    float twist = 0;            // radians the hand turns about its pointing axis (the controller's roll)
    float twistLimit = 1.75f;   // radians of twist at most either way (about 100 degrees)
    // the hand's own axis that is up when it is held level (learnt from the idle pose: level_hand_up). Set, the hand's
    // roll is the controller's roll from level and nothing else; zero, the twist is added to the animation's roll,
    // which turns with Link's body and his run cycle (the hands rolled over when he ran left or right: reported with
    // a video)
    Vec3 levelUp;
    // the hand held rigidly to the controller (an empty hand: vr_game.cpp): its orientation the controller's (`ctrl`,
    // game world) times a fixed offset (`ctrlToHand`, controller_to_hand), then turned back within wristLimit of the
    // forearm. No "level" in it: the roll from level (levelUp) had no level to measure from with the hand pointing
    // down, and a turn of the body rolled the hanging hands round (a player, 2026-11-10: "his hands still rotated when
    // he's moving around in a circle")
    bool rigid = false;
    Quat ctrl, ctrlToHand;
};

// the controller held level and pointing along `forward` (horizontal), in the game world: OpenXR's controller frame
// (-Z forward, +Y up) turned there
inline Quat controller_level(Vec3 forward) {
    const Vec3 f = normalize(Vec3{forward.x, 0, forward.z}, {0, 0, 1});
    const Vec3 z = f * -1.0f, y{0, 1, 0};
    return from_basis(cross(y, z), y, z);
}
// the fixed offset from the controller to the hand: the hand `level` (pointing along `forward`, level: level_hand's)
// when the controller is level along `forward`
inline Quat controller_to_hand(const Quat& level, Vec3 forward) {
    return normalize(conjugate(controller_level(forward)) * level);
}
// the hand turned the shortest way from `hand` to point its pointing axis (`pointLocal`, its own frame) along
// `forward` (horizontal): the hand held level and forward, as the idle pose holds it
inline Quat level_hand(const Quat& hand, Vec3 pointLocal, Vec3 forward) {
    forward = normalize(Vec3{forward.x, 0, forward.z}, {0, 0, 1});
    return normalize(rotation_between(rotate(hand, pointLocal), forward) * hand);
}

// the hand's level "up" axis in its own frame: the hand turned the shortest way from `hand` to point its pointing axis
// (`pointLocal`, its own frame) along `forward` (horizontal), and the world's up read in that frame
inline Vec3 level_hand_up(const Quat& hand, Vec3 pointLocal, Vec3 forward) {
    forward = normalize(Vec3{forward.x, 0, forward.z}, {0, 0, 1});
    const Quat level = normalize(rotation_between(rotate(hand, pointLocal), forward) * hand);
    return normalize(rotate(conjugate(level), {0, 1, 0}), {0, 1, 0});
}

// the roll of a hand about its pointing direction `f` (world), from level, given its up `u` (world): measured as
// controller_roll measures a controller's
inline float roll_from_level(Vec3 f, Vec3 u) {
    Vec3 level = Vec3{0, 1, 0} - f * f.y;
    if (length(level) < 1e-3f) return 0;
    level = normalize(level);
    u = u - f * dot(u, f);
    if (length(u) < 1e-4f) return 0;
    u = normalize(u);
    return std::atan2(dot(cross(level, u), f), dot(level, u));
}

// a direction turned towards `limitTo` until it is within `maxAngle` of it
inline Vec3 limit_angle(Vec3 dir, Vec3 limitTo, float maxAngle) {
    dir = normalize(dir);
    limitTo = normalize(limitTo);
    const float a = std::acos(std::clamp(dot(dir, limitTo), -1.0f, 1.0f));
    if (a <= maxAngle || a < 1e-5f) return dir;
    return rotate(slerp(Quat{}, rotation_between(limitTo, dir), maxAngle / a), limitTo);
}

// the controller's roll about its pointing direction, relative to holding it level (its up as close to the world's
// up as the pointing allows): + rolls clockwise as seen from behind the controller
inline float controller_roll(const Quat& orientation) {
    const Vec3 f = rotate(orientation, {0, 0, -1});
    const Vec3 u = rotate(orientation, {0, 1, 0});
    Vec3 level = Vec3{0, 1, 0} - f * f.y;
    if (length(level) < 1e-3f) return 0;  // pointing straight up or down: no level to roll from
    level = normalize(level);
    const Vec3 up = normalize(u - f * dot(u, f));
    return std::atan2(dot(cross(level, up), f), dot(level, up));
}

// The arm with its clavicle: the clavicle turns a little towards far or high targets (the shoulder follows the
// reach), the elbow keeps a natural side, the hand bends within the wrist's limit and twists with the controller.
// `clavicle` may be null (then only the arm). Returns the arm; *clavicleOut gets the clavicle when given.
// `hingeLocal` (optional state kept per arm between calls): the elbow's hinge axis in the upper arm's frame, learnt
// from the animation whenever it bends the elbow clearly, so a straight animated arm still has one.
inline ArmJoints pose_arm_natural(const ArmJoints& anim, const Joint* clavicle, Vec3 wristTarget, Vec3 pointTarget,
                                  const ArmStyle& style, float weight, Joint* clavicleOut = nullptr,
                                  Vec3* hingeLocal = nullptr) {
    if (clavicleOut && clavicle) *clavicleOut = *clavicle;
    if (!(weight > 0) || !finite(wristTarget)) return anim;
    const float armLength = length(anim.lower.pos - anim.upper.pos) + length(anim.hand.pos - anim.lower.pos);
    if (!(armLength > 1e-3f)) return anim;
    ArmJoints a = anim;
    // the shoulder: a share of the turn from the clavicle towards the target, growing with how extended the reach is
    if (clavicle) {
        const Vec3 C = clavicle->pos;
        const float reach = std::clamp(length(wristTarget - a.upper.pos) / armLength, 0.0f, 1.3f);
        const Quat full = rotation_between(a.upper.pos - C, wristTarget - C);
        // the clavicle joins in only on extended reaches (beyond 60 % of the arm's length), fully at 120 %
        float k = style.shrug * std::clamp((reach - 0.6f) / 0.6f, 0.0f, 1.0f);
        const float angle = angle_between(full, Quat{});
        if (angle * k > style.shrugMax) k = style.shrugMax / std::max(angle, 1e-5f);
        const Quat q = slerp(Quat{}, full, std::clamp(k, 0.0f, 1.0f));
        auto carry = [&](Joint& j) {
            j.rot = normalize(q * j.rot);
            j.pos = C + rotate(q, j.pos - C);
        };
        carry(a.upper);
        carry(a.lower);
        carry(a.hand);
        if (clavicleOut) {
            Joint c = *clavicle;
            c.rot = normalize(q * c.rot);
            *clavicleOut = weight < 1 ? Joint{slerp(clavicle->rot, c.rot, weight), c.scale, c.pos} : c;
        }
    }
    // the elbow: between the animation's side and the natural one
    Vec3 animPole = a.lower.pos - (a.upper.pos + a.hand.pos) * 0.5f;
    animPole = length(animPole) > 1e-3f ? normalize(animPole) : normalize(style.pole, {0, -1, 0});
    const Vec3 pole = normalize(lerp(animPole, normalize(style.pole, {0, -1, 0}), style.poleBlend), {0, -1, 0});
    ArmJoints out = a;
    {
        // the same steps as pose_arm, with the chosen pole
        const Vec3 S = a.upper.pos, E = a.lower.pos, W = a.hand.pos;
        const ArmSolution s = solve_arm(S, wristTarget, pole, ArmChain{length(E - S), length(W - E)});
        Quat r1 = rotation_between(E - S, s.elbow - S);
        // The elbow is a hinge. The shortest turn of the upper arm keeps the animation's twist, so when the new bend
        // plane differs from the animation's the forearm would turn about another axis than its hinge and the elbow
        // skin would twist (the elbow looking bent sideways). The upper arm also turns about its own axis so the
        // animation's hinge (the normal of its bend plane) lies along the new bend's normal; the forearm then turns
        // about the hinge only.
        Vec3 hinge = cross(E - S, W - E);
        const bool bent = length(hinge) > 0.26f * length(E - S) * length(W - E);  // bent more than 15 degrees
        if (hingeLocal) {
            if (bent) *hingeLocal = rotate(conjugate(a.upper.rot), normalize(hinge));
            hinge = length(*hingeLocal) > 0.5f ? rotate(a.upper.rot, *hingeLocal) : Vec3{};
        } else if (!bent) {
            hinge = {};
        }
        const Vec3 bendNormal = cross(s.elbow - S, s.wrist - s.elbow);
        if (length(hinge) > 1e-4f && length(bendNormal) > 1e-4f) {
            const Vec3 u = normalize(s.elbow - S);
            Vec3 h = rotate(r1, normalize(hinge));
            Vec3 n = normalize(bendNormal);
            h = h - u * dot(h, u);
            n = n - u * dot(n, u);
            if (length(h) > 1e-3f && length(n) > 1e-3f) {
                h = normalize(h);
                n = normalize(n);
                const float c = std::clamp(dot(h, n), -1.0f, 1.0f);
                const float angle = std::atan2(dot(cross(h, n), u), c);
                r1 = normalize(axis_angle(u, angle) * r1);
            }
        }
        out.upper.rot = normalize(r1 * a.upper.rot);
        const Quat r2 = rotation_between(rotate(r1, W - E), s.wrist - s.elbow);
        out.lower.rot = normalize(r2 * r1 * a.lower.rot);
        out.lower.pos = s.elbow;
        Quat hand = normalize(r2 * r1 * a.hand.rot);
        const Vec3 local = rotate(conjugate(a.hand.rot), normalize(W - E));  // the hand's pointing axis
        const Vec3 forearm = normalize(s.wrist - s.elbow);
        if (length(pointTarget) > 1e-4f) {
            const Vec3 aim = limit_angle(pointTarget, forearm, style.wristLimit);
            hand = normalize(rotation_between(rotate(hand, local), aim) * hand);
        }
        const float twist = std::clamp(style.twist, -style.twistLimit, style.twistLimit);
        const Vec3 f = normalize(rotate(hand, local));
        if (style.rigid) {
            // the controller's orientation through the fixed offset, then within the wrist's limit of the forearm
            Quat h = normalize(style.ctrl * style.ctrlToHand);
            const Vec3 p = normalize(rotate(h, local));
            const Vec3 within = limit_angle(p, forearm, style.wristLimit);
            if (finite(p) && finite(within)) h = normalize(rotation_between(p, within) * h);
            if (std::isfinite(h.x) && std::isfinite(h.w)) hand = h;
        } else if (length(style.levelUp) > 0.5f && finite(f)) {
            // the roll set from level (the up axis kept square to the pointing axis in the hand's frame)
            Vec3 up = style.levelUp - local * dot(style.levelUp, local);
            if (length(up) > 1e-3f) {
                const float now = roll_from_level(f, rotate(hand, normalize(up)));
                const float turn = std::remainder(twist - now, 2.0f * kPi);
                if (std::isfinite(turn) && turn != 0) hand = normalize(axis_angle(f, turn) * hand);
            }
        } else if (twist != 0) {
            hand = normalize(axis_angle(f, twist) * hand);
        }
        out.hand.rot = hand;
        out.hand.pos = s.wrist;
    }
    if (weight < 1.0f) {
        auto blend = [weight](const Joint& from, const Joint& to) {
            Joint j = to;
            j.rot = slerp(from.rot, to.rot, weight);
            j.pos = lerp(from.pos, to.pos, weight);
            return j;
        };
        out.upper = blend(anim.upper, out.upper);
        out.lower = blend(anim.lower, out.lower);
        out.hand = blend(anim.hand, out.hand);
    }
    return out;
}

// Detached arms (vr.detachedArms, docs/VR_ARM_ANIMATION.md): how far the arm is carried from its shoulder towards a
// wrist target beyond its reach, so the hand gets there with every bone its own length: what the reach lacks, along
// the line from the shoulder, at most `maxShift`; nothing for a target within reach
inline Vec3 detached_shift(Vec3 shoulder, Vec3 target, float reach, float maxShift) {
    const Vec3 d = target - shoulder;
    const float dist = length(d);
    if (!finite(d) || !(dist > reach) || !(dist > 1e-4f)) return {};
    return d * (std::min(dist - reach, std::max(maxShift, 0.0f)) / dist);
}
// a limit opened from its normal value to `detached` by the detached arms' weight (0 off .. 1 on)
inline float detached_limit(float normal, float detached, float weight) {
    return normal + (detached - normal) * std::clamp(weight, 0.0f, 1.0f);
}

// the natural elbow direction of Link's arm on `side` (-1 left, +1 right) for his body heading: down, out to that
// side and a little back
inline Vec3 natural_pole(float bodyHeading, int side) {
    const Vec3 right{-std::cos(bodyHeading), 0, std::sin(bodyHeading)};  // heading_right (vr_locomotion.h)
    const Vec3 back{-std::sin(bodyHeading), 0, -std::cos(bodyHeading)};
    return normalize(Vec3{0, -1, 0} + right * (0.45f * float(side)) + back * 0.3f);
}

// The world scale that makes the player's arm as long as Link's (game units per metre), within the settings' range
inline float world_scale_for_arms(float linkArm, float playerArmMetres) {
    return std::clamp(linkArm / playerArmMetres, 40.0f, 250.0f);
}

// The natural elbow direction for the player's body (forward in the game world) and where the hand is from the
// shoulder: relaxed, down, a little out to that side and back; with the hand raised the elbow swings out to the side
// (and less down); with the hand across the body further out; reaching forward it comes under the arm (less back)
inline Vec3 natural_pole_body(Vec3 bodyForward, int side, Vec3 handFromShoulder, float armLength) {
    const Vec3 f = normalize(Vec3{bodyForward.x, 0, bodyForward.z}, {0, 0, 1});
    const Vec3 out = Vec3{-f.z, 0, f.x} * float(side), back = f * -1.0f;
    const float s = armLength > 1e-3f ? 1.0f / armLength : 0.0f;
    const float up = std::clamp(handFromShoulder.y * s, 0.0f, 1.0f);
    const float across = std::clamp(-dot(handFromShoulder, out) * s, 0.0f, 1.0f);
    const float ahead = std::clamp(dot(handFromShoulder, f) * s, 0.0f, 1.0f);
    return normalize(Vec3{0, -1, 0} * (1.0f - 0.6f * up) + out * (0.45f + 0.7f * up + 0.4f * across) +
                         back * (0.3f * (1.0f - ahead)),
                     {0, -1, 0});
}

// Where Link's wrist goes: the controller's offset from the player's shoulder (game directions, game units),
// scaled by Link's arm length over the player's, from Link's shoulder
inline Vec3 scaled_wrist_target(Vec3 linkShoulder, Vec3 offsetFromPlayerShoulder, float linkArm, float playerArm) {
    const float k = playerArm > 1e-3f ? linkArm / playerArm : 1.0f;
    return linkShoulder + offsetFromPlayerShoulder * k;
}

// Where Link's wrist goes in first person: the controller's offset from the player's eyes, scaled the same way, from
// Link's eyes (the camera). A uniform scale about the eyes keeps the direction you see your hand in, so looking down
// at your hands shows his hands there (nearer, as his arms are shorter); the IK clamps what his arm cannot reach.
inline Vec3 eye_wrist_target(Vec3 linkEyes, Vec3 offsetFromPlayerEyes, float linkArm, float playerArm) {
    return scaled_wrist_target(linkEyes, offsetFromPlayerEyes, linkArm, playerArm);
}

// A wrist target his arm cannot reach, pulled back along the line from the eyes (not straight towards the shoulder,
// which would move the hand away from where you see the controller): the farthest point of that line within `reach`
// of the shoulder. When the line passes beyond his reach (a controller far away at eye level) the target is kept: his
// arm reaches towards it from the shoulder; bending it towards the line would raise his sleeve across the view
inline Vec3 keep_in_view(Vec3 eyes, Vec3 target, Vec3 shoulder, float reach) {
    if (length(target - shoulder) <= reach) return target;
    const float dist = length(target - eyes);
    if (dist < 1e-4f) return target;
    const Vec3 d = (target - eyes) * (1.0f / dist);
    const Vec3 e = eyes - shoulder;
    const float b = dot(e, d), c = dot(e, e) - reach * reach;
    const float disc = b * b - c;
    if (disc < 0) return target;
    return eyes + d * std::clamp(-b + std::sqrt(disc), 0.0f, dist);
}

}  // namespace vr

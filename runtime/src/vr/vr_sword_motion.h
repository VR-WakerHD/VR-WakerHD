// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The sword's motion, measured at the headset's frame rate (docs/VR_SWORD_COMBAT_PLAN.md, stage 1).
//
// SwingDetector (vr_swing.h) measures only the hand's linear speed, and the game's collision runs once per logic step
// (30 Hz). SwordMotion keeps the tracked sword's poses (OpenXR tracking space, metres) at the time each was predicted
// for, once per headset frame, and measures from them:
//   linear velocity   of the sword's root (the grip), from position differences
//   angular velocity  from the quaternion difference q1 * conj(q0) as axis * angle / dt (the shortest way round)
//   point velocity    of any point along the blade: v + w x r (r from the root to that point): a wrist flick moves
//                     the tip fast while the hand barely moves
// over a short history, so a hit reported a logic step later can be given the speed the blade had while it swept
// through the target. It only measures: nothing here changes what the game does. Header only, no OpenXR (vr_test).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "vr_math.h"
#include "vr_swing.h"

namespace vr {

// the sword's frame: its origin at the blade's root, the blade along -Z (blade_from_grip, vr_swing.h)
struct SwordSample {
    double time = 0;  // seconds (the pose's predicted display time)
    Pose pose;
    Vec3 velocity;         // m/s of the root, from the sample before (zero for the first after a gap)
    Vec3 angularVelocity;  // rad/s, tracking space
};

// angular velocity taking orientation a to b in dt seconds (tracking space axis, rad/s)
inline Vec3 angular_velocity(Quat a, Quat b, float dt) {
    if (!(dt > 0)) return {};
    Quat d = normalize(b * conjugate(a));
    if (d.w < 0) d = {-d.x, -d.y, -d.z, -d.w};  // (the shortest way round)
    const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(s > 1e-7f)) return {};
    const float angle = 2.0f * std::atan2(s, d.w);
    return Vec3{d.x, d.y, d.z} * (angle / (s * dt));
}

// impact of the blade at a point: its velocity, its speed, and how much of it is across the blade (a cut) rather
// than along it (a stab)
struct BladeImpact {
    Vec3 velocity;
    float speed = 0;
    float acrossShare = 0;  // 0 .. 1: the share of the speed across the blade
    float angularSpeed = 0;  // rad/s of the sword at that sample
    bool valid = false;
};

class SwordMotion {
public:
    static constexpr int kHistory = 32;           // (0.35 s at 90 Hz)
    static constexpr float kMaxGap = 0.1f;        // seconds: a longer step (a hitch, a pause) measures no velocity
    static constexpr float kMaxSpeed = 15.0f;     // m/s of the root: faster is a tracking glitch (kMaxPlausibleHandSpeed)
    static constexpr float kMaxAngular = 60.0f;   // rad/s (about 3400 degrees/s): faster is a tracking glitch

    void reset() { count_ = 0; }
    // a new pose (its time later than the last; the same time again changes nothing). false: not taken
    bool push(double time, const Pose& pose) {
        if (!finite(pose.position) || !std::isfinite(pose.orientation.w) || !std::isfinite(time)) return false;
        SwordSample s;
        s.time = time;
        s.pose = {normalize(pose.orientation), pose.position};
        if (count_ > 0) {
            const SwordSample& p = latest();
            const double dt = time - p.time;
            if (!(dt > 0)) return false;  // (not newer)
            if (dt <= kMaxGap) {
                const float d = float(dt);
                s.velocity = (s.pose.position - p.pose.position) * (1.0f / d);
                s.angularVelocity = angular_velocity(p.pose.orientation, s.pose.orientation, d);
                if (length(s.velocity) > kMaxSpeed || length(s.angularVelocity) > kMaxAngular) {
                    ++glitches_;
                    s.velocity = p.velocity;  // (a glitch: the motion carried on as it was, the pose taken)
                    s.angularVelocity = p.angularVelocity;
                }
            } else {
                ++gaps_;
            }
        }
        head_ = (head_ + 1) % kHistory;
        ring_[head_] = s;
        count_ = std::min(count_ + 1, kHistory);
        ++pushed_;
        return true;
    }
    int count() const { return count_; }
    const SwordSample& latest() const { return ring_[head_]; }
    // i = 0 the latest, 1 the one before ...
    const SwordSample& sample(int i) const { return ring_[(head_ - i + kHistory * 2) % kHistory]; }
    uint64_t pushed() const { return pushed_; }
    uint64_t glitches() const { return glitches_; }
    uint64_t gaps() const { return gaps_; }

    // the velocity of a point `along` metres from the root down the blade (sword frame (0, 0, -along)) at sample i
    static Vec3 point_velocity(const SwordSample& s, float along) {
        const Vec3 r = rotate(s.pose.orientation, Vec3{0, 0, -along});
        return s.velocity + cross(s.angularVelocity, r);
    }
    static BladeImpact impact_at(const SwordSample& s, float along) {
        BladeImpact b;
        b.velocity = point_velocity(s, along);
        b.speed = length(b.velocity);
        const Vec3 blade = rotate(s.pose.orientation, Vec3{0, 0, -1});
        if (b.speed > 1e-4f) {
            const float alongShare = std::fabs(dot(b.velocity, blade)) / b.speed;
            b.acrossShare = std::sqrt(std::max(0.0f, 1.0f - alongShare * alongShare));
        }
        b.angularSpeed = length(s.angularVelocity);
        b.valid = true;
        return b;
    }
    // the fastest the point `along` metres down the blade moved over the samples no older than `window` seconds
    // before the latest (a hit is reported after the sweep that made it)
    BladeImpact peak_impact(float along, float window) const {
        BladeImpact best;
        if (count_ == 0) return best;
        const double from = latest().time - double(window);
        for (int i = 0; i < count_; ++i) {
            const SwordSample& s = sample(i);
            if (s.time < from) break;
            const BladeImpact b = impact_at(s, along);
            if (!best.valid || b.speed > best.speed) best = b;
        }
        return best;
    }
    // the mean time between the samples in the history (seconds; 0 with fewer than two)
    float mean_step() const {
        if (count_ < 2) return 0;
        return float((latest().time - sample(count_ - 1).time) / double(count_ - 1));
    }

private:
    std::array<SwordSample, kHistory> ring_{};
    int head_ = 0, count_ = 0;
    uint64_t pushed_ = 0, glitches_ = 0, gaps_ = 0;
};

// where along the blade a hit point lies: 0 at the root, 1 at the tip (the point's projection, clamped)
inline float blade_fraction(Vec3 root, Vec3 tip, Vec3 p) {
    const Vec3 ab = tip - root;
    const float l2 = dot(ab, ab);
    if (!(l2 > 0) || !finite(p)) return 1.0f;
    return std::clamp(dot(p - root, ab) / l2, 0.0f, 1.0f);
}

// ---- stage 3 / 8: what an impact does (tunable: vr.swordStrongHitSpeed, vr.swordImpactHaptics, vr.hapticStrength)
// the attack power a physical-sword hit gets: the game's normal cut's (1), or a finishing cut's (2) at strongSpeed m/s
// or more at the blade's striking point (strongSpeed 0: always the normal cut's, the game's own rule)
inline int impact_attack_power(float speed, float strongSpeed) {
    return strongSpeed > 0 && speed >= strongSpeed ? 2 : 1;
}
// a hit's haptic pulse, by impact speed: from kHapticFloor of the strength at rest to all of it at kHapticFullSpeed,
// longer for harder hits; a deflection (the hit stopped by a shield or guard) is a sharp, full pulse
struct ImpactHaptic {
    float amplitude = 0, seconds = 0;
};
// (full at 15 m/s since the first headset test, 2026-11-20: its 9 hits struck at 2.3 to 22.7 m/s, all but one over the
// 6 m/s first chosen, so every hit had the same full pulse)
constexpr float kHapticFloor = 0.35f, kHapticFullSpeed = 15.0f;
inline ImpactHaptic impact_haptic(float speed, float strength, bool deflected) {
    ImpactHaptic h;
    if (!(strength > 0)) return h;
    if (deflected) return {std::min(1.0f, strength * 1.25f), 0.12f};
    const float t = std::isfinite(speed) ? std::clamp(speed / kHapticFullSpeed, 0.0f, 1.0f) : 0.0f;
    h.amplitude = strength * (kHapticFloor + (1.0f - kHapticFloor) * t);
    h.seconds = 0.04f + 0.06f * t;
    return h;
}

// ---- the physical sword armed by the blade's own speed (2026-11-20, headset: no hit in a whole test). SwingDetector
// starts a swing from the hand's speed (vr.swingStartSpeed, 1.6 m/s): in that test the hand peaked at 1.06 to 1.42 m/s
// while the blade's tip moved 6.6 to 15 m/s, a swing from the elbow and wrist, and the capsules were armed for about
// ten logic steps in the whole test. The blade's speed three quarters of the way down it (v + w x r) at threshold m/s
// or more arms the physical sword too (the game's attacks, their animations, still start from the hand's swing only).
// fast: arm this step; newSwing: a fresh swing for the hit ledger (each target once per swing), when it begins from
// unarmed without a hand swing. threshold 0: off (the hand's swings only)
struct BladeArm {
    bool fast = false, newSwing = false;
};
inline BladeArm blade_arm(float bladeSpeed, float threshold, bool handSwinging, bool armed) {
    BladeArm a;
    a.fast = threshold > 0 && std::isfinite(bladeSpeed) && bladeSpeed >= threshold;
    a.newSwing = a.fast && !handSwinging && !armed;
    return a;
}
constexpr float kArmAlong = 0.75f;
// (the player-only sword) a fast back-and-forth: the blade at arming speed going back the other way (its direction
// more than about 107 degrees from the current swing's start) is a new swing, so the same target can be hit again (the
// game's own hit-stun still decides whether it takes it); not within kMinSwingSteps of the last swing's start
constexpr float kReverseCos = -0.3f;
constexpr int kMinSwingSteps = 3;  // (0.1 s at 30 logic steps a second)
inline bool blade_reversed(Vec3 velocity, Vec3 swingDir, float speed, float threshold, int stepsSinceStart) {
    if (!(threshold > 0) || !(speed >= threshold) || stepsSinceStart < kMinSwingSteps) return false;
    const float l = length(swingDir), v = length(velocity);
    if (!(l > 0.5f) || !(v > 1e-3f)) return false;
    return dot(velocity * (1.0f / v), swingDir * (1.0f / l)) < kReverseCos;
}  // (of the blade's length from the root: where the arming speed is measured)

// ---- a real swing, not a light movement (2026-11-20, asked for: "I just move my hand very lightly ... Link will
// yell, we need to make only real swings count ... attacks that register should be ones that resemble a swing").
// The blade's speed alone (blade_arm) took a light flick of the wrist: 3 m/s at the measuring point is a turn of 270
// degrees/s for a moment. A real swing is fast AND long: the measuring point (kArmAlong of the blade) moves at
// `speed` or faster and carries on, never slower than `endSpeed`, until it has gone `distance` (0.30 m: about 27
// degrees of arc there; the first headset test's swings turned the blade 31 to 78 degrees). Then it is a swing (its
// hits armed, Link's voice) until the point is slower than endSpeed for endTime. Going back the other way it must
// build up again to count. Per headset frame (the motion history's samples)
// A thrust (asked for: "make sure thrust attacks still work") is a swing of its own kind: the blade pushed along its
// length, tip first, barely turning. Stabs are short and slower than slashes (play tests: 1.8 to 3.4 m/s of the
// hand, 0.1 to 0.3 m), so a thrust counts at thrustSpeed for thrustDistance while the motion stays along the blade
// (kThrustAlong of its speed toward the tip, kThrustKeep while it goes on); a light poke is too short.
struct BladeSwingConfig {
    float speed = 4.5f, distance = 0.30f, endSpeed = 2.0f, endTime = 0.06f;
    float thrustSpeed = 1.8f, thrustDistance = 0.15f, thrustEnd = 1.0f;
};
constexpr float kThrustAlong = 0.8f, kThrustKeep = 0.6f;
class BladeSwing {
public:
    enum class Phase : uint8_t { Idle, Building, Active };
    struct Event {
        bool confirmed = false, ended = false, thrust = false;
        float travel = 0, peak = 0;  // (on confirmed) metres and m/s of the measuring point so far
    };
    void reset() {
        phase_ = Phase::Idle;
        travel_ = peak_ = below_ = 0;
        dir_ = {};
        thrust_ = false;
    }
    // v: the measuring point's velocity (m/s); bladeDir: the blade's direction, root to tip; dt: seconds since the
    // last sample
    Event update(Vec3 v, Vec3 bladeDir, float dt, const BladeSwingConfig& k) {
        Event e;
        const float sp = length(v);
        if (!(dt > 0) || dt > 0.1f || !std::isfinite(sp)) {  // (a hitch, a gap: nothing measured, nothing kept)
            if (phase_ == Phase::Active) e.ended = true;
            reset();
            return e;
        }
        const float bl = length(bladeDir);
        const float along = sp > 1e-3f && bl > 1e-3f ? dot(v, bladeDir) / (sp * bl) : 0.0f;  // (+1: toward the tip)
        auto begin = [&](bool thrust) {
            phase_ = Phase::Building;
            thrust_ = thrust;
            travel_ = sp * dt;
            peak_ = sp;
            dir_ = v;
        };
        // (a cut moves the blade across itself: pulling it back toward the hilt, after a stab, is neither)
        const bool cutFast = sp >= k.speed && along > -kThrustAlong, thrustFast = sp >= k.thrustSpeed && along >= kThrustAlong;
        switch (phase_) {
        case Phase::Idle:
            if (cutFast && along < kThrustAlong) begin(false);
            else if (thrustFast) begin(true);
            break;
        case Phase::Building:
            if (thrust_) {
                if (cutFast && along < kThrustAlong) {  // (it became a slash)
                    begin(false);
                    break;
                }
                if (sp < k.thrustEnd || along < kThrustKeep) {
                    reset();
                    break;
                }
            } else {
                if (sp < k.endSpeed) {
                    reset();
                    break;
                }
                if (cutFast && reversed(v)) {  // (back the other way before it counted: from there again)
                    begin(false);
                    break;
                }
            }
            travel_ += sp * dt;
            peak_ = std::max(peak_, sp);
            if (travel_ >= (thrust_ ? k.thrustDistance : k.distance)) {
                phase_ = Phase::Active;
                below_ = 0;
                ++id_;
                e.confirmed = true;
                e.thrust = thrust_;
                e.travel = travel_;
                e.peak = peak_;
            }
            break;
        case Phase::Active:
            if (sp < (thrust_ ? k.thrustEnd : k.endSpeed)) {
                below_ += dt;
                if (below_ >= k.endTime) {
                    reset();
                    e.ended = true;
                }
            } else {
                below_ = 0;
                peak_ = std::max(peak_, sp);  // (the swing's fastest so far: Link's voice waits for a hard one)
                if (cutFast && reversed(v)) {  // a back-swing: a swing of its own once it too has gone far enough
                    e.ended = true;
                    begin(false);
                }
            }
            break;
        }
        return e;
    }
    bool active() const { return phase_ == Phase::Active; }
    bool thrust() const { return thrust_; }  // (the current or building swing is a thrust)
    Phase phase() const { return phase_; }
    uint32_t id() const { return id_; }  // counts the real swings
    float peak() const { return peak_; }  // m/s: the current swing's fastest so far (0 when idle)

private:
    bool reversed(Vec3 v) const {
        const float a = length(v), b = length(dir_);
        return a > 1e-3f && b > 1e-3f && dot(v, dir_) < kReverseCos * a * b;
    }
    Phase phase_ = Phase::Idle;
    float travel_ = 0, peak_ = 0, below_ = 0;
    Vec3 dir_;
    bool thrust_ = false;
    uint32_t id_ = 0;
};

// ---- stage 3c: the sweep along the swing's arc (docs/VR_SWORD_COMBAT_PLAN.md)
// The game checks the sword's 3 capsules once per logic step (30 Hz). sweep() (vr_swing.h) joins the blade before and
// the blade now by one straight chord of the tip, so a fast curved swing cuts inside its arc and misses what the arc
// passed through. The headset's poses in between (90 Hz: two per step) give the blades the swing passed through;
// sweep_path chooses the 3 capsules from those that best cover the swept area: the blade now always (capsule 0, as
// the game's own), and the pair of segments (the tip's path between any two of its places, a blade in between, or
// the old root-to-tip-before) whose farthest gap to the swept area is smallest. The old choice is one of the
// candidates and wins ties, so the coverage is never worse than before on the measured points.
constexpr int kMaxPath = 6;
struct BladePath {
    std::array<Blade, kMaxPath> blade{};  // game units: [0] the blade before (last step), [n - 1] the blade now
    int n = 0;
};
// the game-space blades between `before` (its pose sample at fromTime) and `now` (the latest sample): each in-between
// sample's motion relative to the latest, turned into the game's frame (toGame) and scaled (unitsPerMetre for the
// root's movement; the blade's turn keeps the game blade's own length), then Link's own movement over the step (and
// any mismatch at the start) shared out by time so the path meets `before` at its start
inline BladePath path_between(const SwordMotion& m, double fromTime, const Blade& before, const Blade& now, Quat toGame,
                              float unitsPerMetre) {
    BladePath p;
    p.blade[0] = before;
    p.n = 1;
    if (m.count() < 2 || !(m.latest().time > fromTime)) {
        p.blade[p.n++] = now;
        return p;
    }
    const SwordSample& last = m.latest();
    const double span = last.time - fromTime;
    const Quat toGameC = conjugate(toGame);
    const Vec3 nowDir = now.tip - now.root;
    auto predict = [&](const SwordSample& s) {
        const Quat d = normalize(s.pose.orientation * conjugate(last.pose.orientation));
        const Quat g = normalize(toGame * d * toGameC);
        Blade b;
        b.root = now.root + rotate(toGame, s.pose.position - last.pose.position) * unitsPerMetre;
        b.tip = b.root + rotate(g, nowDir);
        return b;
    };
    // the samples strictly between the two steps (newest first in the ring), and the one at the step before
    int idx[SwordMotion::kHistory];
    int k = 0, start = -1;
    for (int i = 1; i < m.count(); ++i) {
        const double t = m.sample(i).time;
        if (t <= fromTime + 1e-6) {
            start = i;
            break;
        }
        idx[k++] = i;
    }
    if (start < 0) {  // (the step before is no longer in the history: no path)
        p.blade[p.n++] = now;
        return p;
    }
    const Blade predBefore = predict(m.sample(start));
    const Vec3 eRoot = before.root - predBefore.root, eTip = before.tip - predBefore.tip;
    // oldest first, at most kMaxPath - 2 of them (evenly picked)
    const int want = std::min(k, kMaxPath - 2);
    for (int j = 0; j < want; ++j) {
        const int pick = want == k ? k - 1 - j : k - 1 - (j * (k - 1)) / std::max(want - 1, 1);
        const SwordSample& s = m.sample(idx[pick]);
        const float share = float(std::clamp((s.time - fromTime) / span, 0.0, 1.0));
        Blade b = predict(s);
        b.root = b.root + eRoot * (1.0f - share);
        b.tip = b.tip + eTip * (1.0f - share);
        if (finite(b.root) && finite(b.tip)) p.blade[p.n++] = b;
    }
    p.blade[p.n++] = now;
    return p;
}
// the measured points of the swept area: the blades along the path and halfway between each two, at a quarter, half,
// three quarters and the whole of their length
constexpr int kSweepPoints = (2 * kMaxPath - 1) * 4;
inline int sweep_points(const BladePath& p, Vec3* out) {
    int n = 0;
    auto add = [&](Vec3 root, Vec3 tip) {
        for (int f = 1; f <= 4; ++f) out[n++] = lerp(root, tip, 0.25f * float(f));
    };
    for (int i = 0; i < p.n; ++i) {
        add(p.blade[i].root, p.blade[i].tip);
        if (i + 1 < p.n)
            add(lerp(p.blade[i].root, p.blade[i + 1].root, 0.5f), lerp(p.blade[i].tip, p.blade[i + 1].tip, 0.5f));
    }
    return n;
}
// how a set of capsules (radius r) covers the measured points: `outside` the sum of how far they lie beyond the
// capsules (game units; 0: all inside), `gap` the farthest any lies from the capsules' lines
struct SweepCover {
    float outside = 0, gap = 0;
};
inline SweepCover sweep_cover(const BladePath& p, const Segment* segs, int ns, float r) {
    Vec3 pts[kSweepPoints];
    const int np = sweep_points(p, pts);
    SweepCover c;
    for (int k = 0; k < np; ++k) {
        float d = INFINITY;
        for (int i = 0; i < ns; ++i) d = std::min(d, distance_to_segment(pts[k], segs[i]));
        c.gap = std::max(c.gap, d);
        c.outside += std::max(0.0f, d - r);
    }
    return c;
}
struct SweepChoice {
    Sweep sweep;
    SweepCover before, after;  // the old sweep's cover and the chosen one's
    bool changed = false;      // capsules 1 and 2 not the old ones
};
// r: the capsules' radius (game units; the game's normal cut 20)
inline SweepChoice sweep_path(const BladePath& p, float r) {
    SweepChoice c;
    const Blade& before = p.blade[0];
    const Blade& now = p.blade[p.n - 1];
    c.sweep = sweep(before, now);
    if (p.n < 3) {  // (nothing in between: the old sweep, unchanged)
        c.before = c.after = sweep_cover(p, c.sweep.capsule, 3, r);
        return c;
    }
    constexpr int kMaxCand = 2 + kMaxPath * kMaxPath;
    Segment cand[kMaxCand];
    int nc = 0;
    cand[nc++] = c.sweep.capsule[1];  // (the old pair first: it wins ties)
    cand[nc++] = c.sweep.capsule[2];
    for (int i = 0; i < p.n; ++i)
        for (int j = i + 1; j < p.n; ++j)
            if (!(i == 0 && j == p.n - 1)) cand[nc++] = {p.blade[i].tip, p.blade[j].tip};  // the tip's path
    for (int i = 0; i + 1 < p.n; ++i) cand[nc++] = {p.blade[i].root, p.blade[i].tip};  // a blade on the way
    for (int i = 1; i + 1 < p.n; ++i) cand[nc++] = {now.root, p.blade[i].tip};
    // each point's distance to the blade now (capsule 0, always kept) and to each candidate, once
    Vec3 pts[kSweepPoints];
    const int np = sweep_points(p, pts);
    float d0[kSweepPoints], dc[kMaxCand][kSweepPoints];
    for (int k = 0; k < np; ++k) d0[k] = distance_to_segment(pts[k], c.sweep.capsule[0]);
    for (int i = 0; i < nc; ++i)
        for (int k = 0; k < np; ++k) dc[i][k] = distance_to_segment(pts[k], cand[i]);
    auto cover = [&](int i, int j) {
        SweepCover v;
        for (int k = 0; k < np; ++k) {
            const float d = std::min(d0[k], std::min(dc[i][k], dc[j][k]));
            v.gap = std::max(v.gap, d);
            v.outside += std::max(0.0f, d - r);
        }
        return v;
    };
    SweepCover best = cover(0, 1);
    c.before = best;
    int bi = 0, bj = 1;
    for (int i = 0; i < nc; ++i)
        for (int j = i + 1; j < nc; ++j) {
            if (i == 0 && j == 1) continue;
            const SweepCover v = cover(i, j);
            if (v.outside < best.outside - 1e-3f || (v.outside <= best.outside + 1e-3f && v.gap < best.gap - 1e-3f)) {
                best = v;
                bi = i;
                bj = j;
            }
        }
    c.after = best;
    c.changed = !(bi == 0 && bj == 1);
    c.sweep.capsule[1] = cand[bi];
    c.sweep.capsule[2] = cand[bj];
    return c;
}

}  // namespace vr

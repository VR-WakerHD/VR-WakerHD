// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Physical sword swings (docs/VR_ARCHITECTURE.md, "Sword"): detection from the tracked blade, the swept
// blade the game's sword collision takes, and the per-swing record of what was hit.
//
// Detection works in the OpenXR tracking space (metres), so locomotion, snap turns and the game's own
// movement never count as a swing: only the player's arm moves the blade there. A swing:
//   Idle      the tip moves slower than the start speed (or has not done so for swingMinTime yet)
//   Active    started; ends when the tip is slower than the end speed (hysteresis: end < start)
//   Recovery  swingCooldown seconds in which no new swing may start (the game's own recovery runs too)
// A frame time step outside (0, kMaxStep] (a hitch, a pause, a load) does not measure a velocity: the next
// update starts from the new pose, so a blade that "jumped" is not a swing.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "vr_config.h"
#include "vr_math.h"

namespace vr {

struct Blade {
    Vec3 root, tip;
};
// the blade of a sword held in a controller's grip pose: the grip offset and pitch from the settings, then
// bladeLength along the grip's forward (-Z) direction
inline Blade blade_from_grip(const Pose& grip, const Config& c) {
    const Quat tilt = axis_angle({1, 0, 0}, c.gripPitchDegrees * kPi / 180.0f);
    const Pose sword = compose(grip, Pose{tilt, Vec3{c.gripOffsetX, c.gripOffsetY, c.gripOffsetZ}});
    return {sword.position, transform(sword, Vec3{0, 0, -c.bladeLength})};
}

enum class SwingPhase : uint8_t { Idle, Active, Recovery };
// The direction of a swing in the body's frame, for the game's attack choice (vr_game.cpp)
enum class SwingKind : uint8_t { Horizontal, Vertical, Thrust, Rising };
struct SwingEvent {
    bool started = false, ended = false;
    // the swing has become a full one: the hand carried down, left to right, right to left, or forward along the blade
    // (a thrust; not an upward swing). Once per swing; the game's attack is started on it
    bool full = false;
    SwingKind fullKind = SwingKind::Horizontal;
    int fullSide = 0;
    // (diagnostics) on `ended`: how far the blade turned (radians) and the hand moved (metres, body frame: +X right,
    // +Y up, -Z forward) over the swing, and whether it was full
    float turned = 0;
    Vec3 moved;
    bool wasFull = false;
    SwingKind kind = SwingKind::Horizontal;
    int side = 0;          // horizontal swings: -1 towards the left, +1 towards the right
    float speed = 0;       // tip speed (m/s) this update
    // (diagnostics) on `full`: the hand's path so far (metres, body frame) and the blade's upward share (-1 to 1)
    Vec3 fullPath;
    float fullBladeUp = 0;
    float fullBladeTurnCos = 1;  // the cosine of the blade's turn since the swing started
};

constexpr float kMaxStep = 0.1f;  // seconds; longer steps are not measured
// m/s of the hand: faster is a tracking glitch, not a swing (TPVR's SwingDetector rejects hand speeds over 15 m/s
// after a captured one-frame spike of 31 m/s)
constexpr float kMaxPlausibleHandSpeed = 15.0f;

// classify a tip velocity v (tracking space) for a blade pointing along bladeDir, the body facing bodyYaw
inline SwingKind classify(Vec3 v, Vec3 bladeDir, float bodyYaw, int* side) {
    const Vec3 local = rotate(yaw_quat(-bodyYaw), v);  // +X right, +Y up, -Z forward of the body
    const Vec3 dir = normalize(v);
    // a thrust moves the tip along the blade (mostly forward)
    if (dot(dir, normalize(bladeDir)) > 0.75f && -local.z > 0) {
        if (side) *side = 0;
        return SwingKind::Thrust;
    }
    const float ax = std::fabs(local.x), ay = std::fabs(local.y);
    if (ax >= ay) {
        if (side) *side = local.x >= 0 ? 1 : -1;
        return SwingKind::Horizontal;
    }
    if (side) *side = 0;
    return local.y < 0 ? SwingKind::Vertical : SwingKind::Rising;
}

class SwingDetector {
public:
    explicit SwingDetector(const Config& c = {}) { configure(c); }
    void configure(const Config& c) {
        start_ = c.swingStartSpeed;
        end_ = std::min(c.swingEndSpeed, c.swingStartSpeed);
        minTime_ = c.swingMinTime;
        cooldown_ = c.swingCooldown;
        fullAngle_ = c.swingFullDegrees * kPi / 180.0f;
        acrossDistance_ = c.swingAcrossDistance;
        downDistance_ = c.swingDownDistance;
        thrustDistance_ = c.swingThrustDistance;
    }
    void reset() { have_ = false; phase_ = SwingPhase::Idle; above_ = 0; timer_ = 0; }
    // Locked on (asked for after Orca's training, 2026-11-04: "make it easier to register a vertical slice and thrust"):
    // the game then takes only three moves from a swing (the side cut, the vertical slash, the thrust), so its looser
    // rule for the chop: within about 60 degrees of straight down (down > kLockedDownShare x across) and kLockedDown
    // metres. (The thrust's rules are the same locked on or not: full(), below.) Set before each update; the settings'
    // distances stay as they are otherwise
    void set_locked_on(bool on) { lockedOn_ = on; }
    SwingPhase phase() const { return phase_; }
    float speed() const { return speed_; }
    float start_speed() const { return start_; }
    Vec3 tip_velocity() const { return velocity_; }

    // blade in tracking space (metres), dt in seconds, the body's yaw (tracking space)
    SwingEvent update(const Blade& b, float dt, float bodyYaw) {
        SwingEvent e;
        if (!finite(b.root) || !finite(b.tip)) { have_ = false; return e; }
        if (!have_ || !(dt > 0) || dt > kMaxStep) {
            prev_ = b;
            have_ = true;
            velocity_ = {};
            speed_ = 0;
            above_ = 0;
            return e;
        }
        // the hand's motion (the blade's root, at the grip), as TPVR measures it: turning the wrist swings the tip a
        // long way (a 35 degree turn carries it 0.5 m) without a swing of the arm, and started attacks (reported in the
        // headset)
        const Vec3 velocity = (b.root - prev_.root) * (1.0f / dt);
        prev_ = b;
        if (length(velocity) > kMaxPlausibleHandSpeed) {  // a tracking glitch: not measured, nothing changes
            velocity_ = {};
            speed_ = 0;
            return e;
        }
        velocity_ = velocity;
        speed_ = length(velocity_);
        e.speed = speed_;
        // a new swing: fast for swingMinTime (from rest, or back the other way)
        // (a thrust, the hand pushed forward along the blade, starts at kThrustStartShare of the start speed: a stab is
        // short and slower than a slash, and a play test's three or four thrusts never reached the start speed at
        // all, no swing logged; 2026-11-04)
        const Vec3 bladeDir = normalize(b.tip - b.root);
        const bool alongBlade = speed_ > 0 && dot(velocity_ * (1.0f / speed_), bladeDir) > 0.75f &&
                                -rotate(yaw_quat(-bodyYaw), velocity_).z > 0;
        // (and a push mostly forward in the body's frame, kStabForward of its speed, whatever the blade's angle, with
        // half the time: stabs at 1.8, 1.9 and 3.4 m/s, 0.86 to 0.93 forward, started nothing in a play test,
        // 2026-11-05, too short for swingMinTime or the blade not along them)
        const bool stab = speed_ > 0 && -rotate(yaw_quat(-bodyYaw), velocity_).z > kStabForward * speed_;
        // (locked on, a chop: the hand mostly down, kChopDown of its speed, starts at kChopStartShare of the start
        // speed with half the time, so a short chop is the vertical slash; asked for, 2026-11-05: "make vertical slice a
        // little easier to hit so the player doesn't need to make as long a swing"; chops at 1.35 to 1.76 m/s started
        // nothing in that play test). Locked on or not since 2026-11-06 (a PSVR2 player's chop at 1.58 m/s, 0.90 down,
        // started nothing just before he locked on: "make it easier to do the slice")
        const bool chop = speed_ > 0 && -velocity_.y > kChopDown * speed_;
        // (a stab still easier: at kStabStartShare and a quarter of the time, a pose or two; stabs at 1.14 and 1.57 m/s,
        // 0.89 to 0.95 forward, started nothing in a play test, 2026-11-05. A thrust is still only one once it has gone
        // kThrust forward with the blade along it, full() below)
        const float startSpeed = stab ? start_ * kStabStartShare : alongBlade ? start_ * kThrustStartShare
                               : chop ? start_ * kChopStartShare : start_;
        const float minTime = stab ? minTime_ * 0.25f : chop ? minTime_ * 0.5f : minTime_;
        // back the other way: the hand's direction over kReversedCos (about 120 degrees) from the swing's start. (Past
        // 90 degrees before 2026-11-05: a wide side swing about the shoulder that began forward, its start earlier
        // with the stab's rule, turned 112 degrees by its end and was taken for a second swing, vr_test)
        auto back_the_other_way = [&]() {
            const float a = length(velocity_), b = length(startVelocity_);
            return a > 0 && b > 0 && dot(velocity_, startVelocity_) < kReversedCos * a * b;
        };
        auto starting = [&](bool reversed) {
            if (speed_ >= startSpeed && (!reversed || back_the_other_way())) {
                above_ += dt;
                if (above_ + 1e-6f >= minTime) {
                    phase_ = SwingPhase::Active;
                    e.started = true;
                    e.kind = classify(velocity_, b.tip - b.root, bodyYaw, &e.side);
                    startVelocity_ = velocity_;
                    startBlade_ = b;
                    fullDone_ = fullWasAttack_ = false;
                    above_ = 0;
                    return true;
                }
            } else {
                above_ = 0;
            }
            return false;
        };
        // a full swing: the blade turned through swingFullDegrees since the swing started, its tip carried down or
        // across (in the body's frame); thrusts and upward swings never are
        // A committed swing, early enough that the game's attack (pressed now, hitting from two steps on for about
        // five) overlaps the rest of it: the hand carried swingFullDistance across or down in the body's frame (+X
        // right, +Y up, -Z forward), its direction that path. Not the blade's turn or the tip's path: a wrist turn
        // moves the tip without a swing (it started attacks, reported in the headset), and with the sword tilted up
        // the blade's turn did not show side swings. A path mostly forward or back (a thrust, a wind-up) or up never
        // attacks; until a swing is judged one that attacks it is judged again as it goes on.
        auto full = [&]() {
            if (fullDone_) return;
            const Quat toBody = yaw_quat(-bodyYaw);
            const Vec3 d0 = rotate(toBody, normalize(startBlade_.tip - startBlade_.root));
            const Vec3 d1 = rotate(toBody, normalize(b.tip - b.root));
            const Vec3 m = rotate(toBody, b.root - startBlade_.root);
            const float across = std::fabs(m.x), down = -m.y, plane = std::sqrt(m.x * m.x + m.y * m.y);
            // A thrust, locked on or not (asked for, 2026-11-04: "thrust should happen lock on or lock off"): a path more
            // forward than down and than across, judged before any slice and held for as long as it stays so, kThrust
            // metres forward with the blade within about 65 degrees of the path (a thrust that dipped on its way out was
            // taken for the vertical slash before it was far enough forward: the regression after 62b4fcf7)
            const float thrustDistance = std::min(thrustDistance_, kThrust);
            const float thrustAlong = 0.25f;  // (about 75 degrees; 0.4 before the stabs of 2026-11-05)
            const bool mostlyForward = -m.z > std::max(across, std::fabs(m.y));
            const bool mostlyBack = m.z > 0 && std::fabs(m.z) > 1.5f * plane;
            // a side slice needs swingAcrossDistance across (more across than up or down), a chop swingDownDistance
            // down (more down than across)
            const float downShare = lockedOn_ ? kLockedDownShare : 1.0f;
            const float downDistance = lockedOn_ ? std::min(downDistance_, kLockedDown) : downDistance_;
            if (mostlyForward || mostlyBack) {
                // mostly forward or back. A thrust: swingThrustDistance forward with the blade pointing along the path
                // (asked for in the headset: drawn in, then pushed out; the drawing in is a swing back, no attack). A
                // reach or a wind-up back never is
                const float forward = -m.z;
                // (not the start of a chop: a path heading down, more than kThrustDownShare of its forward, or the
                // blade raised over kThrustBladeUp; an overhead chop first moves forward with the blade up, and was
                // taken for a thrust in 43 of 55 chops of a play test, 2026-11-06: "vertical slice still misses". It
                // is judged again as it goes on, so the chop becomes the vertical slash once it is more down)
                const bool headingDown = down > kThrustDownShare * forward;
                const bool bladeRaised = d1.y > kThrustBladeUp;
                // (nor a chop's flat start: a thrust keeps the blade's direction, within kThrustTurnCos of where it
                // pointed when the swing started. A PSVR2 player's chops, 2026-11-06, "still can't hit vertical slice":
                // 18 of 22 locked on were taken for thrusts after a flat push 0.19 to 0.46 m forward with the blade 19 to
                // 37 degrees up, then came down 0.5 to 0.76 m, the blade turning 130 to 170 degrees over the swing; his
                // thrusts turned it 38 to 41 over the whole swing. Nor the hand raised on its way forward, more than
                // kThrustDownShare of its forward: the wind-up for the next chop, taken for a thrust there too)
                const float bladeTurnCos = dot(d0, d1);
                const bool bladeTurning = bladeTurnCos < kThrustTurnCos;
                const bool rising = -down > kThrustDownShare * forward;
                if (forward >= thrustDistance && dot(normalize(m), d1) > thrustAlong && !headingDown && !bladeRaised &&
                    !bladeTurning && !rising) {
                    e.fullPath = m;
                    e.fullBladeUp = d1.y;
                    e.fullBladeTurnCos = bladeTurnCos;
                    e.fullKind = SwingKind::Thrust;
                    e.fullSide = 0;
                    fullDone_ = fullWasAttack_ = true;
                    fullKind_ = e.fullKind;
                    e.full = true;
                    return;
                }
                // a chop forward and down: heading down (more than kThrustDownShare of its forward) with the blade
                // turning (over 30 degrees since the start) is the vertical slash, though more forward than down
                // (the PSVR2 player, 2026-11-06, "make it easier to do the slice": 7 chops 0.42 to 0.59 m down and 0.42
                // to 0.64 m forward, the blade turning 125 to 147 degrees, were neither a thrust nor a slash: no attack).
                // A thrust that dips, or a stab aimed down, keeps its blade's direction: never this
                if (forward > 0 && headingDown && down > downShare * across && down >= downDistance && bladeTurning) {
                    e.fullPath = m;
                    e.fullBladeUp = d1.y;
                    e.fullBladeTurnCos = bladeTurnCos;
                    e.fullKind = SwingKind::Vertical;
                    e.fullSide = 0;
                    fullDone_ = fullWasAttack_ = true;
                    fullKind_ = e.fullKind;
                    e.full = true;
                    return;
                }
                return;  // (not yet a thrust, a reach or a wind-up: judged again as it goes on)
            }
            if (down > 0 && down > downShare * across && down >= downDistance && down > -m.z) {
                // (first: locked on, a steep diagonal chop is the vertical slash, not a side cut)
                e.fullKind = SwingKind::Vertical;
                e.fullSide = 0;
            } else if (across >= std::fabs(m.y) && across >= acrossDistance_) {
                e.fullKind = SwingKind::Horizontal;
                e.fullSide = m.x >= 0 ? 1 : -1;
            } else {
                return;  // not far enough yet, or upward
            }
            fullDone_ = fullWasAttack_ = true;
            fullKind_ = e.fullKind;
            e.full = true;
            e.fullPath = m;
            e.fullBladeUp = d1.y;
            e.fullBladeTurnCos = dot(d0, d1);
        };
        switch (phase_) {
        case SwingPhase::Idle:
            if (starting(false)) full();
            break;
        case SwingPhase::Active:
            if (speed_ < end_) {
                full();  // its last frame counts too (a swing judged full only there was lost)
                phase_ = SwingPhase::Recovery;
                timer_ = cooldown_;
                e.ended = true;
                above_ = 0;
                e.turned = std::acos(std::clamp(dot(normalize(startBlade_.tip - startBlade_.root), normalize(b.tip - b.root)), -1.0f, 1.0f));
                e.moved = rotate(yaw_quat(-bodyYaw), b.root - startBlade_.root);
                e.wasFull = fullDone_ && fullWasAttack_;
            } else if (back_the_other_way() &&
                       // (not a chop's follow-through: once the swing is the vertical slash, the hand curving on down
                       // and back is the same swing; a new chop comes up first. Seen as two slashes from one chop once
                       // the forward-and-down chop is the slash early, 2026-11-06)
                       !(fullDone_ && fullKind_ == SwingKind::Vertical && -velocity_.y > 0.5f * speed_)) {
                // back the other way without slowing down (a fast flurry): this swing ends, the next starts once it
                // has been fast that way for swingMinTime (TPVR re-arms on a reversal: the speed of a fast back and
                // forth never drops, so its swings after the first were lost)
                if (starting(true)) e.ended = true;
                else full();
            } else {
                above_ = 0;
                full();
            }
            break;
        case SwingPhase::Recovery:
            timer_ -= dt;
            if (starting(true)) break;  // back the other way: a new swing within the cooldown
            if (timer_ <= 0) phase_ = SwingPhase::Idle;
            break;
        }
        return e;
    }

private:
    static constexpr float kLockedDownShare = 0.6f, kLockedDown = 0.06f,  // (0.10 m until 2026-11-05: shorter chops)
                            kThrust = 0.08f, kThrustStartShare = 0.6f, kStabForward = 0.8f,
                           kChopDown = 0.75f, kChopStartShare = 0.75f, kStabStartShare = 0.45f,
                           kReversedCos = -0.5f,
                           kThrustDownShare = 0.5f,  // (a thrust's path within about 27 degrees below level)
                           kThrustBladeUp = 0.6f,    // (and its blade under about 37 degrees up)
                           kThrustTurnCos = 0.866f;  // (and turned under 30 degrees since the swing started)
    bool lockedOn_ = false;
    float start_ = 2.6f, end_ = 1.2f, minTime_ = 0.05f, cooldown_ = 0.25f;
    bool have_ = false;
    Blade prev_;
    Vec3 velocity_, startVelocity_;  // the tip's velocity now and when the current / last swing started
    Blade startBlade_;               // the blade where the current swing started
    bool fullDone_ = false;          // the current swing was judged full or not (once)
    bool fullWasAttack_ = false;
    SwingKind fullKind_ = SwingKind::Horizontal;  // the current / last swing's full kind, once full
    float fullAngle_ = 0.61f, acrossDistance_ = 0.12f, downDistance_ = 0.15f, thrustDistance_ = 0.15f;
    float speed_ = 0, above_ = 0, timer_ = 0;
    SwingPhase phase_ = SwingPhase::Idle;
};

// ---- swept blade for the game's collision
// Link's sword attack collides with three capsules (daPy_lk_c::mAtCps[3], set by setSwordAtCollision):
//   [0] the blade now (root -> tip), [1] the path of the tip (previous tip -> tip), [2] root -> previous tip.
// Together they cover the area the blade swept between two frames, so a fast swing cannot pass through a
// target between frames. The same arrangement is used with the tracked blade (game units).
struct Segment {
    Vec3 a, b;
};
struct Sweep {
    Segment capsule[3];
    Vec3 atVec;  // the attack direction the game gives its hit (tip now - tip before)
};
inline Sweep sweep(const Blade& before, const Blade& now) {
    Sweep s;
    s.capsule[0] = {now.root, now.tip};
    s.capsule[1] = {before.tip, now.tip};
    s.capsule[2] = {now.root, before.tip};
    s.atVec = now.tip - before.tip;
    return s;
}
// distance from a point to a segment (tests, debug overlay)
inline float distance_to_segment(Vec3 p, const Segment& s) {
    const Vec3 ab = s.b - s.a;
    const float l2 = dot(ab, ab);
    const float t = l2 > 0 ? std::clamp(dot(p - s.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return length(p - (s.a + ab * t));
}
// whether a sphere (centre, radius) touches any capsule of the sweep (capsule radius r)
inline bool sweep_touches(const Sweep& s, float r, Vec3 centre, float radius) {
    for (const auto& c : s.capsule)
        if (distance_to_segment(centre, c) <= r + radius) return true;
    return false;
}

// ---- the physical sword (TPVR's g_physicalSwordSwingActive): the sword's attack collision armed along the tracked
// blade while the hand swings, without the game's attack (its animation and sound). Armed from a swing's start (the
// detector's Active phase: swingStartSpeed for swingMinTime) and for kPhysicalHold after it ends (TPVR holds 0.1 s), only
// while `allowed` (the sword held for an attack, first person, gameplay) and no attack of the game's own runs (its
// capsules follow the tracked blade already). Per logic step.
constexpr float kPhysicalHold = 0.1f;
struct PhysicalStep {
    bool armed = false;
    bool began = false;  // armed this step and not the last, or a new swing while armed: the attack's hits reset
};
class PhysicalSword {
public:
    // swingId: the hit ledger's swing count (a new swing while armed starts its hits afresh)
    PhysicalStep update(bool allowed, bool gameAttack, bool swinging, uint32_t swingId, float dt) {
        PhysicalStep s;
        if (swinging) hold_ = kPhysicalHold;
        else hold_ -= dt;
        const bool want = allowed && !gameAttack && (swinging || hold_ > 0);
        if (want) {
            s.armed = true;
            s.began = !armed_ || swingId != swingId_;
            swingId_ = swingId;
        }
        if (!allowed || gameAttack) hold_ = 0;
        armed_ = want;
        return s;
    }
    bool armed() const { return armed_; }
    void reset() { armed_ = false; hold_ = 0; }

private:
    bool armed_ = false;
    float hold_ = 0;
    uint32_t swingId_ = 0;
};

// ---- what one swing hit: each target counts once per swing (haptics, debug overlay)
class HitLedger {
public:
    void begin_swing() { ++swing_; hits_.clear(); }
    uint32_t swing() const { return swing_; }
    // true the first time this target is reported in the current swing
    bool record(uint32_t target) {
        if (!target || std::find(hits_.begin(), hits_.end(), target) != hits_.end()) return false;
        hits_.push_back(target);
        return true;
    }
    size_t count() const { return hits_.size(); }

private:
    uint32_t swing_ = 0;
    std::vector<uint32_t> hits_;
};

// A quick swing of a hand up or down (the Deku Leaf, docs/VR_HELD_ITEMS.md "The Deku Leaf"): its point (the tip of
// the sword it would hold, tracking space, metres, +Y up) moving up or down faster than vr.swingStartSpeed, more up or
// down than across, until it has gone vr.swingDownDistance that way. Once per swing: the next needs the point slowed
// below vr.swingEndSpeed first, and kCooldown since the last. The sword's SwingDetector judges only swings down and across.
// At the sword's own thresholds the leaf went off too readily and too often (reported in the headset): a flap needs at
// least kMinStart and kMinDistance, and kCooldown between two
class FlapDetector {
public:
    static constexpr float kMinStart = 2.8f;     // m/s at least, whatever vr.swingStartSpeed
    static constexpr float kMinDistance = 0.4f;  // metres at least, whatever vr.swingDownDistance
    static constexpr float kCooldown = 1.0f;     // seconds from one flap to the next
    void configure(const Config& c) {
        start_ = std::max(c.swingStartSpeed, kMinStart);
        end_ = std::min(c.swingEndSpeed, c.swingStartSpeed);
        distance_ = std::max(c.swingDownDistance, kMinDistance);
    }
    void reset() {
        have_ = moving_ = done_ = false;
        vel_ = {};
        since_ = kCooldown;
    }
    // +1 a swing up, -1 down, 0 none (this update)
    int update(Vec3 p, float dt) {
        const bool ok = std::isfinite(p.x + p.y + p.z);
        if (!have_ || !(dt > 0) || !ok) {
            prev_ = p;
            have_ = ok;
            return 0;
        }
        vel_ = vel_ * 0.5f + (p - prev_) * (0.5f / dt);  // (smoothed over a couple of frames)
        prev_ = p;
        since_ += dt;
        const float vy = std::fabs(vel_.y), vh = std::sqrt(vel_.x * vel_.x + vel_.z * vel_.z);
        if (!moving_) {
            if (vy > start_ && vy > vh) {
                moving_ = true;
                done_ = false;
                from_ = p;
            }
            return 0;
        }
        if (length(vel_) < end_) {
            moving_ = false;
            return 0;
        }
        if (done_) return 0;
        const Vec3 m = p - from_;
        const float across = std::sqrt(m.x * m.x + m.z * m.z);
        if (std::fabs(m.y) >= distance_ && std::fabs(m.y) > across && since_ >= kCooldown) {
            done_ = true;
            since_ = 0;
            return m.y > 0 ? 1 : -1;
        }
        return 0;
    }

private:
    float start_ = kMinStart, end_ = 0.8f, distance_ = kMinDistance;
    float since_ = kCooldown;  // seconds since the last flap
    bool have_ = false, moving_ = false, done_ = false;
    Vec3 prev_, from_, vel_;
};

}  // namespace vr

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Conducting with the Wind Waker in VR (docs/VR_WIND_WAKER.md).
//
// While Link conducts (the game's metronome is on: play +0x5BD1, set by daPy_lk_c::procTactWait_init and cleared at
// the song's end, a cancel or a talk), the game reads its two sticks as it does on the GamePad: the left stick held
// up, down, left or right is the note of each beat, and the right stick (the C-stick) held left or right sets the
// beat (3/4, 6/4; neutral 4/4; up and down move Link's left arm). It reads where they are held, beat by beat, not presses. So each side here is one
// held direction, the same kind of value the stick gives, not a stream of note events:
//   - channel 0, the notes: the left stick, or the wand's hand (BatonInput::noteHand) held out that way from where it settled when conducting
//     began (out of a neutral circle round that place, in one of four quadrants: BatonZone);
//   - channel 1, the beat: the right stick, or the other hand, up, down, left or right (left / right set the beat).
// A channel's stick, pushed out of its dead zone, wins over its hand; a neutral stick leaves the hand's direction (a
// neutral source never cancels the other). Each channel then gives the game a full push along its direction, so the
// game's own beat timing, notes, songs and their success are decided by its code alone.
//
// Pure (vr_test.cpp test_baton); vr_game.cpp reads the metronome, the sticks and the controllers and writes the result
// to the game's sticks.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "vr_math.h"

namespace vr {

enum class BatonDir : uint8_t { None, Up, Down, Left, Right };
inline const char* baton_dir_name(BatonDir d) {
    switch (d) {
    case BatonDir::None: return "-";
    case BatonDir::Up: return "up";
    case BatonDir::Down: return "down";
    case BatonDir::Left: return "left";
    case BatonDir::Right: return "right";
    }
    return "?";
}

// One source's held direction from a 2D value (x right, y up), with hysteresis:
//   - from neutral, a direction once the value is `enter` out along one axis: the larger axis decides, and an exact
//     diagonal goes to left / right;
//   - held until it is back within `exit` along its own axis (exit below enter: no flicker at the edge);
//   - while held, it turns to the other axis's direction only when that one is `enter` out and a quarter larger
//     (no flicker along a diagonal).
struct BatonFilter {
    BatonDir dir = BatonDir::None;
    BatonDir update(float x, float y, float enter, float exit) {
        if (!std::isfinite(x) || !std::isfinite(y)) return dir = BatonDir::None;
        const float ax = std::fabs(x), ay = std::fabs(y);
        auto horizontal = [&] { return x > 0 ? BatonDir::Right : BatonDir::Left; };
        auto vertical = [&] { return y > 0 ? BatonDir::Up : BatonDir::Down; };
        if (dir != BatonDir::None) {
            const bool alongX = dir == BatonDir::Left || dir == BatonDir::Right;
            const float own = alongX ? (dir == BatonDir::Right ? x : -x) : (dir == BatonDir::Up ? y : -y);
            const float other = alongX ? ay : ax;
            if (other >= enter && other > 1.25f * std::max(own, 0.0f)) return dir = alongX ? vertical() : horizontal();
            if (own < exit) dir = BatonDir::None;  // back to neutral (or pushed the other way: a new direction below)
            else return dir;
        }
        if (std::max(ax, ay) < enter) return dir = BatonDir::None;
        return dir = ax >= ay ? horizontal() : vertical();
    }
};

// A hand's direction from its offset from its centre (x right, y up, metres): the circle of `radius` round the centre
// is a neutral zone and gives nothing; outside it, the quadrant the hand is in gives its direction (up, down, left or
// right, the diagonals the borders between them). So a hand held out any way gives one as soon as it leaves the
// circle, however its reach splits between the two axes: down and to the right had given nothing while neither axis
// alone reached the radius (reported in the headset: down and right did not register). With hysteresis:
//   - held until the hand is back within kExitShare of the radius (no flicker at the circle's edge);
//   - it turns to the other axis's quadrant only once that axis is kTurn times the held one's, about 51 degrees from
//     the held axis (no flicker along a diagonal); straight across to the opposite side it turns at once;
//   - an exact diagonal from neutral goes to left / right.
// `shape`: how far out each direction is, as shares of the radius: left, right, and up / down (the zone is then a
// circle stretched or squashed on each side; 1, 1, 1: a circle). The quadrants are taken in the stretched space, so a
// nearer side also has a wider quadrant
struct BatonShape { float left = 1, right = 1, vertical = 1; };
struct BatonZone {
    // (0.9 since 2026-11-10, calibrated in the headset: "still too hard for middle note"; 0.8 before)
    static constexpr float kExitShare = 0.9f;
    // (1.15 since 2026-11-10: a clear move to the other axis turns sooner, about 49 degrees from the held one; 1.25
    // held a sweep from left to down on left in the headset)
    static constexpr float kTurn = 1.15f;
    BatonDir dir = BatonDir::None;
    BatonDir update(float x, float y, float radius, BatonShape shape = {}) {
        if (!std::isfinite(x) || !std::isfinite(y)) return dir = BatonDir::None;
        x /= std::max(0.05f, x < 0 ? shape.left : shape.right);
        y /= std::max(0.05f, shape.vertical);
        const float out = std::sqrt(x * x + y * y);
        if (out < radius * (dir == BatonDir::None ? 1.0f : kExitShare)) return dir = BatonDir::None;
        const float ax = std::fabs(x), ay = std::fabs(y);
        const BatonDir horizontal = x > 0 ? BatonDir::Right : BatonDir::Left;
        const BatonDir vertical = y > 0 ? BatonDir::Up : BatonDir::Down;
        if (dir != BatonDir::None) {
            const bool alongX = dir == BatonDir::Left || dir == BatonDir::Right;
            if (alongX) return dir = ay > kTurn * ax ? vertical : horizontal;
            return dir = ax > kTurn * ay ? horizontal : vertical;
        }
        return dir = ax >= ay ? horizontal : vertical;
    }
};

// One hand's held direction: where it is from where it settled when conducting began (its centre), in a frame turned
// to the head's heading at that moment (x right, y up, from tracking space), so looking round while conducting does
// not change what a hand means. The centre is taken once the hand has been still (slower than kStillSpeed) for
// kSettle: conducting begins as the hand comes forward from the item wheel, and a centre taken then left the hand
// "down" once it came to rest (seen in the headset's log). Until then it gives nothing. A hand that loses tracking
// gives nothing; when it is tracked again, or after a recentre or a jump of its pose (more than kJump between reads:
// a tracking glitch), it settles again where it is.
class BatonHand {
public:
    // (0.5 since 2026-11-10, Wind Waker Test 5: a full arm's sweep at 4-5 m/s over a slow frame went past 0.25 and
    // the hand settled again, out at the far end of the sweep)
    static constexpr float kJump = 0.5f;        // metres between two reads
    static constexpr float kStillSpeed = 0.15f; // m/s: the hand counts as still
    static constexpr float kSettle = 0.3f;      // s still before its centre is taken
    static constexpr float kFollow = 1.5f;      // s: the centre's ease towards the resting hand (no direction held)
    // s still in a direction before that place is taken as the middle (a player's log, 2026-11-10: the middle was taken
    // with the wand raised, the hand then rested 18-30 cm below it, so "down" was held for good and nothing else
    // registered, worse with every try; the ease above runs only with no direction held)
    // (4 s since 2026-11-10, Wind Waker Test 5: big arm moves held up or out, and a note held two beats (Earth God's
    // Lyric's Down, Down), passed 1.5 s and took the far end as the middle: every note after read wrong. 4 s is past
    // any note the songs hold)
    static constexpr float kRestTime = 4.0f;
    // A flick (asked for, 2026-11-10: "if player swing out it's right, swing in towards their body it's left, and up and
    // down swings are up and down"): the hand moving faster than kFlickSpeed (in its frame, across and up) gives the
    // direction it moves in at once, held kFlickHold, before the hand has gone the zone's distance. Not the stroke back:
    // a move towards the centre from more than kFlickReturn out is the hand coming back to rest and gives nothing. One
    // a stroke: the next flick only once the hand has slowed below kFlickRearm. The motion smoothed over kFlickSmooth
    static constexpr float kFlickSpeed = 1.0f;    // m/s
    static constexpr float kFlickRearm = 0.5f;    // m/s
    static constexpr float kFlickHold = 0.3f;     // s at least (0.3 since the headset run, 2026-11-10: a quick move out and back gave its note for 0.15 s, and the game, taking the note held at its beat, missed most)
    static constexpr float kFlickReturn = 0.04f;  // m
    static constexpr float kFlickSmooth = 0.03f;  // s
    // A stroke's intent (asked for, 2026-11-10: "if the player's hand is travelling to the right position but doesn't
    // make it there but was almost there, register it as a right note; if the hand is still around the middle, the
    // middle"): a move out from the middle faster than kStrokeSpeed is a stroke; once it has gone kIntentShare of the
    // zone's distance from where it began, heading clearly one way, it gives that way (as a flick does, held the same),
    // though the hand stops short of the zone. A hand still or drifting near the middle gives the middle
    static constexpr float kStrokeSpeed = 0.25f;  // m/s
    static constexpr float kIntentShare = 0.5f;   // of the zone's distance (5 cm at the wand's 10.2)
    // Long motions (Wind Waker Test 5, 2026-11-10: "It works good when you make small little pointer movements ... But
    // if you raise your arm up all the way, down, left right in large hand/arm movements. It doesn't work", then "we
    // need to register longer motions"). The wand's zone grows with the size of the moves being made: kScaleShare of
    // the farthest the hand has gone lately (fading over kPeakFade), from the set zone up to kMaxScale times it. Small
    // pointer moves keep the 10.2 cm zone; whole-arm moves of 50-70 cm give a zone of about 20 cm, so the arm brought
    // back roughly to the middle is the middle. And a long stroke (kLongShare of the zone from where it began, heading
    // clearly one way) gives where it has gone overall, over a flick's first direction: a whole-arm raise that sets off
    // sideways is up
    static constexpr float kScaleShare = 0.3f;  // (0.3, the cap 1.6, the long stroke 1.0 since 2026-11-10, asked for in the headset: "all whole arm conducting allow for shorter moves")
    static constexpr float kMaxScale = 1.6f;   // (16.3 cm at the wand's 10.2)
    static constexpr float kPeakFade = 8.0f;   // s (a pause between big moves keeps the zone; small flicks work by their own measure meanwhile)
    static constexpr float kLongShare = 1.0f;
    // the wand's strokes (2026-11-10, asked for in the headset: "8cm of continuous arm movement in any direction will
    // register that direction")
    static constexpr float kStrokeLen = 0.08f;  // m
    // (0.05 and 0.03, and a stop only after kStopTime slow, since 2026-11-10, asked for in the headset: "how fast does
    // the player need to move to hit notes? can we make it slower?" It had been 0.15 to begin and 0.08 to end, ended
    // by one slow read: 8 cm in about half a second at the slowest, and a slowing mid-move began it again)
    static constexpr float kMoveSpeed = 0.05f;  // m/s: a stroke begins (8 cm in 1.6 s at the slowest)
    static constexpr float kStopSpeed = 0.03f;  // m/s: slower than this ...
    static constexpr float kStopTime = 0.15f;   // s: ... this long, and it ends (a brief slowing mid-move does not)
    // shape: how far out each side of its zone is (the left hand's: Baton::kNoteShape); flicks: a flick gives its
    // direction too (the wand's hand)
    BatonDir update(bool tracked, Vec3 pos, float headingYaw, float reach, bool recentre, float dt, bool horizontalOnly = false,
                    BatonShape shape = {}, bool flicks = false) {
        if (!tracked || !finite(pos)) {
            reset();
            return BatonDir::None;
        }
        if (!haveLast_ || recentre || length(pos - last_) > kJump) {  // (again) from here: not settled yet
            reset();
            haveLast_ = true;
            last_ = pos;
            return BatonDir::None;
        }
        const Vec3 delta = pos - last_;
        const float moved = length(delta);
        last_ = pos;
        if (!centred_) {
            if (dt > 1e-4f) still_ = moved / dt < kStillSpeed ? still_ + dt : 0.0f;  // (reads within one frame: no time)
            if (still_ < kSettle) return BatonDir::None;
            centred_ = true;
            centre_ = pos;
            frame_ = conjugate(yaw_quat(headingYaw));
            return BatonDir::None;
        }
        const Vec3 local = rotate(frame_, pos - centre_);
        local_ = local;
        // (the set zone: a wrist flick's or short stroke's measure, whatever size the zone has grown to; asked for,
        // 2026-11-10: "both big and small should register, big if player is swinging their arms or small if they are
        // flicking their wrists")
        const float base = reach;
        if (flicks) {  // (the wand's hand: its zone follows the size of the moves)
            const float outNow = std::sqrt(local.x * local.x + (horizontalOnly ? 0.0f : local.y * local.y));
            peak_ = std::max(peak_ * std::exp(-std::max(dt, 0.0f) / kPeakFade), outNow);
            reach = std::clamp(kScaleShare * peak_, reach, kMaxScale * reach);
        }
        radius_ = reach;
        // the beat's hand: where it is (the zone). The wand's: its strokes (below)
        BatonDir d = flicks ? BatonDir::None : zone_.update(local.x, horizontalOnly ? 0.0f : local.y, reach, shape);
        if (flicks && dt > 1e-4f) {
            // Strokes (asked for in the headset, 2026-11-10: "8cm of continuous arm movement in any direction will
            // register that direction", for a wrist's flick and a whole arm's move alike). A stroke: the hand moving
            // (faster than kMoveSpeed) until it stops (slower than kStopSpeed for kStopTime) or turns back; once it has gone kStrokeLen
            // from where it began, its way (the larger of across and up) is the note, held until another stroke.
            // From a held note: a stroke straight back the way it came is the hand coming back; it gives the middle once
            // the hand stops or is within the middle's zone, or the opposite note if it goes on past the middle's zone
            // (up through the middle to down). A stroke back and to one side gives that side (up, then over to the left:
            // left). A note is held kFlickHold at least, so the game's beat can find a quick one
            const Vec3 v = rotate(frame_, delta) * (1.0f / dt);
            vel_ = vel_ + (Vec3{v.x, horizontalOnly ? 0.0f : v.y, 0} - vel_) * std::min(1.0f, dt / kFlickSmooth);
            const float speed = std::sqrt(vel_.x * vel_.x + vel_.y * vel_.y);
            const float lx = local.x, ly = horizontalOnly ? 0.0f : local.y;
            if (!inStroke_) {
                if (speed > kMoveSpeed) {
                    inStroke_ = true;
                    slowFor_ = 0;
                    decided_ = false;
                    strokeFrom_ = prevLocal_;
                }
            } else {
                // (the way it had gone up to the last read; once it has given its note, the way it went then: round a
                // curve the whole stroke's way turns with the hand)
                float sx = prevLocal_.x - strokeFrom_.x, sy = (horizontalOnly ? 0.0f : prevLocal_.y) - strokeFrom_.y;
                if (decided_) { sx = strokeHead_.x; sy = strokeHead_.y; }
                slowFor_ = speed < kStopSpeed ? slowFor_ + dt : 0.0f;
                if (slowFor_ >= kStopTime) {
                    inStroke_ = false;
                } else if (speed > kMoveSpeed && sx * sx + sy * sy > 1e-4f &&
                           vel_.x * sx + vel_.y * sy < 0.5f * speed * std::sqrt(sx * sx + sy * sy)) {
                    // turned more than 60 degrees (back, or round a corner: out, then up): a new stroke from here
                    strokeFrom_ = prevLocal_;
                    decided_ = false;
                }
            }
            noteLeft_ -= dt;
            if (inStroke_ && !decided_) {
                const float sx = lx - strokeFrom_.x, sy = ly - strokeFrom_.y;
                if (std::sqrt(sx * sx + sy * sy) >= kStrokeLen) {
                    decided_ = true;
                    strokeHead_ = Vec3{sx, sy, 0} * (1.0f / std::sqrt(sx * sx + sy * sy));
                    BatonDir nd = BatonDir::None;
                    if (note_ != BatonDir::None) {
                        const Vec3 n = dir_vec(note_);
                        const float a = -(sx * n.x + sy * n.y);  // (how far back the way the note went)
                        const float px = sx + a * n.x, py = sy + a * n.y;
                        if (a > 0 && std::sqrt(px * px + py * py) < 0.6f * a) {
                            backing_ = true;
                            backDir_ = reverse_dir(note_);
                        } else {
                            nd = a > 0 ? axis_dir(px, py) : axis_dir(sx, sy);
                        }
                    } else {
                        nd = axis_dir(sx, sy);
                    }
                    if (nd != BatonDir::None) {
                        if (nd != note_) ++flicks_;
                        note_ = nd;
                        noteLeft_ = kFlickHold;
                        backing_ = false;
                    }
                }
            }
            if (backing_) {
                const Vec3 b = dir_vec(backDir_);
                const float out = std::sqrt(lx * lx + ly * ly);
                // on past the middle's zone (the set zone, or grown by big moves: an arm brought back overshoots): the
                // opposite note
                if (inStroke_ && lx * b.x + ly * b.y >= std::max(kStrokeLen, reach)) {
                    note_ = backDir_;
                    noteLeft_ = kFlickHold;
                    backing_ = false;
                    ++flicks_;
                } else if (note_ != BatonDir::None && noteLeft_ <= 0 && (out < reach * BatonZone::kExitShare || !inStroke_)) {
                    note_ = BatonDir::None;  // back: the middle
                }
                if (!inStroke_ && note_ == BatonDir::None) backing_ = false;
            }
            d = note_;
        }
        prevLocal_ = local;
        lastOut_ = d;
        // the middle follows the resting hand: while no direction is held the centre eases towards where the hand is
        // (kFollow), so a hand that drifts as you play still comes back to the middle (2026-11-10, calibrated in the
        // headset: "I can't hit the neutral note"; the centre had stayed where the hand first settled)
        // (only while the hand is still: a slow move out to a direction did not drag the middle along with it)
        // (not during a stroke of the wand's: a slow move out had the middle follow it and never got 8 cm from it)
        if (d == BatonDir::None && dt > 1e-4f && moved / dt < kStillSpeed && !(flicks && inStroke_))
            centre_ = centre_ + (pos - centre_) * std::min(1.0f, dt / kFollow);
        // a direction held with the hand still for kRestTime: the hand is resting there, so that is the middle now
        if (d != BatonDir::None && dt > 1e-4f) {
            restStill_ = moved / dt < kStillSpeed ? restStill_ + dt : 0.0f;
            if (restStill_ >= kRestTime) {
                centre_ = pos;
                restStill_ = 0;
                zone_.dir = BatonDir::None;
                note_ = BatonDir::None;
                backing_ = false;
                ++rests_;
                return BatonDir::None;
            }
        } else {
            restStill_ = 0;
        }
        return d;
    }
    void reset() {
        centred_ = haveLast_ = false;
        still_ = 0;
        zone_.dir = BatonDir::None;
        vel_ = prevLocal_ = {};
        flickArmed_ = true;
        flickLeft_ = 0;
        flickActive_ = inStroke_ = false;
        strokeFrom_ = {};
        lastOut_ = BatonDir::None;
        peak_ = 0;
        note_ = backDir_ = BatonDir::None;
        noteLeft_ = 0;
        backing_ = decided_ = false;
    }
    bool centred() const { return centred_; }
    Vec3 offset() const { return centred_ ? local_ : Vec3{}; }  // (diagnostics) metres from its centre, its frame
    uint32_t rests() const { return rests_; }  // (diagnostics) times the middle was taken where the hand rested in a direction
    uint32_t flicks() const { return flicks_; }  // (diagnostics) flicks taken
    bool flicking() const { return note_ != BatonDir::None; }  // (diagnostics) a stroke's note held
    uint32_t longs() const { return longs_; }   // (diagnostics) long strokes that gave their direction
    float radius() const { return radius_; }    // (diagnostics) the zone's radius at the last read (metres)
    Vec3 centre() const { return centre_; }  // (tracking space; while centred)

private:
    bool centred_ = false, haveLast_ = false;
    float still_ = 0;
    Vec3 centre_, last_;
    Quat frame_;
    Vec3 local_;
    float restStill_ = 0;
    uint32_t rests_ = 0;
    Vec3 vel_, prevLocal_;
    bool flickArmed_ = true;
    BatonDir flickDir_ = BatonDir::None;
    float flickLeft_ = 0;
    bool flickActive_ = false, inStroke_ = false;
    Vec3 strokeFrom_;
    BatonDir lastOut_ = BatonDir::None;  // (what the last read gave, after the flick and stroke)
    uint32_t flicks_ = 0, longs_ = 0;
    float peak_ = 0, radius_ = 0;
    BatonDir note_ = BatonDir::None, backDir_ = BatonDir::None;  // (the stroke's note; the way back from it)
    float noteLeft_ = 0;
    bool backing_ = false, decided_ = false;
    float slowFor_ = 0;  // (s the stroke has been slower than kStopSpeed)
    Vec3 strokeHead_;  // (the stroke's way when it gave its note, a unit vector)
    static Vec3 dir_vec(BatonDir d) {
        return d == BatonDir::Right ? Vec3{1, 0, 0} : d == BatonDir::Left ? Vec3{-1, 0, 0}
             : d == BatonDir::Up    ? Vec3{0, 1, 0} : d == BatonDir::Down ? Vec3{0, -1, 0} : Vec3{};
    }
    static BatonDir reverse_dir(BatonDir d) {
        return d == BatonDir::Right ? BatonDir::Left : d == BatonDir::Left ? BatonDir::Right
             : d == BatonDir::Up    ? BatonDir::Down : d == BatonDir::Down ? BatonDir::Up : BatonDir::None;
    }
    static BatonDir axis_dir(float x, float y) {
        if (x == 0 && y == 0) return BatonDir::None;
        return std::fabs(x) >= std::fabs(y) ? (x > 0 ? BatonDir::Right : BatonDir::Left) : (y > 0 ? BatonDir::Up : BatonDir::Down);
    }
    BatonZone zone_;
};

// the channel's direction: its stick's when the stick has one, else its hand's
inline BatonDir baton_channel(BatonDir stick, BatonDir hand) { return stick != BatonDir::None ? stick : hand; }
// the game's stick for a direction: a full push along it (x right, y up, as the controllers' sticks)
inline void baton_stick(BatonDir d, float& x, float& y) {
    x = d == BatonDir::Right ? 1.0f : d == BatonDir::Left ? -1.0f : 0.0f;
    y = d == BatonDir::Up ? 1.0f : d == BatonDir::Down ? -1.0f : 0.0f;
}

// both channels, per read of the pad
struct BatonInput {
    bool conducting = false;    // the game's metronome is on (VR running)
    float stick[2][2] = {};     // the left and right sticks (x right, y up)
    bool tracked[2] = {};       // each controller's pose is tracked now
    Vec3 hand[2];               // each controller's grip position (tracking space, metres)
    float headingYaw = 0;       // the head's heading now (taken when a hand is centred)
    bool hands = true;          // vr.batonHands
    float reach = 0.12f;        // vr.batonHandReach
    float stickEnter = 0.5f, stickExit = 0.35f;  // of the stick's full push (above its dead zone, below where it
                                                 // reaches when pushed on purpose)
    // the hand that holds the notes (0 left, 1 right): the one holding the wand, the sword hand (vr.leftHanded). The
    // other hand holds the beat
    int noteHand = 0;
    // the two hands' channels swapped, each hand keeping its calibration (the wand's hand still read with the wand's
    // zone, kNoteReachShare and kNoteShape): the wand's hand gives the right stick, the other the left (asked for in the
    // headset, 2026-11-09: "the left hand and right hand needs their roles reversed, right hand should be the one hitting
    // the notes ... but keep the calibrations the same")
    bool swapHands = false;
    // a hand's middle held (its trigger, vr_game.cpp): that hand gives no direction, wherever it is (asked for in the
    // headset, 2026-11-10: the middle note too hard to hit by hand)
    bool middle[2] = {};
    bool recentre = false;      // the view was recentred this read
    float dt = 0;               // seconds since the last read (a hand's settling)
};
struct BatonOutput {
    BatonDir dir[2] = {};                // what each channel gives the game
    BatonDir stickDir[2] = {}, handDir[2] = {};
    bool started = false, stopped = false;  // conducting began / ended at this read
};
class Baton {
public:
    // the left hand's zone (asked for in the headset: its left, then its right, too hard to reach): its left note out
    // at half the radius (6 cm at the default 12), its right at 0.75 (9 cm), up and down at 0.87 (10.4 cm)
    // (2026-11-09, first person: "still too hard to hit the right note", the right being across the body for the left
    // hand: across now as near as outward, and up and down nearer: 0.5, 0.5, 0.7; 0.5, 0.75, 0.87 before)
    // (2026-11-10, calibrated in the headset: the wand's moves were 10 to 50 cm, left and right won over up and down
    // on a diagonal, and the middle could not be hit: even on every side now, 1, 1, 1)
    static constexpr BatonShape kNoteShape{1.0f, 1.0f, 1.0f};
    // the notes' zone, a share of vr.batonHandReach on every side (asked for with a video, 2026-11-09: "most of the time
    // it doesn't register the wand direction", the player reaching far for each note; 0.65 that day, then "still too
    // hard ... you need to stretch your hand out too far": 0.5): at the default 12 cm the notes left and right at 3 cm,
    // up and down at 4.2, either hand. vr_game.cpp gives the notes' hand as the
    // wand's tip (kBatonTip ahead of the grip), so a turn of the wrist moves it as well as the arm
    // (0.75 since 2026-11-10, calibrated in the headset: at 3 cm a jitter of the wand gave a direction and the middle,
    // back within 80 % of it, could not be reached; the moves were 10 to 50 cm. At the default 12 cm: a direction from
    // 9 cm, the middle back within 7.2)
    // (0.85 since the second run that day: a direction from 10.2 cm, the middle back within 9.2)
    static constexpr float kNoteReachShare = 0.85f;
    static BatonShape note_shape(int hand) {
        const BatonShape s = kNoteShape;
        return hand == 1 ? BatonShape{s.right, s.left, s.vertical} : s;
    }
    BatonOutput update(const BatonInput& in) {
        BatonOutput out;
        out.started = in.conducting && !conducting_;
        out.stopped = !in.conducting && conducting_;
        conducting_ = in.conducting;
        if (!in.conducting || out.started) {  // nothing kept from before (or from the last song)
            for (int s = 0; s < 2; ++s) { stick_[s].dir = BatonDir::None; hand_[s].reset(); }
            if (!in.conducting) return out;
        }
        for (int s = 0; s < 2; ++s) {
            // both channels in all four directions: the game reads the C-stick's up and down too (Link's left arm,
            // getTactPlayLeftArmAnm: WAITTAKTLHANDU / D), and up / down on the right stick and hand were asked for in
            // the headset
            out.stickDir[s] = stick_[s].update(in.stick[s][0], in.stick[s][1], in.stickEnter, in.stickExit);
            // (the notes from the wand's hand, the beat from the other: in.noteHand. It was always the left hand's
            // notes, and the player conducted with the hand holding the wand, the right: 70 beats logged, 20 notes)
            const int wand = in.noteHand == 1 ? 1 : 0;
            int h = s == 0 ? wand : 1 - wand;
            if (in.swapHands) h = 1 - h;  // (each hand keeps its calibration: the wand's zone stays with the wand's hand)
            if (h != handOf_[s]) { handOf_[s] = h; hand_[s].reset(); }
            const bool wandZone = h == wand;
            const float reach = wandZone ? in.reach * kNoteReachShare : in.reach;
            out.handDir[s] = in.hands ? hand_[s].update(in.tracked[h], in.hand[h], in.headingYaw, reach, in.recentre,
                                                        in.dt, false, wandZone ? note_shape(h) : BatonShape{}, wandZone)
                                      : BatonDir::None;
            if (in.middle[h]) out.handDir[s] = BatonDir::None;  // (the middle held: the zone still follows the hand)
            if (!in.hands) hand_[s].reset();
            out.dir[s] = baton_channel(out.stickDir[s], out.handDir[s]);
        }
        return out;
    }
    bool conducting() const { return conducting_; }
    // where the hands settled when conducting began (tracking space): the midpoint of both, or the one settled
    // (diagnostics, the calibration) a channel's hand: which controller, its offset from its centre (metres, x right,
    // y up), whether it has settled
    int hand_of(int channel) const { return handOf_[channel & 1]; }
    Vec3 hand_offset(int channel) const { return hand_[channel & 1].offset(); }
    bool hand_settled(int channel) const { return hand_[channel & 1].centred(); }
    uint32_t hand_rests(int channel) const { return hand_[channel & 1].rests(); }
    bool hand_flicking(int channel) const { return hand_[channel & 1].flicking(); }
    float hand_radius(int channel) const { return hand_[channel & 1].radius(); }
    bool hands_centre(Vec3& at) const {
        const bool l = hand_[0].centred(), r = hand_[1].centred();
        if (!conducting_ || (!l && !r)) return false;
        at = l && r ? (hand_[0].centre() + hand_[1].centre()) * 0.5f : l ? hand_[0].centre() : hand_[1].centre();
        return true;
    }

private:
    bool conducting_ = false;
    BatonFilter stick_[2];
    BatonHand hand_[2];  // by channel: the notes' hand, the beat's
    int handOf_[2] = {0, 1};
};

}  // namespace vr

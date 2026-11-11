// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The sailing views (docs/VR_SAILING_VIEWS.md): more views while Link sails the King of Red Lions on the Great Sea,
// chosen with the right stick's click after first and third person: the view on the figurehead's neck, then the Free
// Roam Camera (above and behind the boat, the triggers zoom, the right stick slides it).
//
// What is here has no guest memory and no OpenXR (runtime/tools/vr_test.cpp tests it):
// - the view mode (ViewCycle): the one state the right stick's click changes. Outside sailing it cycles first person
//   and third person as before; while sailing it adds the neck view (kNeckView) and the Free Roam Camera
//   (kFreeRoamView), and leaving sailing in one of the boat's views returns to third person at once;
// - when the boat's views may be chosen (SailState, sailing_eligible);
// - the boat's frame (BoatFrame: its position and shape_angle, as its model's base matrix is built) blended between
//   logic steps as its drawn model is (BoatFrames);
// - the boat's geometry the anchors are placed from (BoatGeometry: the mast joint, the figurehead's head joint and
//   where Link sits, measured in the boat's own frame from its models once they hold still: BoatMeasure), and the
//   neck view's anchor in that frame (sail_anchor, its offsets in kSailAnchors);
// - the Free Roam Camera (FreeRoamCamera: its zoom and slide, reset each time it begins; its numbers in kFreeRoam).
// HeadController (vr_head.h) composes the head-tracked view from the anchor (boat_view); vr_game.cpp reads the boat.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "vr_math.h"

namespace vr {

enum class ViewMode : uint8_t { FirstPerson, ThirdPerson, FreeRoam, Neck };
// the boat's views offered by the right stick's click while sailing, after third person: the neck view (off from
// 2026-11-03, on again 2026-11-06), then the Free Roam Camera (asked for, 2026-11-06, in place of the side view; the
// mast view was deleted then)
constexpr bool kFreeRoamView = true;
constexpr bool kNeckView = true;
inline bool boat_view_mode(ViewMode m) { return m == ViewMode::FreeRoam || m == ViewMode::Neck; }
inline const char* view_mode_name(ViewMode m) {
    switch (m) {
    case ViewMode::FirstPerson: return "first person";
    case ViewMode::ThirdPerson: return "third person";
    case ViewMode::FreeRoam: return "the Free Roam Camera, above and behind the boat";
    case ViewMode::Neck: return "on the King of Red Lions' neck, facing ahead";
    }
    return "?";
}

// The one view mode. Every change goes through click() or update(); `thirdPerson()` is what the rest of the VR code
// called the third-person view (the game's camera or the boat's: Link's head shown, the game's HUD, no motion controls)
struct ViewCycle {
    ViewMode mode = ViewMode::FirstPerson;
    bool third_person() const { return mode != ViewMode::FirstPerson; }
    // sailing ended (or never began) while in one of the boat's views: third person, the game's camera, at once.
    // Returns whether the mode changed
    bool update(bool sailing) {
        if (sailing || !boat_view_mode(mode)) return false;
        mode = ViewMode::ThirdPerson;
        return true;
    }
    // one click: the next mode (update first, so a stale boat view is never the start of a click). Returns the new mode
    ViewMode click(bool sailing) {
        update(sailing);
        switch (mode) {
        case ViewMode::FirstPerson: mode = ViewMode::ThirdPerson; break;
        // while sailing: third person, the neck view, the Free Roam Camera, then first person (asked for, 2026-11-06)
        case ViewMode::ThirdPerson:
            mode = sailing && kNeckView       ? ViewMode::Neck
                   : sailing && kFreeRoamView ? ViewMode::FreeRoam
                                              : ViewMode::FirstPerson;
            break;
        case ViewMode::Neck: mode = sailing && kFreeRoamView ? ViewMode::FreeRoam : ViewMode::FirstPerson; break;
        case ViewMode::FreeRoam: mode = ViewMode::FirstPerson; break;
        }
        return mode;
    }
};

// daShip_c::mCurMode (d_a_ship.h): sailing is waiting at the tiller, steering with the sail up or paddling with it down,
// and using the boat's tools at sea: the cannon (MODE_CANNON 9) and the salvage arm with the grappling hook (MODE_CRANE
// 10, its lift MODE_CRANE_UP 11). Those had ended the boat's views the moment the cannon or the hook came out (the
// player's log, 2026-11-06: "the boat's views end: the boat is not sailing"); they must work in every ship view.
// Getting on (READY_FIRST/SECOND 3, 4), getting off (GET_OFF_FIRST/SECOND 5, 6), events (7), talking (8), a tornado
// (12), a warp (13, 14), a whirlpool (15) and being thrown (16) are not
constexpr uint8_t kShipModeCannon = 9, kShipModeCrane = 10, kShipModeCraneUp = 11;
inline bool sailing_ship_mode(uint8_t m) {
    return m <= 2 || m == kShipModeCannon || m == kShipModeCrane || m == kShipModeCraneUp;
}

// what decides whether the boat's views may be chosen (vr_game.cpp reads it each pad read and each frame)
struct SailState {
    bool ship = false;         // the boat actor exists (play's ship pointer, its position finite)
    bool riding = false;       // Link rides it (player status 0 SHIP_RIDE, play +0x5CD8 bit 0x10000)
    uint8_t shipMode = 0xFF;   // its mCurMode
    bool greatSea = false;     // the stage is the Great Sea ("sea"): the open world
    bool playCamera = false;   // normal play with the player's camera: no event, cutscene, talk, title, conducting
    bool menu = false;         // a GamePad menu (items, map, pause...) is open
    bool zoomView = false;     // the telescope's or the Picto Box's view
};
inline bool sailing_eligible(const SailState& s) {
    return s.ship && s.riding && s.greatSea && s.playCamera && !s.menu && !s.zoomView && sailing_ship_mode(s.shipMode);
}
// (log) why the boat's views are not offered: the first condition that fails
inline const char* sail_blocker(const SailState& s) {
    if (!s.riding) return "Link is not on the boat";
    if (!s.ship) return "no boat";
    if (!s.greatSea) return "not on the Great Sea";
    if (!s.playCamera) return "an event, a talk or a cutscene";
    if (s.menu) return "a menu";
    if (s.zoomView) return "the telescope or the Picto Box";
    if (!sailing_ship_mode(s.shipMode)) return "the boat is not sailing (getting on or off, a tornado, a warp...)";
    return "sailing";
}

// ---- the boat's controls (docs/VR_CONTROLS.md, "On the boat"; asked for, 2026-11-06): only while Link rides the boat
// in normal play, in every view (first and third person, the neck view, the Free Roam Camera), and nowhere else
// - A also presses ZR while sailing (boat_hop), the game's R-lock trigger, the boat's hop (daShip_c::procSteerMove: CPad_R_LOCK_TRIGGER, when
//   the sail is up, the boat goes faster than 30 with wind of 20 and more). Its own A press stays the game's (getting
//   off, talking to the King of Red Lions, holding it to stop). The right stick down does not press ZR on the boat (it
//   is the camera's there, in every view), so A is the only hop;
// - the shield hand's grip held makes the left stick the D-pad: pushed past kBoatDpadPush its strongest direction
//   presses D-pad up, down, left or right, and the boat is not steered meanwhile (the game gets no left stick). Let go
//   of the grip and the stick steers again. Off the boat the grip is as before (third person: D-pad up)
inline bool boat_controls(const SailState& s) {
    return s.ship && s.riding && s.playCamera && !s.menu && !s.zoomView;
}
// A's hop only while Link sails the boat: waiting at the tiller, steering, paddling (modes 0 to 2); not with the cannon
// or the salvage arm out (ZR held there lets the boat move while aiming, or stops the hook's stick), getting on or off,
// or anywhere else (asked for, 2026-11-06)
inline bool boat_hop(const SailState& s) { return boat_controls(s) && s.shipMode <= 2; }
// Link's aimed weapons on the boat (d_a_player: SHIP_BOOMERANG 0x8B, SHIP_HOOKSHOT 0x8C, SHIP_BOW 0x8D). Taken out in
// one of the boat's views (the neck view, the Free Roam Camera), the view goes to first person at once, where the
// first-person motion controls aim and use them (VR archery, the hand's pointing, the triggers, the boomerang's swing;
// asked for, 2026-11-06). The telescope (0x8A) ends the boat's views anyway; the cannon (0x8E) and the salvage arm
// (0x8F) keep them
constexpr uint32_t kProcShipBoomerang = 0x8B, kProcShipHookshot = 0x8C, kProcShipBow = 0x8D;
inline bool boat_weapon_proc(uint32_t proc) { return proc >= kProcShipBoomerang && proc <= kProcShipBow; }
// The boomerang on the boat (Link's procShipBoomerang): thrown, Link stays in SHIP_BOOMERANG (0x8B) with mEquipItem no
// longer the boomerang while it flies (no laser then; held, the laser is the hand's own 15 m line); caught, the game puts it away at once (back to
// paddling, 0x89, or steering, 0x88), where on land Link keeps holding it. To be reused after the catch as on land (asked
// for, 2026-11-06: "the boomerang should be able to be reused once the player catches it"), VR takes it out again: its
// item button pressed as the item wheel presses one (a press and release takes it out, it does not throw it). Put away
// before a throw (A or B with it held) it is not taken out again
constexpr uint16_t kBoatItemBoomerang = 0x2D, kBoatItemNone = 0x100;
inline bool boat_boomerang_flying(uint32_t proc, uint16_t equipItem) {
    return proc == kProcShipBoomerang && equipItem != kBoatItemBoomerang;
}
struct BoatBoomerangCatch {
    bool flown = false;  // thrown in this use: it flew
    // each pad read on the boat in first person: true once, when it has come back and been put away
    bool step(uint32_t proc, uint16_t equipItem) {
        if (boat_boomerang_flying(proc, equipItem)) {
            flown = true;
            return false;
        }
        if (proc == kProcShipBoomerang) return false;  // held, aimed
        const bool caught = flown && (proc == 0x88 || proc == 0x89) && equipItem == kBoatItemNone;
        flown = false;
        return caught;
    }
};
// The bow's first-person aim: the land's (daPyProc_BOW_SUBJECT 0x94) and the boat's (SHIP_BOW 0x8D). The HD game treats
// them alike (setDrawHandModel hides Link's left hand in both before an arrow is on the bow); VR's bow fixes (the hand
// kept drawn, the bow's place in the hand from the aiming pose) were keyed to 0x94 only, so on the boat the hand
// vanished and the bow turned in it again (reported, 2026-11-06: "the bow the hands missing")
constexpr uint32_t kProcBowSubject = 0x94;
inline bool bow_aim_proc(uint32_t proc) { return proc == kProcBowSubject || proc == kProcShipBow; }
// Link's procedures while he rides the boat (SHIP_READY 0x86 to SHIP_RESTART 0x91): he sits facing the boat's way,
// whatever way the head looks
inline bool ship_ride_proc(uint32_t proc) { return proc >= 0x86 && proc <= 0x91; }
inline bool boat_view_to_first_person(ViewMode m, uint32_t proc) { return boat_view_mode(m) && boat_weapon_proc(proc); }
// The views the item wheel opens in (the sword hand's trigger with nothing in Link's hands, or the reach over the
// shoulder): first person, and the Red Lion's view (asked for, 2026-11-06; a weapon chosen there, the bow, the
// boomerang or the hookshot, then takes the view to first person: boat_view_to_first_person). Not third person, nor the
// Free Roam Camera, whose triggers zoom it
inline bool wheel_view(ViewMode m) { return m == ViewMode::FirstPerson || m == ViewMode::Neck; }
// The same in third person in normal play off the boat (asked for, 2026-11-06): the left grip held, the left stick is
// the D-pad and does not move Link. `playNow`: normal play with the stick Link's (no event, menu, telescope or Picto
// Box, not conducting). First person is unchanged (there the grip closes Link's left hand)
inline bool left_grip_dpad(bool boatControls, bool thirdPerson, bool playNow) {
    return boatControls || (thirdPerson && playNow);
}
// which physical grip makes the left stick the D-pad (0 the left, 1 the right): the right, on the boat and in third
// person on foot (asked for, 2026-11-10: "change that to R grip to toggle dpad and then L grip can guard instead", then
// "swap the grips for boat controls dpad too so they match 3rd person mode"; the boat's was the left)
inline int dpad_grip_side(bool boatControls) { (void)boatControls; return 1; }
// the left grip held is the game's block (ZR) in third person on foot, in play: not on the boat, where it is the D-pad
inline bool left_grip_guards(bool boatControls, bool thirdPerson, bool playNow) {
    return !boatControls && thirdPerson && playNow;
}
enum class BoatDpad : uint8_t { None, Up, Down, Left, Right };
constexpr float kBoatDpadPush = 0.5f;
inline BoatDpad boat_dpad(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y) || std::sqrt(x * x + y * y) < kBoatDpadPush) return BoatDpad::None;
    if (std::fabs(y) >= std::fabs(x)) return y > 0 ? BoatDpad::Up : BoatDpad::Down;
    return x > 0 ? BoatDpad::Right : BoatDpad::Left;
}

// ---- the Free Roam Camera's picture (asked for, 2026-11-06): Link's whole body is not drawn there (his model, the
// items in his hands, his shadow: mods/vr_hooks.cpp daPy_Draw), so the boat is seen clear; the "Free Roam" label is
// not shown any more (kFreeRoamLabel)
constexpr bool kFreeRoamLabel = false;
inline bool free_roam_hides_link(ViewMode m, bool playCamera) { return m == ViewMode::FreeRoam && playCamera; }

// ---- the boat's frame: T(current.pos) * ZXY(shape_angle), as daShip_c::setMtx builds its body's base matrix
// (mDoMtx_ZXYrotM: about Y, then X, then Z; game units, s16 angles)
struct BoatFrame {
    Vec3 pos;
    int16_t ax = 0, ay = 0, az = 0;
};
inline Quat boat_rotation(float ax, float ay, float az) {
    return normalize(axis_angle({0, 1, 0}, ay) * axis_angle({1, 0, 0}, ax) * axis_angle({0, 0, 1}, az));
}
inline Quat boat_rotation(const BoatFrame& f) {
    return boat_rotation(s16_to_radians(f.ax), s16_to_radians(f.ay), s16_to_radians(f.az));
}
// the boat's frame of the last two logic steps, blended at the step fraction as its model is (interp: model matrices)
struct BoatFrames {
    uint32_t ship = 0;   // whose
    uint64_t step = 0;   // the step `cur` is of
    BoatFrame prev, cur;
    bool valid = false;
    void reset() { *this = {}; }
    // a new logic step's frame (the same step again: only the latest)
    void sample(uint32_t who, uint64_t atStep, const BoatFrame& f) {
        if (!valid || who != ship) {
            ship = who;
            prev = cur = f;
            step = atStep;
            valid = true;
            return;
        }
        if (atStep != step) prev = cur;
        cur = f;
        step = atStep;
    }
    // the boat as drawn at the fraction t between the two steps: its position, its rotation, its heading (radians)
    Vec3 position(float t) const { return t < 1.0f ? lerp(prev.pos, cur.pos, t) : cur.pos; }
    float heading(float t) const {
        return t < 1.0f ? blend_heading(s16_to_radians(prev.ay), s16_to_radians(cur.ay), t) : s16_to_radians(cur.ay);
    }
    Quat rotation(float t) const {
        if (!(t < 1.0f)) return boat_rotation(cur);
        auto a = [t](int16_t p, int16_t c) { return blend_heading(s16_to_radians(p), s16_to_radians(c), t); };
        return boat_rotation(a(prev.ax, cur.ax), a(prev.ay, cur.ay), a(prev.az, cur.az));
    }
    // a point of the boat's own frame in the game world, drawn at t
    Vec3 to_world(Vec3 local, float t) const { return position(t) + rotate(rotation(t), local); }
};

// ---- the boat's geometry, in its own frame (game units; +Z the bow, +Y up): measured from its models (BoatMeasure),
// these defaults until then
struct BoatGeometry {
    Vec3 mast{0, 30, 30};       // the mast joint (FN_BODY J_FN_MAST, 6): its foot on the deck
    Vec3 head{0, 200, 200};     // the figurehead's head joint (FN_HEAD_H J_FN_ATAMA, 8)
    Vec3 seat{0, 20, -120};     // where Link sits at the tiller (his position)
    bool measured = false;
};
// a measurement is believed only when it is a boat: the mast near the middle line, the head ahead of and above it,
// Link behind it
inline bool plausible_boat(const BoatGeometry& g) {
    auto ok = [](Vec3 v) { return finite(v) && std::fabs(v.x) < 60 && std::fabs(v.y) < 600 && std::fabs(v.z) < 600; };
    return ok(g.mast) && ok(g.head) && ok(g.seat) && std::fabs(g.mast.x) < 25 && std::fabs(g.head.x) < 40 &&
           g.head.z > g.mast.z + 40 && g.head.y > g.seat.y + 40 && g.seat.z < g.mast.z - 20;
}
// the measurement holds still for kBoatSettleSteps steps (within kBoatSettleUnits) before it is kept: the mast's own
// animation (raised, lowered) and the head's (it turns to talk) are over
constexpr int kBoatSettleSteps = 10;
constexpr float kBoatSettleUnits = 0.5f;
struct BoatMeasure {
    uint32_t ship = 0;
    BoatGeometry last, kept;
    int still = 0;
    // a step's measurement of `who`; returns true when it was kept now (the log)
    bool add(uint32_t who, const BoatGeometry& g) {
        if (who != ship) {
            *this = {};
            ship = who;
        }
        if (kept.measured || !plausible_boat(g)) {
            still = 0;
            return false;
        }
        const bool same = still > 0 && length(g.mast - last.mast) < kBoatSettleUnits &&
                          length(g.head - last.head) < kBoatSettleUnits && length(g.seat - last.seat) < kBoatSettleUnits;
        still = same ? still + 1 : 1;
        last = g;
        if (still < kBoatSettleSteps) return false;
        kept = g;
        kept.measured = true;
        return true;
    }
    // the geometry to place the views from: the kept measurement, or the defaults
    BoatGeometry geometry() const { return kept.measured ? kept : BoatGeometry{}; }
};

// ---- the neck view's anchor: where the tracking space's origin (your head at the switch) stands on the boat and which
// way it faces, relative to the boat's geometry (game units; boat frame: +X the boat's left, +Y up, +Z the bow); a
// standing eye is about 90 units above the feet (Link's own, first person)
constexpr float kStandingEye = 90.0f;
struct SailAnchorOffsets {
    // the Red Lion's view: just over the head, so looking down shows its eyebrows and head (asked for in the headset,
    // 2026-11-06: 60 behind and 55 above was behind the horns, then 60 ahead and 40 above too far forward, then 15 ahead
    // and 55 above almost right: a little back and down), facing the bow
    float neckAhead = 5.0f, neckHigh = 45.0f;
};
constexpr SailAnchorOffsets kSailAnchors{};
struct SailAnchor {
    Vec3 local;        // boat frame
    float yaw = 0;     // radians relative to the boat's heading (0: the bow, pi: the stern), game convention
};
inline SailAnchor sail_anchor(ViewMode m, const BoatGeometry& g, const SailAnchorOffsets& o = kSailAnchors) {
    switch (m) {
    case ViewMode::Neck: return {g.head + Vec3{0, o.neckHigh, o.neckAhead}, 0};
    default: return {};
    }
}

// ---- the Free Roam Camera (asked for, 2026-11-06): it begins above and behind the boat, facing its bow, as the game's
// sailing camera but higher. The left trigger zooms it in towards the boat, the right trigger out (along the line from
// the boat to the camera, faster the harder they are pulled); the right stick slides it left and right, and up on it
// forward along the boat (past Link, towards the bow), down back towards the stern (asked for, 2026-11-06), without
// turning it. It follows the boat's position and turn, never its pitch or roll (the horizon stays level, and a
// camera far behind the boat does not bob with its waves). Each time it begins it starts again from its first place.
// Every number is here (game units, seconds)
struct FreeRoamOffsets {
    float back = 300.0f, up = 250.0f;  // the start: this far behind and above the boat's middle, on the deck
    float minDistance = 120.0f;        // the zoom's nearest (clear of the boat), and its farthest
    float maxDistance = 2000.0f;
    float zoomPerSecond = 600.0f;      // at a trigger pulled all the way
    float slidePerSecond = 400.0f;     // at the stick pushed all the way
    float slideLimit = 1500.0f;        // the slide's farthest from the start's line, each way
    float triggerDeadZone = 0.1f;      // a trigger resting lightly does not zoom
};
constexpr FreeRoamOffsets kFreeRoam{};
// the boat's middle on its deck, the point the camera zooms towards (boat frame)
inline Vec3 free_roam_focus(const BoatGeometry& g) { return {0, g.seat.y, 0.5f * (g.mast.z + g.seat.z)}; }
struct FreeRoamCamera {
    float distance = 0;  // from the focus along the start's direction (back and up)
    float slideX = 0;    // to the view's right (the boat's right: its -X)
    float slideZ = 0;    // forward along the boat (its +Z, the bow): the right stick's up
    FreeRoamCamera() { reset(); }
    // the start (each time the view begins)
    void reset(const FreeRoamOffsets& o = kFreeRoam) {
        distance = std::sqrt(o.back * o.back + o.up * o.up);
        slideX = slideZ = 0;
    }
    // one frame: the triggers (0..1: in, out) and the right stick (after its dead zone), over dt seconds
    void update(float zoomIn, float zoomOut, float stickX, float stickY, float dt, const FreeRoamOffsets& o = kFreeRoam) {
        if (!(dt > 0) || !std::isfinite(dt)) return;
        auto pull = [&o](float v) {
            if (!std::isfinite(v) || !(v > o.triggerDeadZone)) return 0.0f;
            return (std::min(v, 1.0f) - o.triggerDeadZone) / (1.0f - o.triggerDeadZone);
        };
        auto stick = [](float v) { return std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f; };
        distance = std::clamp(distance + (pull(zoomOut) - pull(zoomIn)) * o.zoomPerSecond * dt, o.minDistance,
                              o.maxDistance);
        slideX = std::clamp(slideX + stick(stickX) * o.slidePerSecond * dt, -o.slideLimit, o.slideLimit);
        slideZ = std::clamp(slideZ + stick(stickY) * o.slidePerSecond * dt, -o.slideLimit, o.slideLimit);
    }
    // where it stands in the boat's frame (facing the bow: yaw 0)
    Vec3 local(const BoatGeometry& g, const FreeRoamOffsets& o = kFreeRoam) const {
        const float len = std::sqrt(o.back * o.back + o.up * o.up);
        const Vec3 dir{0, o.up / len, -o.back / len};  // back (-Z) and up from the focus
        return free_roam_focus(g) + dir * distance + Vec3{-slideX, 0, slideZ};
    }
};
// a point of the boat's frame turned by its heading only (no pitch or roll), in the world, drawn at t
inline Vec3 boat_level_to_world(const BoatFrames& f, Vec3 local, float t) {
    return f.position(t) + rotate(boat_rotation(0.0f, f.heading(t), 0.0f), local);
}

}  // namespace vr

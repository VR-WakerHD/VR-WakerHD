// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// Controller poses through short tracking gaps (docs/VR_CONTROLS.md, "Tracking gaps").
//
// Inside-out tracked controllers lose their position for moments (held behind the body, close to the headset, out
// of the cameras' view): the runtime then reports the pose without the position-valid flag, sometimes the
// orientation too. Treating every such read as "not tracked" dropped Link's arm to the game's animation for a moment
// and snapped it back. A hand keeps its last valid pose through a gap of up to `holdSeconds` instead, taking a fresh
// orientation when only the position is missing; only a longer loss counts as untracked.
#pragma once
#include "vr_math.h"

namespace vr {

struct PoseHold {
    Pose last;          // the last pose with a valid position
    double at = -1;     // when it was valid (seconds, the runtime's clock)
    bool have = false;
    bool seenTracked = false;  // the runtime has tracked this hand's position (POSITION_TRACKED) this session
    // back from a held gap: a short blend from the pose shown to the tracked one (no jump), from `from` since `since`
    Pose from, shown;
    double since = -1;
    bool wasHeld = false;
};
constexpr double kRecoverSeconds = 0.1;

// A pose for this read: the fresh one when its position and orientation are valid, else the held one (with a valid
// fresh orientation, if there is one) while the gap is shorter than holdSeconds. Returns false when there is none;
// *held tells a held pose.
inline bool hold_pose(PoseHold& h, bool positionValid, bool orientationValid, const Pose& fresh, double now,
                      double holdSeconds, Pose& out, bool* held = nullptr) {
    if (held) *held = false;
    if (positionValid && orientationValid) {
        h.last = fresh;
        h.at = now;
        h.have = true;
        out = fresh;
        return true;
    }
    if (!h.have || now < h.at || now - h.at > holdSeconds) return false;
    out = h.last;
    if (orientationValid) out.orientation = fresh.orientation;
    if (held) *held = true;
    return true;
}

// A hand's pose for this read, the runtime's tracking state taken into account (docs/VR_CONTROLS.md, "Tracking
// gaps"): `positionTracked` is XR_SPACE_LOCATION_POSITION_TRACKED_BIT. A position that is valid but not tracked is the
// runtime's estimate while the controller is out of its cameras' view (dead reckoning from the controller's motion
// sensors); it drifts, and the hands slid away from the controllers with it. Once the hand has been tracked this
// session such a read counts as a gap: the last tracked pose is held (with a fresh orientation) for up to
// holdSeconds, then the estimate is used, as the best there is. A runtime that never reports the tracked bit keeps the
// old behaviour (valid is enough). Back from a held gap the hand blends to the tracked pose over kRecoverSeconds
// instead of jumping; in normal tracking nothing is filtered (no added latency).
inline bool track_pose(PoseHold& h, bool positionValid, bool positionTracked, bool orientationValid, const Pose& fresh,
                       double now, double holdSeconds, Pose& out, bool* held = nullptr) {
    bool isHeld = false;
    bool ok;
    if (positionValid && positionTracked) h.seenTracked = true;
    const bool estimate = positionValid && !positionTracked && h.seenTracked;
    if (estimate) {
        if (h.have && now >= h.at && now - h.at <= holdSeconds) {
            out = h.last;
            if (orientationValid) out.orientation = fresh.orientation;
            isHeld = true;
        } else {
            out = fresh;  // a longer gap: the runtime's estimate (not kept as a tracked pose)
        }
        ok = true;
    } else {
        ok = hold_pose(h, positionValid, orientationValid, fresh, now, holdSeconds, out, &isHeld);
    }
    if (ok && !isHeld && h.wasHeld) {  // back from a gap: blend from what was shown
        h.from = h.shown;
        h.since = now;
    }
    if (ok && !isHeld && h.since >= 0 && now >= h.since && now - h.since < kRecoverSeconds) {
        const float t = float((now - h.since) / kRecoverSeconds);
        out.position = lerp(h.from.position, out.position, t);
        out.orientation = slerp(h.from.orientation, out.orientation, t);
    }
    h.wasHeld = ok && isHeld;
    if (ok) h.shown = out;
    if (held) *held = isHeld;
    return ok;
}

// A head position no head can reach (docs/VR_CONTROLS.md, "Jumps no head makes"): `fresh` the runtime's tracked
// position, `last` the last good one, `sinceLast` the seconds since it, `heldFor` how long a jump has been held (-1:
// none). A head moves at most kHeadJumpMetres plus kHeadSpeed a second, so a jump is held until the runtime's position
// is back within that reach; it keeps growing (a drifted estimate snapping back 0.5 to 1 m is taken after a moment),
// but downwards only the reach at the jump counts. Grown there too, a PSVR2 that lost the room and put the head 1.25 m
// lower (1.44 m in all, 2026-10-31) was taken as back after 0.47 s, and the view sank through the floor. Upwards it
// grows as before: that is how the tracker corrects an estimate that drifted down
constexpr float kHeadJumpMetres = 0.5f, kHeadSpeed = 2.0f;
inline bool head_out_of_reach(Vec3 fresh, Vec3 last, double sinceLast, double heldFor) {
    const auto reach = [](double s) { return kHeadJumpMetres + kHeadSpeed * float(s > 0 ? s : 0); };
    if (length(fresh - last) > reach(sinceLast)) return true;
    return heldFor >= 0 && last.y - fresh.y > reach(sinceLast - heldFor);
}

// Back from a held head (a gap or a jump), the view eases from where it was held to the tracked position over
// kHeadEaseSeconds instead of snapping: `offset` is held minus tracked when it came back (at `since`). The part of it
// still added at `now`
constexpr double kHeadEaseSeconds = 0.2;
inline Vec3 head_ease(Vec3 offset, double since, double now) {
    if (since < 0 || now < since || now - since >= kHeadEaseSeconds) return {};
    return offset * float(1.0 - (now - since) / kHeadEaseSeconds);
}

}  // namespace vr

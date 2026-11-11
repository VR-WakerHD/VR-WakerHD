// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The first-person VR head (docs/VR_HEAD_CONTROLLER.md): the one owner of where the tracking space stands in the game
// world and of the head pose each frame is drawn with.
//
// It owns:
// - the mapping from the tracking space to the game world (WorldMapping: origin, heading, scale, anchor), whose only
//   writers are its methods: the recentre, the turns (smooth, snap, the turn to Link on ladders, ledges and lock-ons)
//   and the head's pose for a frame;
// - the room-scale origin (RoomScale): the head's walk in the play space taken into Link's position, step by step;
// - the calibration (recentred for which Link, a recentre asked for);
// - the view mode it last drew (first person, or the game's own camera turned by the head) and its transitions;
// - the views drawn by display time (the screen blur's reprojection).
//
// Everything else gives it input: the headset's pose (OpenXR, located once a frame by the camera), Link's eye point and
// facing (his logic, blended between steps), the sticks (turns), the walls (the lean's limit). It does not read the
// guest's memory or the runtime: vr_game.cpp calls it from the game's hooks and writes its result into the game's
// camera once a frame (CameraOverride), and runtime/tools/vr_test.cpp tests it without a headset.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "vr_camera.h"
#include "vr_ik.h"
#include "vr_math.h"

namespace vr {

// The body under the head (docs/VR_FIRST_PERSON_CAMERA.md, "The body under the head"): your head's horizontal movement
// in the play space moves Link by the same amount before his logic (link_executing), through his own collision, and
// the tracking origin moves with him by what was taken. The view stays exactly where your head is (the move and the
// origin cancel), and his body, drawn at his position, stays under it. The origin is blended between steps with his
// position (HeadController::origin), as the anchor is: neither leads the other
struct RoomScale {
    Vec3 base;            // OpenXR metres: the head's position at the recentre
    Vec3 prev, cur;       // OpenXR metres taken by Link's moves up to the last two steps (cumulative)
    Vec3 pendingXr;       // this step's move (link_executing), taken into cur at its step (link_executed)
    Vec3 pendingGame;     // the same in game units, as written to his position
    // (walls) this step's walk alone (game units) and his position right after it: his logic's collision may stop his
    // body short of it (taken). Only standing: walking with the stick moves him by the game's own amount too
    Vec3 walkGame, walkFrom;
    bool walkStanding = false;
    bool moving = false;  // (log: a catch-up longer than a step's limit)
    // this step Link was turned with the head about his eyes (room_scale_move): his eye point is not held through the
    // turn (link_executed), and `facing` is the facing he was given (a turn of the game's own on top is still held)
    bool aboutEyes = false;
    float facing = 0;
};

// (diagnostics, the tracking summary) standing walks, those not all taken, how far (game units), views held at his back
struct HeadStats {
    uint32_t walks = 0, shortWalks = 0, backHeld = 0, floorHeld = 0;
    float wanted = 0, taken = 0;
};

class HeadController {
public:
    // ---- reading
    const WorldMapping& mapping() const { return map_; }
    const RoomScale& room() const { return room_; }
    bool calibrated() const { return calibrated_; }
    bool recenter_requested() const { return recenter_; }
    uint32_t calibrated_link() const { return calibratedLink_; }
    // recentred for this Link, and no recentre asked for
    bool calibrated_for(uint32_t link) const { return calibrated_ && !recenter_ && calibratedLink_ == link; }
    // the stick's turns so far (radians): Link's drawn body turns by them between steps (frame_correct)
    float turn_total() const { return turnTotal_; }
    // metres the view may lean from the origin until the next step (walls); no wall: a large number
    float wall_lean() const { return wallLean_; }
    const RigTurn& rig_turn() const { return rig_; }
    HeadStats take_stats() {
        const HeadStats s = stats_;
        stats_ = {};
        return s;
    }

    // ---- the recentre: the head stands at Link's eyes, facing his heading (the room-scale walk starts again)
    void request_recenter() { recenter_ = true; }
    void recenter(const Pose& head, float linkHeading, uint32_t link) {
        map_.origin = head.position;
        room_ = {};
        room_.base = head.position;
        map_.heading = linkHeading - yaw_of(head.orientation);
        calibrated_ = true;
        recenter_ = false;
        calibratedLink_ = link;
    }

    // ---- turns: the only writers of the heading besides the recentre
    // a stick turn (smooth, a frame's share; snap): Link's body turns with it (turn_total)
    void turn(float radians) {
        map_.heading = wrap_radians(map_.heading + radians);
        turnTotal_ += radians;
    }
    // the turn to Link on a ladder or ledge and during a lock-on: its target set at the pad read, advanced each frame
    void set_rig_turn(const RigTurn& r) { rig_ = r; }
    void stop_rig_turn() { rig_.active = false; }
    bool advance_rig_turn(float dt) {
        if (!rig_.active) return false;
        map_.heading = rig_.update(map_.heading, dt);
        return true;
    }

    // ---- the room-scale origin (OpenXR metres) for a frame at the step fraction t, and now at a step
    Vec3 origin(float t) const { return room_.base + (t < 1.0f ? lerp(room_.prev, room_.cur, t) : room_.cur); }
    Vec3 origin_now() const { return room_.base + room_.cur + room_.pendingXr; }
    // a new Link: what was pending taken at once (not blended: his position is not either)
    void step_new_link() {
        room_.cur = room_.cur + room_.pendingXr;
        room_.prev = room_.cur;
        room_.pendingXr = room_.pendingGame = {};
    }
    // a logic step done (Link's position after it): the origin of the last two steps, this step's walk by the share he
    // took (walls). Returns what the room scale moved him this step (game units)
    Vec3 step(Vec3 pos) {
        room_.prev = room_.cur;
        room_.cur = room_.cur + room_.pendingXr * taken(pos);
        return take_pending();
    }
    // the same step again (true 60's preview): only the latest position. Returns the further move
    Vec3 step_again(Vec3 pos) {
        room_.cur = room_.cur + room_.pendingXr * taken(pos);
        return take_pending();
    }
    // before Link's logic (room_scale_move): nothing turned about his eyes yet, no wall known
    void begin_move() {
        room_.aboutEyes = false;
        wallLean_ = 1e9f;
    }
    // turned with the head about his eyes: his position moved by `about` (game units), his facing `facing`
    void turned_about_eyes(Vec3 about, float facing) {
        room_.pendingGame = room_.pendingGame + about;
        room_.aboutEyes = true;
        room_.facing = facing;
    }
    // (link_executed) whether he was turned about his eyes this step, and the facing he was given; cleared
    bool take_turned_about_eyes(float& facing) {
        const bool was = room_.aboutEyes;
        room_.aboutEyes = false;
        facing = room_.facing;
        return was;
    }
    // the head's walk this step: `xr` metres in the play space, `game` units to his position (from `from` after it)
    void walk(Vec3 xr, Vec3 game, Vec3 from, bool standing) {
        room_.walkGame = game;
        room_.walkFrom = from;
        room_.walkStanding = standing;
        room_.pendingXr = room_.pendingXr + xr;
        room_.pendingGame = room_.pendingGame + game;
    }
    // a catch-up longer than a step's limit begins or ends: true when it changed (the log)
    bool set_catching_up(bool catchingUp) {
        const bool changed = catchingUp != room_.moving;
        room_.moving = catchingUp;
        return changed;
    }
    void set_wall_lean(float metres) { wallLean_ = metres; }

    // ---- the head a frame is drawn with
    // First-person play: the tracking space anchored at Link's eyes (`eyes`, blended like his model), the origin blended
    // the same way (t), the head within the lean limit and the walls' (gameplay_head), and leaning back at most
    // leanBack game units behind his eyes along his drawn facing (hold_lean_back; leanBack 0: no such limit), and never
    // lower than `floor` (the lowest the view may be: hold_above_floor; -infinity: no such limit)
    Pose first_person(const Pose& head, Vec3 eyes, float facing, float t, float heightOffset, float maxHeadOffset,
                      float scale, float leanBack, float floor = -INFINITY) {
        map_.origin = origin(t);
        Pose h = gameplay_head(map_, eyes, head, heightOffset, std::min(maxHeadOffset, wallLean_), scale);
        if (leanBack > 0 && hold_lean_back(h.position, eyes, facing, leanBack)) ++stats_.backHeld;
        if (hold_above_floor(h.position, floor)) ++stats_.floorHeld;
        return h;
    }
    // The same head at a logic step, for what is aimed at it during his logic (the arms: link_arms): the origin as it
    // is now with this step's room-scale walk (origin_now), Link's eyes at the step's position; nothing is changed
    Pose step_head(const Pose& head, Vec3 eyes, float heightOffset, float maxHeadOffset, float scale) const {
        WorldMapping m = map_;
        m.origin = origin_now();
        return gameplay_head(m, eyes, head, heightOffset, maxHeadOffset, scale);
    }
    // Third person, events, cutscenes, the title and file select: the game's camera at `eye` (looking along center - eye,
    // up), turned by the head relative to where it looked when this camera began (begin_game_camera); its position the
    // game's, the head's own is not added
    void begin_game_camera(float headYaw) {
        gameCamera_ = true;
        gameCameraYaw_ = headYaw;
        boatView_ = 0;
    }
    bool game_camera() const { return gameCamera_; }
    Pose game_camera_view(Vec3 eye, Vec3 center, Vec3 up, Quat head) const {
        const Quat camera = look_rotation(center - eye, up);
        return {normalize(camera * normalize(yaw_quat(-gameCameraYaw_) * head)), eye};
    }
    // first person again after the game's camera: true when it was the game's (the caller recentres: the event may have
    // moved Link, and you may have walked or turned in the room meanwhile)
    bool end_game_camera() {
        const bool was = gameCamera_ || boatView_;
        gameCamera_ = false;
        boatView_ = 0;
        return was;
    }
    // One of the boat's views (vr_sail_view.h, docs/VR_SAILING_VIEWS.md): the tracking space stands at a point on the
    // boat (`anchor`, game units, blended with the boat as drawn) facing `heading` (the boat's heading plus the view's,
    // game radians), your head where it was when the view began (begin_boat_view) at the anchor and looking along that
    // heading. The head's whole pose is added: its position (within maxHeadOffset metres, scaled) and its orientation,
    // untouched; only the boat's turn turns the view, never its pitch or roll (the horizon stays level). `view` names
    // which of the boat's views it is (nonzero); a new one begins from where the head is then
    void begin_boat_view(const Pose& head, int view) {
        boatView_ = view;
        boatOrigin_ = head.position;
        boatYaw_ = yaw_of(head.orientation);
        gameCamera_ = false;
    }
    int boat_view() const { return boatView_; }
    // the right stick's turn in a boat view (the neck view: as in first person, smooth or snap), about the anchor; the
    // first-person mapping is not touched. A new boat view begins unturned
    void boat_turn(float radians) { boatYaw_ = wrap_radians(boatYaw_ - radians); }
    Pose boat_view_pose(const Pose& head, Vec3 anchor, float heading, float maxHeadOffset, float scale) const {
        WorldMapping m;
        m.anchor = anchor;
        m.heading = wrap_radians(heading - boatYaw_);
        m.unitsPerMetre = scale;
        m.origin = boatOrigin_;
        const Vec3 offset = limit_head_offset(head.position - m.origin, maxHeadOffset);
        return {to_game_orientation(m, head.orientation), to_game_point(m, m.origin + offset)};
    }

    // ---- the views drawn, by display time (the screen blur: blur_head_turn)
    void begin_frame(int64_t displayTime) {
        if (displayTime == viewTime_) return;  // (a second pass of the same frame)
        prevView_ = curView_;
        havePrev_ = haveCur_;
        haveCur_ = false;
        viewTime_ = displayTime;
    }
    void drawn(Quat view) {
        curView_ = view;
        haveCur_ = true;
    }
    bool blur_reprojection(const Fov& f, float a[6]) const {
        return havePrev_ && haveCur_ && vr::blur_reprojection(prevView_, curView_, f, a);
    }

private:
    // (walls) the share of this step's room-scale walk Link took, from his position after his logic: his collision
    // cylinder (its centre about 20 units behind his eyes) stops his body before the eyes' line check would, and the
    // origin then moves only by what he took, so the rest of the lean stays in the view (towards the object, up to
    // kWallMargin from it: wall_lean_update) instead of the world sliding with the head
    float taken(Vec3 pos) {
        const float want = std::sqrt(room_.walkGame.x * room_.walkGame.x + room_.walkGame.z * room_.walkGame.z);
        const Vec3 from = room_.walkFrom, walk = room_.walkGame;
        room_.walkGame = {};
        if (!room_.walkStanding || !(want > 1e-3f) || !std::isfinite(from.x) || !std::isfinite(pos.x)) return 1.0f;
        const float along = ((pos.x - from.x) * walk.x + (pos.z - from.z) * walk.z) / want;
        const float k = std::clamp((want + along) / want, 0.0f, 1.0f);
        ++stats_.walks;
        stats_.shortWalks += k < 0.95f;
        stats_.wanted += want;
        stats_.taken += want * k;
        return k;
    }
    Vec3 take_pending() {
        const Vec3 push = room_.pendingGame;
        room_.pendingXr = room_.pendingGame = {};
        return push;
    }

    WorldMapping map_;
    RoomScale room_;
    bool calibrated_ = false, recenter_ = true;
    uint32_t calibratedLink_ = 0;
    float turnTotal_ = 0;
    float wallLean_ = 1e9f;
    RigTurn rig_;
    bool gameCamera_ = false;
    float gameCameraYaw_ = 0;
    int boatView_ = 0;  // the boat's view drawn (0: none), from where the head was when it began
    Vec3 boatOrigin_;
    float boatYaw_ = 0;
    Quat prevView_, curView_;
    int64_t viewTime_ = 0;
    bool havePrev_ = false, haveCur_ = false;
    HeadStats stats_;
};

}  // namespace vr
